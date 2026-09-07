#pragma once
#include "IOCPContext.h"
#include "../GameServer/Types/Defines.h"
#include "../GameServer/Core/Thread/SpinLock.h"
#include "../GameServer/Core/Thread/MemoryOrder.h"
#include "../GameServer/Core/Memory/RingBuffer.h"



using namespace KYS::GAMESERVER::MEMORY;
using namespace KYS::GAMESERVER::THREAD;

namespace KYS
{
	namespace GAMESERVER
	{
        namespace NETWORK
        {
            class INetEventHandler;
            class SessionPool;


            // 세션 한 개의 송신 압력 스냅샷 (전부 raw 숫자 - 도메인 의미 0). SnapshotSendMetrics 가 슬롯당 하나씩 채운다.
            //   drop/wsaPost/wrap = 누적 카운터, peak/sample = 큐 점유 게이지, watermarkHits = 워터마크 상향돌파 횟수.
            //   정렬/랭킹/accountId 라벨/창평균 계산은 라이브러리가 아니라 게임측 모니터가 한다(도메인 무관 유지).
            struct SessionSendMetrics
            {
                UINT64 sid;            // 이 슬롯의 현재 세션 핸들 (자유/재사용 슬롯이면 0 - 모니터가 창평균 baseline 리셋에 사용)
                UINT32 dropCount;      // Enqueue 실패(배치 통째 drop) 횟수
                UINT64 dropBytes;      // drop 된 바이트 누적 (UINT32 는 ~2.4h 만에 wrap 이라 64비트)
                UINT32 wrapCount;      // RingBuffer wrap 으로 연속분만 보낸 횟수
                UINT32 wsaPostCount;   // 실 WSASend 게시 횟수 (coalescing 확인)
                UINT32 peakUsed;       // 연결 생애 최대 큐 점유 바이트 (sticky high-water)
                UINT64 sampleSum;      // Enqueue 성공 시점 used 합 (창평균 분자 - send-사건가중)
                UINT32 sampleCount;    // Enqueue 성공 표본 수 (창평균 분모)
                UINT32 watermarkHits;  // used 가 SEND_WATERMARK 를 처음 넘긴 횟수 (백프레셔 조기신호)
                UINT32 capacity;       // 송신 큐 전체 바이트 (peak% 분모)
            };

            // 풀 수준 은퇴(retired) 송신 누적 - 세션이 Release 되며 Reset 0화되기 직전 접은 합. 활성 슬롯 합과 더해 단조 총량 복원.
            struct RetiredSendTotals
            {
                UINT64 dropCount;
                UINT64 dropBytes;
                UINT64 wrapCount;
                UINT64 wsaPostCount;
                UINT64 watermarkHits;   // 워터마크 상향돌파 누적 (export 총량 단조성 - 이탈 세션 분 증발 방지)
            };


            // 소켓 하나의 자기 I/O 담당 - 받은 바이트를 패킷 경계로 잘라 게임에 넘기고, 보낼 바이트는 큐에 모아 이어서 송신. 풀 슬롯으로 재사용된다.
            class Session
            {
            public:
                Session();
                ~Session()=default;

                Session(const Session&) = delete;
                Session& operator=(const Session&) = delete;
                Session(Session&&) = delete;
                Session& operator=(Session&&) = delete;

                // 연결 / 송수신 / 종료 (자기 I/O)
                bool BeginSession();                                           // accept 직후 - 게임에 연결 통지 + 첫 수신 등록
                bool Send(const BYTE* data, int size, ESendDropPolicy policy);   // 큐 적재 성공 = true. 큐 넘침 + EVICT_ON_DROP 이면 여기서 Disconnect(SEND_TIMEOUT) - 처분은 원인이 나는 이 층, 이유는 호출자 몫
                void OnRecvComplete(int transferred, BOOL ok);                 // 받은 바이트 쌓고 -> 길이 규약으로 패킷 경계 잘라 게임 통지 -> 다음 수신 재게시
                void OnSendComplete(int transferred, BOOL ok);                 // 보낸 만큼 큐 비우고 -> 남았으면 이어서 송신
                bool RegisterRecv();                                           // 다음 수신 등록 (자기 m_sock/m_recvContext로 WSARecv)
                void Disconnect(EDisconnectReason reason);                     // 종료 한 번만 통과(CAS) -> 떠 있는 I/O 취소 -> 소켓 닫기
                void DecPendingIo();                                           // 떠 있는 I/O -1, 0이면 게임에 종료 통지 + 자기 풀 슬롯 반납

                // I/O lease (SessionPool::FindAndLease가 pool lock 안에서 호출) - 게임 스레드가 세션을 빌려 쓰는 동안 살려둔다.
                LONG AcquireIoLease();                                         // pendingIoCount +1, 증가 후 값 반환 (==1이면 떠 있는 IO 없던 세션=teardown 중)
                void AbortIoLease();                                           // lease 되돌림 (raw -1, 종료 트리거 안 함) - FindAndLease after==1 경로 전용(pool lock 보유 중)

                // 풀 생명주기 연동 (SessionPool이 호출)
                void Init(UINT64 sessionId);                                   // Allocate 시 - 세션 식별자 세팅
                void Reset();                                                  // Release 시 - generation++ + 상태 초기화
                void SetSocket(SOCKET sock) { m_sock = sock; }                 // accept된 소켓 주입
                void SetSink(INetEventHandler* sink) { m_handler = sink; }     // 게임측 통지 통로 주입
                void SetPool(SessionPool* pool) { m_pool = pool; }             // 자기 풀 back-pointer 주입 (자가 반납용)
                void SetRemoteIp(UINT32 ip) { m_remoteIp = ip; }               // accept 경로가 접속자 IPv4 주입 (진단/로깅용 전송 사실)

                // 접근자
                UINT32 GetGeneration() const { return m_generation; }          // 슬롯 세대 (m_lock 보유 경로의 핸들 발급/검증용)
                UINT32 GetGenerationAcquire() const { return LoadAcquire(const_cast<volatile UINT32*>(&m_generation)); }   // worker가 락 없이 세대 검증 (acquire 읽기로 재배열 차단, x86 TSO)
                UINT64 GetSessionId() const { return m_sessionId; }            // 세션 식별자
                UINT32 GetRemoteIp() const { return m_remoteIp; }              // 접속자 IPv4 (network byte order)
                UINT64 GetConnectTick() const { return m_connectTick; }        // 연결 성립 시각 (연결 나이 진단용)
                bool   IsIdleTimedOut(UINT64 nowTick, UINT64 deadlineMs) const;   // 마지막 수신 후 deadline 초과 무수신이면 true (무음 소켓 회수 판정, 인증 무지)

                void   GetSendMetrics(SessionSendMetrics& out) const;          // 송신 압력 스냅샷 복사 (무락 근사 - 모니터 1Hz pull). 자유/재사용 슬롯은 sid=0 표시
                // (송신 실패의 원인을 밖에서 되짚는 접근자는 두지 않는다 - 큐 넘침의 처분은 Send 의 policy 인자로 이 안에서 끝낸다. 옛 GetSendDropCountValue 는 2026-09-02 에 소비처 0 으로 제거)

            private:
                bool DoWSASendLocked();  // m_sendLock 보유 중 호출 - 큐의 연속 영역 하나를 WSASend (zero-copy WSABUF)
                UINT16 ReadPacketLength();               // OnRecvComplete - 길이 규약 2B를 읽어 패킷 전체 길이 반환 (연속이면 바로, 끝에 갈리면 이어붙여)
                void   DeliverPacket(UINT16 totalLen);   // OnRecvComplete - 완성된 한 패킷을 게임에 통지하고 읽기 위치 전진


                // 식별
                SOCKET            m_sock;                // 이 세션의 소켓 핸들 (I/O 대상)
                UINT64            m_sessionId;           // 세션 식별자 (idx<<32 | generation)
                UINT32            m_generation;          // 슬롯 세대 (Release마다 ++, 옛 핸들 무효화)
                INetEventHandler* m_handler;             // 게임측 통지 통로 (EnqueueConnect/Packet/Disconnect 호출)
                SessionPool*      m_pool;                // 자기 풀 back-pointer (DecPendingIo가 0이면 여기에 반납)

                // 연결 메타 (전송 사실 - 도메인 무관: IP/연결시각/마지막수신시각)
                UINT32            m_remoteIp;            // 접속자 IPv4 (accept 캡처, network byte order) - 진단/로깅
                UINT64            m_connectTick;         // 연결 성립 시각 (GetTickCount64) - 연결 나이 진단
                UINT64            m_lastRecvTick;        // 마지막 수신 시각 (init=connectTick, 수신마다 갱신) - 무수신 타임아웃 기준


                // Send
                SpinLock          m_sendLock;            // 송신 큐/진행 플래그 보호
                RingBuffer        m_sendQueue;           // 보낼 바이트 모음 (WSASend 대상)
                bool              m_sendInProgress;       // 지금 WSASend 진행 중인지 (이어서 송신 판단)

                // 송신 압력 계측 - 전부 m_sendLock 임계 안에서만 write(평문 ++/max, 추가 원자연산 0). read 는 근사(무락). Reset 이 전량 0화.
                UINT32            m_sendDropCount;         // Enqueue 실패(배치 통째 drop) 횟수
                UINT64            m_sendDropBytes;         // drop 된 바이트 누적 (64비트 - UINT32 는 고부하 시 wrap)
                UINT32            m_sendWrapCount;         // wrap 으로 연속분만 보낸 횟수
                UINT32            m_wsaSendPostCount;      // 실 WSASend 게시 횟수 (coalescing 확인)
                UINT32            m_sendQueuePeakUsed;     // 연결 생애 최대 큐 점유 (sticky - Reset 0화)
                UINT64            m_sendQueueSampleSum;    // Enqueue 성공 시 used 합 (창평균 분자 - send-사건가중)
                UINT32            m_sendQueueSampleCount;  // Enqueue 성공 표본 수 (창평균 분모)
                UINT32            m_sendWatermarkHits;     // used 가 SEND_WATERMARK 를 상향돌파한 횟수 (직전값 재계산으로 계수 - 별도 멤버 불요)

                // Recv
                RingBuffer        m_recvBuffer;          // 받은 바이트 쌓임 (Worker 한 곳만 기록)

                // Disconnect
                LONG              m_disconnecting;       // 종료 진입 1회만 통과 (CAS)
                LONG              m_pendingIoCount;      // 떠 있는 I/O 수 (recv 1 + send 0~1, 0되면 종료 확정)
                EDisconnectReason m_disconnectReason;    // 종료 사유 (게임 통지에 실음)

                // IOCPContext
                IOCPContext       m_recvContext;         // 수신 overlapped (생성자에서 m_opType = RECV)
                IOCPContext       m_sendContext;         // 송신 overlapped (생성자에서 m_opType = SEND)
            };

            // FindAndLease로 빌린 세션의 lease를 스코프 종료 시 정확히 1회 반납하는 RAII 가드.
            //   복사/이동 금지(자원 단일 소유) - lease 누락(슬롯 영구 점유)을 한 곳에서 봉인한다.
            //   dtor의 DecPendingIo가 0을 만들면 그 자리에서 종료 통지+풀 반납이 일어난다(게임 스레드 발). 반납 후 m_session 미접근.
            class SessionLeaseGuard
            {
            public:
                explicit SessionLeaseGuard(Session* s) : m_session(s) {}
                ~SessionLeaseGuard() { if (m_session != nullptr) { m_session->DecPendingIo(); } }   // lease 반납 (0이면 종료 통지+풀 반납)
                Session* Get() const { return m_session; }

                SessionLeaseGuard(const SessionLeaseGuard&) = delete;
                SessionLeaseGuard& operator=(const SessionLeaseGuard&) = delete;
                SessionLeaseGuard(SessionLeaseGuard&&) = delete;
                SessionLeaseGuard& operator=(SessionLeaseGuard&&) = delete;
            private:
                Session* m_session;
            };

        }
	}
}
