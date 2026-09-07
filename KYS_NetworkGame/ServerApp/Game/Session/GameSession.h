#pragma once
#include "../ServerApp/Game/Player/Player.h"
#include "../GameCommon/Protocol/PacketObfuscator.h"   // ObfKeyState 멤버 값 보유 (opcode 난독화 방향별 rolling 키)

namespace KYS { namespace GAMESERVER { namespace NETWORK { class IOCPServer; class Session; } } }

namespace KYS
{
    namespace SERVERAPP
    {
        constexpr int INVALID_CHANNEL_ID = -1;   // 채널 미소속 (미인증 pre-auth / 옛 sid)

        // 인증된 연결의 게임측 세션 - 소켓 Session(라이브러리)과 1:1, 채널 소속/Player를 들고 송신/종료를 대행한다.
        class GameSession
        {
        public:
            GameSession();
            ~GameSession() = default;

            GameSession(const GameSession&) = delete;
            GameSession& operator=(const GameSession&) = delete;
            GameSession(GameSession&&) = delete;
            GameSession& operator=(GameSession&&) = delete;

            int    GetChannelId() const { return m_channelId; }
            bool   IsAuthenticated() const { return m_channelId != INVALID_CHANNEL_ID; }
            UINT64 GetSessionId() const { return m_sessionId; }   // generation 대조 키
            void   SetLastActivityMs(UINT64 nowMs) { m_lastActivityMs = nowMs; }
            UINT64 GetLastActivityMs() const { return m_lastActivityMs; }
            void   SetChannel(int chId);                 // 로그인 완료(-1->N) / 채널 이동(A->B)
            Player* GetPlayer() const { return m_player; }               // 부착된 Player (Find=ChannelManager::GetSession(sid)->GetPlayer())
            void    SetPlayer(Player* p) { m_player = p; }           // MapManager.Spawn/Despawn(또는 ChannelManager)이 set/clear

            void   Init(UINT64 sessionId);               // 로그인 성공 시 호출 (짝 Session 핸들 세팅)
            void   Reset();                              // OnClientLeave 시 호출 (게임 스레드 자율 회수)

            bool Send(const BYTE* data, int size, ESendDropPolicy policy);   // 풀의 SendTo 위임. 반환 = 큐 적재 성공. 큐 넘침 처분은 policy 로 라이브러리 안에서(EVICT 면 그 자리에서 SEND_TIMEOUT 끊김)
            void Disconnect(EDisconnectReason reason);
            void SetServer(KYS::GAMESERVER::NETWORK::IOCPServer* server) { m_server = server; }   // ChannelManager::Init 주입

            // 패킷 opcode 난독화 (opcode obfuscation + rolling) - admission 시 token으로 방향별 키를 심고,
            //   direct 송신은 send 키로 뒤섞고, 수신 seam은 recv 키로 평문화한다. 라이브러리는 opcode를 모르므로 무침해.
            void SeedObfKeys(UINT64 token);                  // admission 시 token으로 방향별 키 심기 (recv=C2S, send=S2C)
            void SendScrambled(const BYTE* data, int size);  // direct post-auth 송신 - opcode scramble + send 키 advance 후 Send
            void DescrambleRecv(BYTE* pkt, int len);         // recv seam(EnqueuePacket)이 호출 - opcode de-scramble + recv 키 advance

            // 인증된 계정 번호 - game-auth 성공(admission) 시 set, 종료/축출 시 0. 0=미인증(IS_PLAYER_LEAVE 통지/역인덱스 게이트).
            void   SetAccountId(UINT32 accountId) { m_accountId = accountId; }
            UINT32 GetAccountId() const { return m_accountId; }

            // 축출(kick) 시 보존하는 accountId - m_accountId는 0으로 지워지므로 별도 채널. OnSessionGone가 종료 저장 후 pendingSave 플래그 clear에 사용.
            void   SetKickSaveAccountId(UINT32 a) { m_kickSaveAccountId = a; }
            UINT32 GetKickSaveAccountId() const { return m_kickSaveAccountId; }

            // admission(Init) 시각(GetTickCount64 글로벌 wall-clock). idle 활동 시각(m_lastActivityMs)도 채널 이관 cross-epoch 회피로 같은 GetTickCount64 - 되돌려 채널별 QPC 로 찍지 말 것. player-less 세션이 LOADING_DEADLINE_MS 넘게 고착하면 reaper 회수.
            UINT64 GetAdmittedAtMs() const { return m_admittedAtMs; }

            // per-tick 송신 배칭 (broadcast를 수신자별 1회로 묶어 WSASend 횟수 감소).
            //   ProcessPacket이 mover마다 AppendToBatch로 누적 -> tick-end FlushBatch로 1회 Send.
            void AppendToBatch(const BYTE* data, int size);        // 누적 (오버플로 시 내부 flush-and-continue)
            void FlushBatch();                                     // 누적분 1회 Send + 리셋 (m_batchPos>0일 때만)
            bool IsBatchQueued() const { return m_batchQueued; }   // 이번 tick flush 대상 등록 여부 (중복 등록 가드)
            void SetBatchQueued(bool v) { m_batchQueued = v; }
            UINT32 GetMidTickFlushCount() const { return m_midTickFlushCount; }   // tick 중간 강제 flush 누적 (소켓 큐 적체 상류 조기신호 - 모니터 라벨)

        private:

            UINT64  m_sessionId;                         // 짝 Session 핸들 (generation 포함 - 1:1 매핑)
            int     m_channelId;                         // -1=PRE_LOGIN, >=0=POST_LOGIN
            Player* m_player;
            KYS::GAMESERVER::NETWORK::IOCPServer* m_server;
            UINT64  m_lastActivityMs;   // 마지막 활동 시각(ms). 0=미초기화 - sweep 제외 가드
            UINT32  m_accountId;        // 인증 계정 번호 (admission 시 set, 종료/축출 시 0). leave 통지 + kick 역인덱스 키
            UINT32  m_kickSaveAccountId;  // 축출된 세션이 빚진 종료 저장의 accountId (m_accountId=0으로 지워져도 보존). OnSessionGone가 종료 저장 후 pendingSave clear에 사용. 0=축출 아님.
            UINT64  m_admittedAtMs;       // admission(Init) 시각(GetTickCount64). player-less 고착 좀비 reaper deadline 기준. 0=미admission/회수됨.

            KYS::GAMECOMMON::PROTOCOL::ObfKeyState m_sendKey;   // S2C 방향 rolling 키 (channel 스레드만 advance - 단일 writer 락 불요)
            KYS::GAMECOMMON::PROTOCOL::ObfKeyState m_recvKey;   // C2S 방향 rolling 키 (worker만 advance - 단일 writer 락 불요)

            static const int SEND_BATCH_SIZE = 8192;   // 송신 배칭 버퍼 크기 (수신자 AOI 누적, 시작값, 측정 후 조정)
            BYTE m_sendBatch[SEND_BATCH_SIZE];          // per-tick broadcast 누적 (풀 슬롯 동승 - per-tick alloc 0)
            static_assert(MAX_PACKET_SIZE <= SEND_BATCH_SIZE, "패킷 하나는 배치 버퍼에 담겨야 한다 - AppendToBatch 의 단일 대형 패킷 우회 경로는 이 등식 아래서 도달 불가");
            int  m_batchPos;                            // 누적 위치 (다음 append 지점, FlushBatch가 0 리셋)
            bool m_batchQueued;                         // flush 리스트 등록 여부 (tick당 1회 등록 가드)
            UINT32 m_midTickFlushCount;                 // AppendToBatch 가 tick 중간에 강제 flush 한 횟수(배치 오버플로/단일 대형 패킷 우회) - 소켓 큐 적체 상류 조기신호. Reset 0화.
        };

    }
}
