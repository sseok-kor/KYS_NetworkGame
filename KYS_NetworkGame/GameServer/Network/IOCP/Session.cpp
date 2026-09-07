#include "pch_gameserver.h"
#include "Session.h"
#include "INetEventHandler.h"
#include "SessionPool.h"

// 송신 압력 계측은 전역 카운터에서 세션 인스턴스 멤버로 이관됐다(SSOT) - 세션 멤버가 m_sendLock 안 평문 갱신이라
//   전 세션 공유 캐시라인 hotspot(전역 Interlocked)이 소멸하고, 세션별/채널별 귀속과 상위 N 느린 세션 식별이 가능해진다.
//   총량 단조 복원은 Release 시 SessionPool retired 누적에 접어 활성 슬롯 합과 더한다(GetSendMetrics/GetRetiredSendTotals).

// 빈 세션 초기화 - 두 IOCPContext에 수신/송신 표식을 박아둠 (완료통지 때 종류 구분용).
KYS::GAMESERVER::NETWORK::Session::Session()
	: m_sock(INVALID_SOCKET)
	, m_sessionId(0)
	, m_generation(1)
    , m_handler(nullptr)
	, m_pool(nullptr)
	, m_sendQueue(SEND_BUFFER_SIZE)   // 명시 생성 - 없으면 RingBuffer 기본 RECV_BUFFER_SIZE로 생성돼 SEND_BUFFER_SIZE가 무시됨
	, m_sendInProgress(false)
	, m_sendDropCount(0)
	, m_sendDropBytes(0)
	, m_sendWrapCount(0)
	, m_wsaSendPostCount(0)
	, m_sendQueuePeakUsed(0)
	, m_sendQueueSampleSum(0)
	, m_sendQueueSampleCount(0)
	, m_sendWatermarkHits(0)
	, m_disconnecting(0)
	, m_pendingIoCount(0)
	, m_disconnectReason(EDisconnectReason::CLIENT_FIN)
	, m_remoteIp(0)
	, m_connectTick(0)
	, m_lastRecvTick(0)
{
	m_recvContext.m_opType = EOpType::RECV;
	m_sendContext.m_opType = EOpType::SEND;
}

// accept 직후 호출 - 게임에 새 연결을 알리고 첫 수신을 건다.
bool KYS::GAMESERVER::NETWORK::Session::BeginSession()
{
    m_connectTick = ::GetTickCount64();         // 연결 성립 시각
    m_lastRecvTick = m_connectTick;             // 아직 무수신 - 무수신 타임아웃을 연결 시점부터 잼
    m_handler->EnqueueConnect(m_sessionId);    // 게임측에 "새 연결" 통지
    return RegisterRecv();                          // 첫 수신 등록
}

// 보낼 바이트를 송신 큐에 넣고, 진행 중이 아니면 송신을 시작한다.
//   data : 보낼 바이트 시작 주소
//   size : 보낼 바이트 수
bool KYS::GAMESERVER::NETWORK::Session::Send(const BYTE* data, int size, ESendDropPolicy policy)
{
    if (m_disconnecting != 0) { return false; }   // 종료 진입한 세션엔 송신 생략 (lease가 안전을 보장하나 헛수고 방지, best-effort 읽기)
    bool queued = false;
    bool needDisconnect = false;
    bool postedIo = false;                                   // DoWSASendLocked 가 pendingIo 를 +1 했는가 (실패 시 되돌릴 몫은 그것뿐)
    EDisconnectReason reason = EDisconnectReason::SEND_FAIL;
    {
        SpinLockGuard guard(m_sendLock);

        if (!m_sendQueue.Enqueue((const char*)data, size))
        {
            ++m_sendDropCount;                        // 계측: 헤드룸 부족으로 배치 통째 drop
            m_sendDropBytes += (UINT64)size;
            if (policy == ESendDropPolicy::EVICT_ON_DROP)   // 이 스트림은 한 번 버리면 다시 못 맞춘다(호출자 판단) -> 넘친 그 자리에서 끊는다. 왜인지는 이 층이 모른다
            {
                needDisconnect = true;
                reason = EDisconnectReason::SEND_TIMEOUT;
            }
            // 실패 표본은 아래 계측(used/peak/watermark)을 안 지난다 - used < size 라 뺄셈이 언더플로하고 창평균 표본이 오염된다
        }
        else
        {
            queued = true;
            // 계측: Enqueue 성공 직후 큐 점유(used) 관측 (m_sendLock 안 - 원자연산 0). peak=생애 최대(sticky), sample=창평균 재료,
            //   watermarkHits=이번 enqueue 로 used 가 워터마크를 처음 넘긴 엣지(직전값 = used - size 재계산). 순수 관측(제어흐름 무변경).
            const UINT32 used = (UINT32)m_sendQueue.GetUsedSize();
            if (used > m_sendQueuePeakUsed) { m_sendQueuePeakUsed = used; }
            m_sendQueueSampleSum += used;
            ++m_sendQueueSampleCount;
            const UINT32 watermark = (UINT32)SEND_WATERMARK;
            if (used >= watermark && (used - (UINT32)size) < watermark)   // 직전값(used-size)은 워터마크 미만, 지금(used)은 이상 = 상향돌파 엣지 1회
            {
                ++m_sendWatermarkHits;
            }

            if (!m_sendInProgress)
            {
                m_sendInProgress = true;
                if (!DoWSASendLocked())   // 동기 실패 시 막힘 방지 (다음 Send가 재시도 가능)
                {
                    m_sendInProgress = false;
                    needDisconnect = true;
                    postedIo = true;
                }
            }
        }
    }
    if (needDisconnect)
    {
        Disconnect(reason);                         // 락 밖에서 (closesocket을 락 보유 중 호출 금지). SEND_TIMEOUT 이면 Disconnect 가 CAS 통과 뒤 풀의 축출 카운터를 1회 올린다
        if (postedIo) { DecPendingIo(); }           // WSASend 가 +1 한 것만 되돌린다 (0이면 자가 반납) - 큐 넘침은 +1 한 적이 없다
        return false;
    }
    return queued;
}

// 받은 바이트를 버퍼에 쌓고, 길이 규약으로 완성 패킷을 잘라 게임에 넘긴 뒤 다음 수신을 건다.
//   transferred : 이번에 받은 바이트 수 (0이면 상대가 종료)
//   ok          : 완료 성공 여부 (실패면 강제 끊김)
void KYS::GAMESERVER::NETWORK::Session::OnRecvComplete(int transferred, BOOL ok)
{
    if (transferred == 0)                   // 0바이트 = 상대가 정상 종료(FIN)
    {
        Disconnect(ok ? EDisconnectReason::CLIENT_FIN : EDisconnectReason::NET_RESET);   // ok면 정상 종료, 아니면 강제 끊김
        return;
    }

    m_recvBuffer.MoveWritePos(transferred);
    m_lastRecvTick = ::GetTickCount64();    // 수신 갱신 - 이 연결은 살아있음(무수신 타임아웃 리셋)



    while (m_recvBuffer.GetUsedSize() >= LENGTH_PREFIX_SIZE)
    {
        UINT16 totalLen = ReadPacketLength();

        if (totalLen < LENGTH_PREFIX_SIZE || totalLen > RECV_BUFFER_SIZE)   // 헤더보다 작거나, 수신버퍼보다 커서 이 버퍼에 다 못 담겨 영원히 미완성될 패킷 = 규약 위반
        {
            Disconnect(EDisconnectReason::RECV_FAIL);
            return;
        }

        if (m_recvBuffer.GetUsedSize() < totalLen)
        {
            break;                                                       // 본문이 아직 다 안 옴 - 다음 수신 기다림
        }

        DeliverPacket(totalLen);

    }
    RegisterRecv();                         // 다음 수신 재게시
    // 어느 채널인지/패킷 종류 해석은 여기 아님 - 게임측 처리(OnRecvJob, Channel 스레드)
}

// 길이 규약 2B를 읽어 패킷 전체 길이를 돌려준다 - 앞 2B가 연속이면 바로, 버퍼 끝에서 갈리면 이어붙여서.
UINT16 KYS::GAMESERVER::NETWORK::Session::ReadPacketLength()
{
    UINT16 totalLen;

    if (m_recvBuffer.GetDirectDequeueSize() >= LENGTH_PREFIX_SIZE)
    {
        totalLen = ntohs(*(UINT16*)m_recvBuffer.GetReadPtr());        // 앞 2B가 연속이라 바로 읽음
    }
    else
    {
        BYTE hdr[LENGTH_PREFIX_SIZE];                                 // 길이 2B가 버퍼 끝에서 갈림
        m_recvBuffer.Peek((char*)hdr, LENGTH_PREFIX_SIZE);            // 갈린 2B를 이어붙여 복사
        totalLen = ntohs(*(UINT16*)hdr);
    }
    return totalLen;
}

// 완성된 한 패킷을 게임에 통지하고 읽기 위치를 그만큼 전진시킨다 - 연속이면 zero-copy, 버퍼 끝에 걸치면 임시 버퍼로 모아서.
void KYS::GAMESERVER::NETWORK::Session::DeliverPacket(UINT16 totalLen)
{
    // 완성된 한 패킷 통지 - 연속이면 그 자리에서(zero-copy), 버퍼 끝에 걸치면 임시 버퍼로 모아서
    if (m_recvBuffer.GetDirectDequeueSize() >= totalLen)
    {
        m_handler->EnqueuePacket(m_sessionId, (const BYTE*)m_recvBuffer.GetReadPtr(),totalLen);
    }
    else
    {
        // 64KB 큰 버퍼를 스택에 두지 않으려고 Worker 스레드마다 하나씩 재사용.
        static thread_local BYTE s_packetBuffer[MAX_PACKET_SIZE];
        m_recvBuffer.Peek((char*)s_packetBuffer, totalLen);               // 갈린 패킷을 이어붙여 복사
        m_handler->EnqueuePacket(m_sessionId,s_packetBuffer,totalLen);
    }
    m_recvBuffer.MoveReadPos(totalLen);                              // 처리한 패킷만큼 읽기 위치 전진
}

// 보낸 만큼 큐를 비우고, 남은 게 있으면 이어서 송신한다. 완료 자체가 에러면 종료 처리.
//   transferred : 이번에 보낸 바이트 수
//   ok          : 완료 성공 여부
void KYS::GAMESERVER::NETWORK::Session::OnSendComplete(int transferred, BOOL ok)
{
    bool completionError = false;   // 완료 자체가 에러(!ok) - 이 send의 -1은 worker가 나중에 처리
    bool chainSendFailed = false;   // 이어서 시작한 새 send가 동기 실패 - 그 +1을 여기서 되돌림
    {
        SpinLockGuard guard(m_sendLock);

        if (!ok)
        {
            m_sendInProgress = false;
            completionError = true;   // 송신 완료 에러 = 연결 깨짐
        }
        else
        {
            m_sendQueue.MoveReadPos(transferred);

            if (m_sendQueue.GetUsedSize() > 0)
            {
                if (!DoWSASendLocked())
                {
                    m_sendInProgress = false;
                    chainSendFailed = true;
                }
            }
            else
            {
                m_sendInProgress = false;
            }
        }
    }
    if (completionError)
    {
        Disconnect(EDisconnectReason::SEND_FAIL);   // 락 밖에서 (완료 send의 -1은 worker가 처리)
    }
    if (chainSendFailed)
    {
        Disconnect(EDisconnectReason::SEND_FAIL);   // 락 밖에서
        DecPendingIo();                             // 이어서 시작한 새 send의 +1 되돌림 (완료 send와 별개)
    }
}

// 다음 수신을 건다 - 빈 버퍼 영역에 WSARecv를 비동기로 게시.
bool KYS::GAMESERVER::NETWORK::Session::RegisterRecv()
{
    m_recvContext.Reset();
    m_recvContext.m_opType = EOpType::RECV;

    WSABUF wsaBuf;
    wsaBuf.buf = m_recvBuffer.GetWritePtr();
    wsaBuf.len = m_recvBuffer.GetDirectEnqueueSize();

    InterlockedIncrement(&m_pendingIoCount);           // post 직전 +1
    DWORD flags = 0;

    int ret = WSARecv(m_sock, &wsaBuf, 1, NULL, &flags,
        &m_recvContext.m_overlapped, NULL);

    int wsaErr = (ret == SOCKET_ERROR) ? WSAGetLastError() : 0;
    if (ret == SOCKET_ERROR && wsaErr != WSA_IO_PENDING)
    {
        Disconnect(EDisconnectReason::RECV_FAIL);
        DecPendingIo();   // 실패한 수신등록의 +1 되돌리고 0이면 자가 반납 (두 번 반납돼도 풀이 무해 처리)
        return false;
    }

    return true;
}

// 종료를 한 번만 통과시키고, 떠 있는 I/O를 취소한 뒤 소켓을 닫는다.
//   reason : 종료 사유 (정상 종료냐 강제 끊김이냐로 LINGER 결정)
void KYS::GAMESERVER::NETWORK::Session::Disconnect(EDisconnectReason reason)
{
    if (InterlockedCompareExchange(&m_disconnecting, 1, 0) != 0)
    {
        return;                             // 중복 호출 중 하나만 통과
    }
    if (reason == EDisconnectReason::SEND_TIMEOUT && m_pool != nullptr) { m_pool->NoteSendDropEvict(); }   // 송신 압력 축출 집계 - CAS 를 통과한 이 한 경로만 세므로 세션당 정확히 1회. 소켓 유무보다 앞(소켓 없는 유닛 테스트도 센다)

    // m_sock을 읽으면서 동시에 INVALID로 교체한다(atomic). 종료 책임을 한 경로가 독점하게 만든다.
    //   CancelIoEx가 떠 있는 I/O를 취소하면 그 완료통지를 받은 worker가
    //   DecPendingIo->Release->Reset(m_sock=INVALID, 슬롯 재사용)을 이 함수 진행 중에 끼워넣을 수 있다.
    //   비원자적 복사(const SOCKET sock = m_sock)는 CAS~복사 사이에 그 Reset이 끼면 INVALID를 잡아
    //   setsockopt/closesocket이 무효 소켓에 실행되고 RST가 누락된다. 원자적 교체는 그 틈을 없애고,
    //   옛 소켓값을 잡은 경로만 종료를 수행하므로 항상 이 연결에 RST가 나간다.
    const SOCKET sock = reinterpret_cast<SOCKET>(
        ::InterlockedExchangePointer(reinterpret_cast<volatile PVOID*>(&m_sock), reinterpret_cast<PVOID>(INVALID_SOCKET)));
    if (sock == INVALID_SOCKET)
    {
        return;   // CAS~교체 극미세 틈에 worker가 먼저 Reset함 - 닫을 소켓 없음 (방어)
    }

    m_disconnectReason = reason;
    CancelIoEx((HANDLE)sock, nullptr);      // 떠 있는 I/O 취소 (독점한 소켓)

    linger lin = IsAbnormal(reason)
                  ? linger{ 1, 0 }            // 즉시 RST로 강제 끊기 (PROTOCOL_VIOLATION 포함 - IsAbnormal SSOT)
                  : linger{ 0, 0 };           // OS 기본 정상 종료(FIN)
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (const char*)&lin, sizeof(lin));
    closesocket(sock);                      // 위 LINGER 옵션에 따라 RST 또는 FIN 전송
    // 이후 떠 있던 완료통지들이 DecPendingIo로 모이고, 0을 만든 경로가 종료 통지 + 자가 반납

}

// 떠 있는 I/O를 하나 줄이고, 0이 되면 게임에 종료를 알린다 (모든 -1 경로가 여기로 모임, 마지막에 0 만든 쪽이 처리).
void KYS::GAMESERVER::NETWORK::Session::DecPendingIo()
{
    LONG after = InterlockedDecrement(&m_pendingIoCount);
    if (after == 0)
    {
        m_handler->EnqueueDisconnect(m_sessionId, m_disconnectReason);   // 게임측에 종료 통지 (socket 수는 SessionPool 활성 슬롯에서 따로 셈)
        m_pool->Release(m_sessionId);       // 자기 슬롯을 풀에 반납 (반납 후 this 접근 금지). 소켓 풀만 회수, 게임세션은 게임측이 따로
    }
}

// SessionPool::FindAndLease가 pool lock 안에서 호출 - 게임 스레드가 빌려 쓰는 동안 살려두려고 IO 카운트를 올린다.
//   증가 후 값을 돌려준다 - ==1이면 떠 있는 IO가 없던 세션(teardown 진행 중)이라 호출자가 AbortIoLease로 되돌린다.
LONG KYS::GAMESERVER::NETWORK::Session::AcquireIoLease()
{
    return InterlockedIncrement(&m_pendingIoCount);
}

// FindAndLease after==1 경로 전용(pool lock 보유 중) - lease를 되돌린다. raw 감소만 - 종료(EnqueueDisconnect/Release)는
//   이미 worker가 진행 중이라 여기서 트리거하면 이중 통지가 된다. 그래서 DecPendingIo가 아닌 순수 감소.
void KYS::GAMESERVER::NETWORK::Session::AbortIoLease()
{
    InterlockedDecrement(&m_pendingIoCount);
}

// SessionPool::Allocate가 호출 - 세션 식별자를 박는다.
void KYS::GAMESERVER::NETWORK::Session::Init(UINT64 sessionId)
{
    m_sessionId = sessionId;                // 세션 식별자 세팅 (generation은 sid 안에 들어 있음)
}

// SessionPool::Release가 호출 - 세대를 올리고 모든 상태를 초기화한다.
void KYS::GAMESERVER::NETWORK::Session::Reset()
{
    ++m_generation;                         // 세대 올려 옛 핸들 무효화 (use-after-free 차단)
    if (m_generation == 0)                  // 0은 무효값이라 건너뜀
    {
        m_generation = 1;
    }
    m_sock = INVALID_SOCKET;
    m_sendInProgress = false;
    m_disconnecting = 0;
    InterlockedExchange(&m_pendingIoCount, 0);   // 평문 store 대신 Interlocked - 다른 스레드의 Interlocked inc/dec와의 data race 위생 (lease 모델 정합)
    m_disconnectReason = EDisconnectReason::CLIENT_FIN;
    m_remoteIp = 0;                         // 회수 슬롯 stale 연결 메타 봉인
    m_connectTick = 0;
    m_lastRecvTick = 0;
    m_sendQueue.Clear();                    // 버퍼 상태 초기화
    m_recvBuffer.Clear();
    m_sendDropCount = 0;                     // 회수 슬롯 stale 송신 계측 봉인 - 재사용 세션이 옛 drop/peak를 상속(가짜 범인)하지 않게
    m_sendDropBytes = 0;
    m_sendWrapCount = 0;
    m_wsaSendPostCount = 0;
    m_sendQueuePeakUsed = 0;
    m_sendQueueSampleSum = 0;
    m_sendQueueSampleCount = 0;
    m_sendWatermarkHits = 0;
}

// m_sendLock 보유 중 호출 - 송신 큐의 연속 영역 하나를 WSASend로 게시한다.
bool KYS::GAMESERVER::NETWORK::Session::DoWSASendLocked()
{
    WSABUF wsaBuf;
    wsaBuf.buf = m_sendQueue.GetReadPtr();
    wsaBuf.len = m_sendQueue.GetDirectDequeueSize();   // 버퍼 끝에서 갈리기 직전까지 연속 영역

    if (m_sendQueue.GetUsedSize() > m_sendQueue.GetDirectDequeueSize())   // 계측: 되감긴 꼬리가 남음(연속분만 나가고 잔여는 다음 completion으로) - scatter-gather 실이익 추정
    {
        ++m_sendWrapCount;   // m_sendLock 보유 중 - 평문
    }

    m_sendContext.Reset();
    m_sendContext.m_opType = EOpType::SEND;
    InterlockedIncrement(&m_pendingIoCount);           // post 직전 +1
    ++m_wsaSendPostCount;                               // 계측: 실 WSASend 게시 카운트 (m_sendLock 보유 중 - 평문, coalescing 확인)

    int ret = WSASend(m_sock, &wsaBuf, 1, NULL, 0,
        &m_sendContext.m_overlapped, NULL);
    int wsaErr = (ret == SOCKET_ERROR) ? WSAGetLastError() : 0;
    if (ret == SOCKET_ERROR && wsaErr != WSA_IO_PENDING)
    {
        return false;   // 동기 실패 - +1 그대로 두고, 호출자가 락 밖에서 DecPendingIo로 되돌림 (0이면 반납)
    }
    return true;
}

// 무음 소켓 회수 판정 - 마지막 수신(없으면 연결 시점) 후 deadline을 넘도록 아무것도 안 받았으면 true.
//   sweep 스레드가 락 없이 근사 호출 - 실제 종료는 FindAndLease(세대 검증) 경로라 오판정도 안전하게 걸러진다.
//   도메인 무지: 인증 여부를 모르고 순수 I/O(무수신)만 본다.
bool KYS::GAMESERVER::NETWORK::Session::IsIdleTimedOut(UINT64 nowTick, UINT64 deadlineMs) const
{
    const UINT64 last = m_lastRecvTick;   // 한 번만 읽어 스냅샷 - 워커(OnRecvComplete)가 사이에 갱신해도 가드와 뺄셈이 같은 값을 봐 split-load 언더플로 없음
    if (last == 0) { return false; }        // 미개시 슬롯(Reset 0, Allocate~BeginSession 사이) - 회수 대상 아님
    if (nowTick <= last) { return false; }  // sweep이 찍은 nowTick 이후에 수신이 들어온 슬롯 - unsigned 언더플로 방지(활성 세션 오회수 차단)
    return (nowTick - last) > deadlineMs;    // 마지막 수신 후 deadline 초과 무수신 -> 회수 후보
}

// 송신 압력 스냅샷을 복사한다 - 무락 근사(모니터 1Hz pull 이 Send 핫패스를 방해하지 않게).
//   자유/재사용 슬롯은 sid=0 으로 표시한다: m_sessionId 의 세대와 현재 m_generation 이 어긋나면(Reset 이 ++세대) 라이브가 아니다.
//   이렇게 해야 모니터가 옛 세션의 큰 sampleCount 값으로 창평균을 이어(언더플로/오평균) 계산하지 않고 baseline 을 리셋한다.
void KYS::GAMESERVER::NETWORK::Session::GetSendMetrics(SessionSendMetrics& out) const
{
    const UINT32 gen = GetGenerationAcquire();          // 슬롯 세대 acquire 읽기 (FindLockFree 선례)
    const UINT64 sid = m_sessionId;
    out.sid = ((sid & 0xFFFFFFFFull) == gen) ? sid : 0; // 세대 일치 = 라이브 슬롯만 sid 노출 (불일치=자유/재사용 슬롯)
    out.dropCount     = m_sendDropCount;
    out.dropBytes     = m_sendDropBytes;
    out.wrapCount     = m_sendWrapCount;
    out.wsaPostCount  = m_wsaSendPostCount;
    out.peakUsed      = m_sendQueuePeakUsed;
    out.sampleSum     = m_sendQueueSampleSum;
    out.sampleCount   = m_sendQueueSampleCount;
    out.watermarkHits = m_sendWatermarkHits;
    out.capacity      = (UINT32)m_sendQueue.GetCapacity();
}
