#pragma once
#include "../GameServer/Network/IOCP/INetEventHandler.h"
#include "../GameServer//Core/Memory/ObjectPool.h"
#include "../GameServer/Core/Thread/SRWLockWrapper.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../GameCommon/GameDefines.h"
#include "IChannel.h"
#include "../Session/GameSession.h"
#include "../Player/Player.h"
#include "./DataBase/DBThread.h"
#include "SharedService/SharedServicesThread.h"

namespace KYS
{
    namespace GAMESERVER
    {
        namespace NETWORK
        {
            class IOCPServer;
        }
    }
}
namespace KYS
{
    namespace GAMECOMMON
    {
        namespace PROTOCOL
        {
            class CPacket;   // BuildChannelListPacket 시그니처용 (헤더 경량화 - 구현부만 CPacket.h 포함)
        }
    }
}
namespace KYS
{
    namespace SERVERAPP
    {
        class OwnedDeliveryJob;   // 전방 선언 - PostToChannel 이 소유권 검증 딜리버리 Job(귓속말 전달/실패통지)에 라우팅 채널을 stamp (정의=SharedService/CrossChannelChat.h)

        // 전 채널 공유 디스패처 - 라이브러리(INetEventHandler)가 넘긴 사건을 채널로 라우팅하고, 인증 게임세션 풀/맵을 소유한다.
        class ChannelManager : public KYS::GAMESERVER::NETWORK::INetEventHandler
        {
        public:
            // Meyers static 싱글턴. ctor/dtor는 private.
            static ChannelManager& GetInstance() { static ChannelManager instance; return instance; }

            ChannelManager(const ChannelManager&) = delete;
            ChannelManager& operator=(const ChannelManager&) = delete;
            ChannelManager(ChannelManager&&) = delete;
            ChannelManager& operator=(ChannelManager&&) = delete;

            // 게임세션 풀 + m_sessionMap + 채널 배열/서버 핸들 주입 (maxSessionCount는 시그니처 호환, 본문 미사용)
            bool Init(int maxSessionCount, IChannel** channels, int channelCount,KYS::GAMESERVER::NETWORK::IOCPServer* server);

            // INetEventHandler 구현 (라이브러리가 호출)
            void EnqueueConnect(UINT64 sid) override;
            void EnqueuePacket(UINT64 sid, const BYTE* data, int len) override;
            void EnqueueDisconnect(UINT64 sid, EDisconnectReason reason) override;

            GameSession* OnLoginSuccess(UINT64 sid, int chId, UINT32 accountId, UINT64 token);   // 로그인 성공 - 게임세션 할당+등록+obf 키 심기(publish 전 SeedObfKeys = seed-before-publish 원자)
            void         OnSessionGone(UINT64 sid);              // 종료 - map 제거+풀 반납 (+ accountId!=0이면 IS_PLAYER_LEAVE 통지, 역인덱스 제거)
            bool         KickByAccount(UINT32 accountId);        // 중복 로그인 축출 - accountId 역인덱스로 기존 세션 강제 종료. 반환=admitted victim(실 CCU 슬롯 회수) 축출 여부. pending도 함께 축출하되 반환엔 불포함(슬롯 미소비라 net-zero 근거 아님)
            bool         IsSaveReady(UINT32 accountId) const;    // 축출된 세션의 종료 저장이 enqueue 끝났나 (LoginLinkThread가 신규 Load defer 해제 판정, read 락)
            GameSession* GetSession(UINT64 sid);   // full sid 키 map 조회 (generation은 키의 일부)
            int          GetAuthenticatedCount() const;     // CCU = m_sessionMap.size() (인증 시점 insert, 집합 크기 파생)
            Player*      GetPlayer(UINT64 sid);    // GetSession(sid) ? ->GetPlayer() : nullptr
            void         CollectIdleSessions(int chId, UINT64 nowMs, UINT64 timeoutMs, std::vector<UINT64>& out) const;   // KickIdle용 idle 후보 sid 스냅샷
            // 채널별/(채널,맵)별 플레이어 수 집계 (m_sessionMap read 락 1회 순회). 호출자가 0-init 버퍼 제공. player 미부착 세션 제외.
            //   perChannel[ch] / perChannelMap[ch*MAX_MAP_COUNT + mapId] (보폭 = 버퍼 할당과 같은 상한 상수).
            void         CollectPopulation(int channelCount, int* perChannel, int* perChannelMap) const;
    // 진단(모니터) - cap 카운터(m_channelPlayerCount) + 파생 플레이어 총수 + player-less(로딩 중) + CCU 를 한 read 락 스냅샷으로.
    //   sum(cap)==CCU 와 derived+playerLess==CCU 불변식을 일관 스냅샷에서 검사하려는 것 (분리 락은 로딩 중 transient 붕괴).
    void         CollectPopulationDiagnostics(int channelCount, int* perChannelCap, int* outDerivedTotal, int* outPlayerLess, long* outCcu) const;
            int          GetGameSessionPoolUsed() const;   // 검증 관측 (누수 탐지 - 근사)
            int          GetPlayerPoolUsed() const;        // 검증 관측 (누수 탐지 - 근사)
            long         GetPreAuthBlockCount() const;     // pre-auth CS_GAME_AUTH cap 차단 누적 (flood 방어 관측 - 모니터 표출용)

            Player* AttachPlayer(UINT64 sid);                // Player 풀 할당+세션 부착만 (DbResultJob서 호출, spawn 은 아래 EnqueueChannelEnter 로 분리)
            void    EnqueueChannelEnter(UINT64 sid, int chId);   // 채널 진입(spawn) job 예약 - DbResultJob 이 캐릭터+인벤을 다 채운 뒤 호출(원자 로드, 인벤 채운 뒤에만 grid 편입)
            IChannel* GetChannel(int chId);                  // 범위 가드 후 m_channels[chId]

            // 채팅 - 대상 채널 mailbox 라우팅 (SST/CrossChannelChat는 GetInstance 직접 접근)
            void                  PostToChannel(UINT64 sid, OwnedDeliveryJob* job);   // 대상 sid의 채널 mailbox enqueue (resolve 채널을 job 에 stamp - execute 시 소유권 재검증)

            bool IsCcuFull() const;   // CCU >= MAX_CCU 여부 (GetAuthenticatedCount >= MAX_CCU)
            void RejectConnection(UINT64 sid, const BYTE* data, int size, EDisconnectReason reason);   // pre-auth sid에 패킷 Send + Disconnect
            void DisconnectPending(UINT64 sid, EDisconnectReason reason);   // pre-auth sid 강제 종료 - Send 없이 Disconnect만 (kick/timeout용, admission kick과 동일 패턴)

            // 채널 선택(B) 지원 - LoginLinkThread가 SC_CHANNEL_LIST 빌드/검증/송신에 사용.
            int  GetChannelCount() const { return m_channelCount; }                  // 채널 수 (SC_CHANNEL_LIST 빌드용)
            bool IsChannelJoinable(int chId) const;                                  // chId 유효 + cap 미만 (선택 검증, read 락)
            void SnapshotChannelCounts(int* outCounts, int n) const;                // 채널별 인구 스냅샷 (read 락)
            bool BuildChannelListPacket(KYS::GAMECOMMON::PROTOCOL::CPacket& out) const;   // 스냅샷 -> SC_CHANNEL_LIST 완성 패킷 (로그인 push + 인게임 재조회 공용 - wire 포맷 단일 소스)
            // 인게임 채널 이동 - count 스왑 + SetChannel + 대상 채널 enter job post 를 한 임계구역(write 락)에.
            //   락이 worker ResolveChannel(read)을 차단 -> 라우팅이 대상 채널로 열릴 때 enter job 이 이미 대상 mailbox 에 있어
            //   대상 tick 의 session-event(phase 0)가 grid insert 를 일반 패킷(phase 1)보다 먼저 처리하게 보장. 반환=이동 여부(진단).
            bool MigrateChannel(UINT64 sid, int fromCh, int toCh);
            void RerouteLeave(UINT64 sid, EDisconnectReason reason);   // 이관 중 disconnect가 옛 채널로 라우팅된 leave를 현 소유 채널로 재전달 (cross-thread UAF 봉인)
            void AppendOnlineAccounts(std::vector<UINT32>& accountIds, std::vector<UINT64>& sids) const;   // admission된 (accountId, sid) 를 append (online 스냅샷용, read 락)
            void SendRaw(UINT64 sid, const BYTE* data, int size);                   // pre-auth sid에 패킷 Send (disconnect 없이)

        private:
            ChannelManager();     // 싱글턴 - GetInstance로만 생성
            ~ChannelManager();

            int  ResolveChannel(UINT64 sid);   // sid -> 소속 채널 id (미인증/옛 sid면 INVALID_CHANNEL_ID)
            void HandlePreAuthPacket(UINT64 sid, const BYTE* data, int len);   // 미인증 세션 첫 패킷 - CS_GAME_AUTH면 토큰 검증 enqueue, 그 외엔 drop(worker 스레드)
            bool AllowPreAuthAttempt(UINT64 sid);   // 이 연결의 CS_GAME_AUTH 시도 +1, cap 이하면 true (초과 시 verify enqueue 스킵 - 인터서버 증폭 차단). LoginHandler::AllowLoginAttempt 미러

            KYS::GAMESERVER::MEMORY::ObjectPool<GameSession> m_gameSessionPool;   // 게임세션 메모리 풀 (소켓 idx 무관, Deallocate=free-list 반납, 블록 영구)
            KYS::GAMESERVER::MEMORY::ObjectPool<Player> m_playerPool;   // Player 메모리 단일 소유
            IChannel**   m_channels;
            int          m_channelCount;
            int          m_channelPlayerCount[MAX_CHANNEL_COUNT];   // 채널별 현재 플레이어 수 (OnLoginSuccess ++ / OnSessionGone --, m_sessionLock 내) - per-channel cap 판정용
            KYS::GAMESERVER::NETWORK::IOCPServer* m_server;   // server 핸들 (비소유, SendTo/DisconnectTo 진입점. 추상 라이브러리 핸들이라 싱글턴 전환서도 KEEP)

            std::unordered_map<UINT64, GameSession*> m_sessionMap;   // full sid 키 조회 (봇A != 봇B 분리 -> 잘못된 덮어쓰기 소멸)
            std::unordered_map<UINT32, UINT64>       m_accountToSid;   // accountId -> 현 세션 sid (중복 로그인 kick 역인덱스, m_sessionLock 공유 보호)
            std::unordered_set<UINT32>               m_pendingSaveAccounts;   // 축출된 세션의 종료 저장이 아직 enqueue 안 된 accountId 집합 (m_sessionLock 공유 보호). 신규 로그인 Load를 이 집합서 빠질 때까지 defer (lost-update 봉인)
            mutable KYS::GAMESERVER::THREAD::SRWLockWrapper m_sessionLock;   // read=조회 shared / write=login insert / leave erase exclusive

            std::unordered_map<UINT64, UINT32>              m_preAuthAttempts;   // pre-auth sid -> CS_GAME_AUTH 시도 수 (flood 증폭 차단, EnqueueDisconnect에서 정리 - 연결 수명과 동일)
            mutable KYS::GAMESERVER::THREAD::SRWLockWrapper m_preAuthLock;        // m_preAuthAttempts 보호 (worker 여럿이 HandlePreAuthPacket 동시 진입)
            static volatile LONG                            s_preAuthBlockCount;   // 시도 cap 차단 누적 (IOCP 워커 다수 - Interlocked). silent-cap 규약: drop 은 조용하되 카운터로 관측
        };
    }
}
