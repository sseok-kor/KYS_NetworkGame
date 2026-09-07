#include "pch_serverapp.h"
#include "GameSession.h"
#include "../GameServer/Network/IOCP/IOCPServer.h"
#include "../GameCommon/Protocol/PacketHeader.h"       // sizeof(PacketHeader) (SendScrambled/DescrambleRecv 방어)
#include "../GameCommon/Protocol/PacketObfuscator.h"   // InitObfKey/ObfuscateOpcode/DeobfuscateOpcode
#include <cstring>   // memcpy (AppendToBatch / SendScrambled)

KYS::SERVERAPP::GameSession::GameSession()
    : m_sessionId(0)
    , m_channelId(INVALID_CHANNEL_ID)
    , m_player(nullptr)
    , m_server(nullptr)
    , m_lastActivityMs(0)
    , m_accountId(0)
    , m_kickSaveAccountId(0)
    , m_admittedAtMs(0)
    , m_sendKey{ 0 }
    , m_recvKey{ 0 }
    , m_batchPos(0)
    , m_batchQueued(false)
    , m_midTickFlushCount(0)
{
}

// 채널 소속을 세팅한다 (로그인 완료 / 채널 이동). write 주체는 ChannelManager 하나뿐(단일 writer).
void KYS::SERVERAPP::GameSession::SetChannel(int chId)
{
    m_channelId = chId;
}

// 로그인 성공 시 - 짝 Session 핸들(generation 포함)을 세팅하고 상태를 비운다.
//   sessionId : 짝 소켓 세션 핸들 (대조 키)
void KYS::SERVERAPP::GameSession::Init(UINT64 sessionId)
{
    m_sessionId = sessionId;     // 짝 Session 핸들(generation) = 대조 키
    m_channelId = INVALID_CHANNEL_ID;   // 직후 SetChannel(chId)로 -1->N (찰나만 -1)
    m_player = nullptr;
    m_lastActivityMs = 0;   // 진입(OnChannelEnter) 전까지 0 - sweep 제외
    m_accountId = 0;        // 직후 SetAccountId(accountId)로 채움 (admission) - 그 전엔 미인증 표식
    m_kickSaveAccountId = 0;   // 새 로그인은 축출 빚 없음
    m_admittedAtMs = ::GetTickCount64();   // admission 시각 박음(GetTickCount64 글로벌) - player-less loading deadline reaper 기준
    m_sendKey.seed = 0;   // SeedObfKeys(admission)가 채우기 전까지 0 (pre-arm) - 실제 arm은 캐릭터 선택 시 SeedObfKeys
    m_recvKey.seed = 0;
}

// 종료 시 - 게임 스레드가 자율 회수. 옛 활동값을 들고 재사용되지 않게 비운다.
void KYS::SERVERAPP::GameSession::Reset()
{
    m_sessionId = 0;
    m_channelId = INVALID_CHANNEL_ID;
    m_player = nullptr;
    m_lastActivityMs = 0;   // 회수된 슬롯이 옛 활동값으로 재사용되는 것 방지
    m_accountId = 0;        // 회수 슬롯이 옛 계정번호로 재사용되는 것 방지 (stale 봉인)
    m_kickSaveAccountId = 0;   // 회수 슬롯 stale kick-save 봉인
    m_admittedAtMs = 0;        // 회수 슬롯 stale admission 시각 봉인
    m_sendKey.seed = 0;        // 회수 슬롯 stale rolling 키 봉인 (재사용 세션 첫 패킷 desync 방지)
    m_recvKey.seed = 0;
    m_batchPos = 0;         // 송신 배치 리셋 (회수 슬롯에 옛 누적 잔류 방지)
    m_batchQueued = false;
    m_midTickFlushCount = 0;   // 회수 슬롯 stale 계측 봉인 (재사용 세션이 옛 midFlush/kick 상속 방지)
}

// 내 소켓으로 송신한다 (연결이 이미 끊겼으면 무시).
//   data : 보낼 바이트
//   size : 바이트 수
bool KYS::SERVERAPP::GameSession::Send(const BYTE* data, int size, ESendDropPolicy policy)
{
    if (m_server == nullptr) { return false; }   // 풀 재구성(placement-new) 중 일시적 nullptr 방어 (현재 도달 불가, 깨지기 쉬운 불변식)
    return m_server->SendTo(m_sessionId, data, size, policy);   // 찾기/세대 검증/lease/송신/반납은 풀 안에서. 큐 넘침 처분(끊기+집계)도 그 안(policy) - 여기는 "실렸나" 만 본다
}

// 내 소켓을 종료한다.
//   reason : 종료 사유
void KYS::SERVERAPP::GameSession::Disconnect(EDisconnectReason reason)
{
    if (m_server == nullptr) { return; }   // 풀 재구성(placement-new) 중 일시적 nullptr 방어 (현재 도달 불가, 깨지기 쉬운 불변식)
    m_server->DisconnectTo(m_sessionId, reason);   // 찾기/세대 검증/lease/CAS 종료/반납은 풀 안에서. 없는/옛 핸들이면 no-op
}

// admission(캐릭터 선택) 시 token으로 방향별 키를 심는다 (클라 ArmObfuscation과 같은 token = 대칭).
void KYS::SERVERAPP::GameSession::SeedObfKeys(UINT64 token)
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    InitObfKey(m_recvKey, token, ObfDirection::CLIENT_TO_SERVER);   // 내가 받는 방향
    InitObfKey(m_sendKey, token, ObfDirection::SERVER_TO_CLIENT);   // 내가 보내는 방향
}

// direct(비배칭) post-auth 송신 - opcode를 send 키로 뒤섞은 사본을 보낸다. 방향 키 1회 advance.
//   사본을 쓰는 이유: 원본/공유 직렬화 버퍼를 훼손하지 않기 위함 (2바이트만 바뀌어도 공유분은 불변 유지).
void KYS::SERVERAPP::GameSession::SendScrambled(const BYTE* data, int size)
{
    if (size < static_cast<int>(sizeof(PacketHeader)) || size > MAX_PACKET_SIZE)
    {
        Send(data, size, ESendDropPolicy::KEEP_CONNECTION);   // 헤더 미만/과대 - 뒤섞지 않고 그대로 (도달 불가 방어)
        return;
    }
    BYTE scrambled[MAX_PACKET_SIZE];
    memcpy(scrambled, data, static_cast<size_t>(size));
    KYS::GAMECOMMON::PROTOCOL::ObfuscateOpcode(scrambled, m_sendKey);   // opcode scramble + send 키 advance
    Send(scrambled, size, ESendDropPolicy::EVICT_ON_DROP);   // drop = 치명: send 키는 이미 advance 됐는데 이 패킷이 클라에 도달 안 함 -> rolling 키 영구 desync.
                                                          //   그래서 EVICT - 넘친 그 자리에서 라이브러리가 SEND_TIMEOUT 로 끊고 축출을 센다(재접속이 arm/키를 리셋)
}

// recv seam(EnqueuePacket)이 복사본에 대해 호출 - opcode를 recv 키로 평문화 + recv 키 1회 advance.
//   drop 여부와 무관하게 수신 순서대로 딱 1회 호출돼야 클라 send 키와 동기 유지된다.
void KYS::SERVERAPP::GameSession::DescrambleRecv(BYTE* pkt, int len)
{
    if (len < static_cast<int>(sizeof(PacketHeader))) { return; }   // opcode 자리 없음 (도달 불가 방어)
    KYS::GAMECOMMON::PROTOCOL::DeobfuscateOpcode(pkt, m_recvKey);
}

// per-tick broadcast를 수신자별 버퍼에 누적한다 (즉시 Send 안 함). 오버플로면 먼저 flush 후 이어 담음.
void KYS::SERVERAPP::GameSession::AppendToBatch(const BYTE* data, int size)
{
    if (size > SEND_BATCH_SIZE)   // 단일 패킷이 배치 버퍼보다 크면 배칭 우회 - 누적분 먼저 비우고(순서 보존) scramble 경로로 직접 송신
    {
        ++m_midTickFlushCount;   // 계측: 단일 대형 패킷 우회 = tick 중간 강제 flush (상류 압력 조기신호)
        FlushBatch();
        SendScrambled(data, size);   // 우회분도 opcode scramble (평문 누출 방지)
        return;
    }
    if (m_batchPos + size > SEND_BATCH_SIZE)
    {
        ++m_midTickFlushCount;   // 계측: 배치 오버플로 = tick 중간 강제 flush
        FlushBatch();   // 오버플로 - 현재 누적분 먼저 전송 후 리셋 (flush-and-continue. 정상 부하선 드문 안전판)
    }
    memcpy(m_sendBatch + m_batchPos, data, size);   // 공유 직렬화 결과를 수신자 버퍼에 평문 복사 (serialize-once + copy). scramble 은 FlushBatch(실제 전송) 시점에 - wire 순서 == keystream 순서 보장
    m_batchPos += size;
}

// 누적된 배치를 한 번에 Send한다 (수신자당 1 WSASend로 tick 내 K개 패킷 운반).
void KYS::SERVERAPP::GameSession::FlushBatch()
{
    if (m_batchPos > 0)
    {
        // wire 순서 == keystream 순서가 되도록 opcode scramble 을 '실제 전송(flush) 직전'에 wire 순서대로 수행한다.
        //   배치는 전송이 phase2c 로 지연되므로 scramble 도 지연해야, 즉시 전송하는 direct SendScrambled 와 키 위치가 wire 순서와 일치한다.
        //   (accumulate 시점 scramble 은 지연 전송분이 낮은 keystream 위치를 갖고도 wire 상 direct 뒤에 놓여 클라가 키를 교차 적용 -> opcode 오염.)
        int off = 0;
        while (off + static_cast<int>(sizeof(PacketHeader)) <= m_batchPos)
        {
            const USHORT pktSize = static_cast<USHORT>((m_sendBatch[off] << 8) | m_sendBatch[off + 1]);   // 앞 2B = 전체 길이 (BE 평문)
            if (pktSize < sizeof(PacketHeader) || off + static_cast<int>(pktSize) > m_batchPos) { break; }   // 방어 (도달 불가 - 정상 배치는 완성 패킷 연속)
            KYS::GAMECOMMON::PROTOCOL::ObfuscateOpcode(m_sendBatch + off, m_sendKey);   // 이 패킷 opcode scramble + send 키 1회 advance (wire 순서대로)
            off += pktSize;
        }
        Send(m_sendBatch, m_batchPos, ESendDropPolicy::EVICT_ON_DROP);   // 연속영역 1 WSASend. drop = 치명(SendScrambled 와 같은 근거: 키가 이미 N회 advance) -> 라이브러리가 그 자리에서 끊고 센다
        m_batchPos = 0;
    }
}
