#pragma once
#include "IChannel.h"
#include "../Session/GameSession.h"
#include "../GameObject.h"
#include "../Player/Player.h"
#include "../World/MapManager.h"
#include "../GameServer/Core/Timer.h"
#include "../GameServer/Core/Thread/TwoLockJobQueue.h"
#include "../GameCommon/Protocol/PacketType.h"    // OnChat opcode 파라미터 타입 (디스패처 decode 값 - 재파싱 제거)
#include "../GameCommon/Protocol/GamePackets.h"
#include "../GameCommon/Protocol/CPacket.h"      // m_broadcastPacket 값 멤버 - 완전 타입 필요 (GamePackets 재수출 의존 제거)
#include <vector>


namespace KYS
{
    namespace SERVERAPP
    {
        class ChannelManager;

        static const int LEAVE_REASON_COUNT = static_cast<int>(EDisconnectReason::COUNT);   // EDisconnectReason 값 수 (enum COUNT 센티널 파생 - SSOT, 값 추가 시 자동 반영)

        struct ChannelStatsSnapshot         // 모니터링 스냅샷
        {
            UINT64 packetCount; UINT64 bytesRecv; UINT64 bytesSent;
            UINT64 totalTickUs; UINT64 maxTickUs; UINT64 overBudgetCount;
            UINT64 warnCount;   UINT64 tickCount;
            // per-phase 계측 (since-start 누적, totalTickUs 합 대비 비율로 병목 phase 진단)
            UINT64 phaseRecvUs;       // ProcessSessionEvent+ProcessJob
            UINT64 phaseBroadcastUs;  // SendObjectUpdates 전체
            UINT64 phaseSendUs;       // SendObjectUpdates 내 gs->Send 루프 (broadcast 중 송신 부분)
            UINT64 phaseUpdateUs;     // UpdateFrame
            int    monsterPerMap[MAX_MAP_COUNT];   // 맵별 현재 몬스터 수 (근사. 배열 = 상한 크기, 쓰는 칸 = MapTableCount())
            // tick 정밀 지표 - 최근 window 표본을 정렬해 산출 (avg/max만으로 못 보는 분포).
            //   trimmedAvg=상하위 5% 제거 대표값(spike 오염 배제) / p50,p95,p99=분포 위치 / windowMax=window 내 최대.
            UINT64 trimmedAvgUs;    // 상하위 5% trim 평균 (대표값 - bimodal spike 오염 배제)
            UINT64 p50Us;           // 중앙값
            UINT64 p95Us;           // 95 백분위
            UINT64 p99Us;           // 99 백분위 (tail - spike 포착)
            UINT64 windowMaxUs;     // window 내 최대 (전체 max는 maxTickUs가 영구 보존)
            int    tickSampleCount; // 산출에 쓴 표본 수 (워밍업 동안 < window)
            // idle/burst 진단 (측정 정밀화 - 모두 경량 always-on: QPC/카운터/버퍼 = budget 0.001%)
            UINT64 sleepUs;         // 실제 Sleep 누적 (idle% = sleepUs/(sleepUs+totalTickUs) - trimmedAvg 추정 대체로 bimodal 정확)
            UINT64 jobP50;          // tick당 입력(drain job) 수 50백분위
            UINT64 jobP95;          // tick당 입력 수 95백분위 (jobP95 >> jobP50 = 입력 burst = WiFi/네트워크 몰림)
            UINT64 jobMax;          // tick당 입력 수 최대
            UINT64 moveDropped;     // movement lane DROP(shed) 누적 - 과부하 시 흘려보낸 CS_MOVE 수(>0=과부하 신호)
            UINT64 criticalDropped; // critical lane DROP 누적 - 과부하 burst 시 login/skill/chat 손실(0이 정상, >0=critical 풀 소진 경고)
            long   leaveReasonCount[LEAVE_REASON_COUNT];   // 이 채널이 처리한 leave 의 reason 별 누적 (대량 이탈 사후 분석 - 메트릭 export 재료)
        };

        // 한 게임 채널 - 자기 스레드에서 30Hz 루프를 돌며 패킷 처리/객체 갱신/broadcast를 담당. 전부 게임 채널(0..N-1).
        class Channel : public IChannel
        {
        public:
            Channel(int channelId);   // ChannelManager는 GetInstance 직접 (DI 제거)
            ~Channel() override;

            // IChannel 구현 (라이브러리가 넘긴 네트워크 사건 처리)
            bool EnqueueJob(KYS::GAMESERVER::THREAD::IJob* job) override;          // critical mailbox에 넣기 (반환 = 성공, false=포화 drop)
            void EnqueueMoveJob(KYS::GAMESERVER::THREAD::IJob* job) override;      // movement(CS_MOVE) mailbox에 넣기 (과부하 시 먼저 shed)
            void EnqueueSessionEvent(KYS::GAMESERVER::THREAD::IJob* job) override; // 세션이벤트 큐에 넣기
            int  GetChannelId() const override;                                    // m_channelId 반환
            Monster* FindMonsterForPath(int mapId, UINT32 monsterId) override;      // 길찾기 결과 재주입 - m_mapManager 위임
            void OnChannelEnter(UINT64 sid) override;
            void OnChannelChangeEnter(UINT64 sid) override;   // 인게임 채널 변경 도착측 (대상 채널 스레드가 실행)
            void OnChannelLeave(UINT64 sid) override;
            void OnClientLeave(UINT64 sid, EDisconnectReason reason) override;
            void OnRecv(UINT64 sid, const BYTE* data, int size) override;          // opcode switch 분기

            // 부팅 2단계 - main 이 콘텐츠 파일(maps/monsters/spawns) 적재 검증 후 채널 스레드 기동 전에 호출.
            //   생성자에서 부르지 않는 이유: 파일 적재가 채널 생성보다 늦어 빈 표로 스폰하게 되기 때문.
            void SpawnInitialMonsters();

            // 30Hz 게임 루프
            void TickLoop();
            void Stop();       // 루프 종료 신호 (main 종료 시)

            ChannelStatsSnapshot GetMonitorStats() const; // 모니터 카운터 복사 반환
            UINT64 GetTickCounter() const;                // 워치독 하트비트 읽기 - 매 tick 증가 카운터 (cross-thread 근사 read)

            void* operator new(size_t size);    // 64바이트 정렬 할당 (m_mailbox alignas(64))
            void  operator delete(void* ptr);



        private:

            // TickLoop = 얇은 오케스트레이터 + 4개 명명 phase 메서드.
            //   순서: drain -> broadcast -> update -> measure-then-sleep.
            //   한 큰 함수를 명명 private 메서드로 나눠 흐름을 드러냄 (Nystrom Game Loop / TrinityCore World::Update 분리 정합).
            void ProcessSessionEvent();   // (0) 세션이벤트 큐 우선 drain (enter/leave)
            void ProcessJob();            // (1) drain - mailbox job Execute (OnMove=대조 후 수용/보정, Send 0)
            void SendObjectUpdates();         // (2) broadcast - 위치 바뀐 객체를 시야(AOI) 전원에게 송신
            void UpdateFrame(float deltaTime);   // (3) update - 객체 순회 다형 Update(dt): 위치 예측(4방향 표) -> 범위 안에 가둠 -> 바뀜 표시
            void KickIdleSessions();      // idle 세션 회수 (약 1초 간격 제한)
            void Sleep(UINT64 startUs);   // (4) measure-then-sleep - tick 처리시간 측정(QPC 2회) 후 남은 시간 Sleep

            // 종료 저장 - TickLoop 루프 종료(m_running=false) 후 1회. 접속 중 전 플레이어를 dirty 무관 저장(마지막 기회).
            //   소유 채널 스레드에서 실행(single-writer) - kick-with-save(OnClientLeave) 형태를 일괄로 재사용.
            void ShutdownSaveConnectedPlayers();

            // SendObjectUpdates (2) broadcast 의 3개 하위 phase + GetMonitorStats 분포 산출 helper
            void BroadcastMovedObjects();                          // (2a) 바뀜 표시 mover를 시야(AOI) 안 player 전원에게 배치 누적
            void BroadcastSpawnsAndDespawns(ChannelManager& cm);   // (2b) Map의 출현/소멸 패킷을 수신자 배치에 합류
            void FlushBatchedSends();                              // (2c) 수신자별 누적 배치를 1회씩 Send (tick-end flush)
            void FillTickDistribution(ChannelStatsSnapshot& s) const;   // GetMonitorStats - tick 분포 지표(trimmed mean/percentile) 산출

            // private 패킷 핸들러 (OnRecv의 switch case가 호출)
            void OnMove(UINT64 sid, const BYTE* data, int size);     // CS_MOVE -> dead-reckon 대조 후 수용/보정
            void OnPing(UINT64 sid, const BYTE* data, int size);     // CS_PING -> SC_PONG echo
            void OnPortal(UINT64 sid, const BYTE* data, int size);   // CS_PORTAL -> sid resolve -> m_mapManager.ChangeMap
            void OnChannelChange(UINT64 sid, const BYTE* data, int size);   // CS_CHANNEL_CHANGE -> 출발측 (A 그리드 제거 + 배치 quiesce + MigrateChannel)
            void OnChannelListRequest(UINT64 sid);   // CS_CHANNEL_LIST_REQUEST -> 인게임 picker 인구 재조회 (현재 인구 스냅샷 SC_CHANNEL_LIST 회신)
            void OnSkill(UINT64 sid, const BYTE* data, int size);    // CS_SKILL -> 서버 권위 판정 + SC_DAMAGE/SC_DEATH 송신
            void OnChat(UINT64 sid, PacketType opcode, const BYTE* data, int size);     // CS_CHAT(일반)/CS_WHISPER(귓속말) opcode 분기 - 핸들러로 위임
            void HandlePublicChat(Player* sender, KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);   // CS_CHAT 분기 - 같은 맵 전원 broadcast (m_mapManager/m_flushList/m_bytesSent 접근)
            void HandleWhisper(UINT64 senderSid, Player* sender, KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);   // CS_WHISPER 분기 - cross-channel 라우팅 + 실패 통지 (CanChat 호출로 멤버)

            bool CanChat(Player* sender, const wchar_t* msg, int len);   // 도배 간격 제한 + 빈 메시지 가드 (Player::m_lastChatMs)

            // 아이템/인벤토리 핸들러 (OnRecv switch가 호출 - critical lane)
            void OnItemPickup(UINT64 sid, const BYTE* data, int size);   // CS_ITEM_PICKUP -> 거리/소유권/공간 검증 후 인벤 편입
            void OnItemMove(UINT64 sid, const BYTE* data, int size);     // CS_ITEM_MOVE -> 가방 칸 이동 (스택 합류/스왑)
            void OnItemUse(UINT64 sid, const BYTE* data, int size);      // CS_ITEM_USE -> 소모품 효과 적용 + 수량 감소
            void OnItemEquip(UINT64 sid, const BYTE* data, int size);    // CS_ITEM_EQUIP -> 장착 (스탯은 파생 - 저장 안 함)
            void OnItemUnequip(UINT64 sid, const BYTE* data, int size);  // CS_ITEM_UNEQUIP -> 장착 해제 -> 가방 빈 칸
            void OnItemDiscard(UINT64 sid, const BYTE* data, int size);  // CS_ITEM_DISCARD -> 가방에서 빼서 자기 위치 바닥 드랍
            void GenerateDrops(GameObject* deadMonster);                 // 몬스터 사망 확정 시 드랍 표 추첨 -> 바닥 드랍 생성 (ApplySkillHit가 호출)

            // 거래 핸들러 (OnRecv switch가 호출 - critical lane). 같은 맵 상대끼리 인메모리 스왑 후 단일 DB 트랜잭션으로 확정.
            void OnTradeRequest(UINT64 sid, const BYTE* data, int size);     // CS_TRADE_REQUEST -> 이름으로 같은 맵 상대에게 요청
            void OnTradeResponse(UINT64 sid, const BYTE* data, int size);    // CS_TRADE_RESPONSE -> 받은 요청 수락/거절 (수락 시 양측 거래창 열기)
            void OnTradeAddItem(UINT64 sid, const BYTE* data, int size);     // CS_TRADE_ADD_ITEM -> 거래창에 오퍼 올리기 (양측 수락 리셋)
            void OnTradeRemoveItem(UINT64 sid, const BYTE* data, int size);  // CS_TRADE_REMOVE_ITEM -> 오퍼 내리기 (양측 수락 리셋)
            void OnTradeAccept(UINT64 sid, const BYTE* data, int size);      // CS_TRADE_ACCEPT -> 내 수락 표시 (양측 수락 시 커밋)
            void OnTradeCancel(UINT64 sid, const BYTE* data, int size);      // CS_TRADE_CANCEL -> 거래 취소

            // 거래 헬퍼
            Player* ResolveTradePartner(Player* me);                                     // me의 거래 상대 조회 - 상호 참조/같은 맵/생존 재검증 (실패 nullptr)
            GameSession* ResolvePartnerSession(ChannelManager& cm, const TradeState& tradeState) const;   // 거래 상대 세션 조회 - partnerSid!=0 + 세션 존재 + 같은 채널 (실패 nullptr)
            void SendTradeUpdate(GameSession* gs, const Player* me, const Player* partner);   // SC_TRADE_UPDATE (me 시점 - 내 오퍼/상대 오퍼/양측 수락)
            void SendTradeResult(GameSession* gs, BYTE result);                          // SC_TRADE_RESULT (종결/실패 사유)
            void CancelTradeFor(Player* p, BYTE result);                                 // 거래 강제 종결 - 양측 상태 리셋 + SC_TRADE_RESULT (취소/상대 이탈/leave/portal/채널변경 공용)
            void CommitTrade(Player* a, Player* b, GameSession* gsA, GameSession* gsB);   // 양측 수락 완료 - 사본 시뮬 검증 후 인메모리 스왑 + 단일 DB 트랜잭션

            // 아이템 응답 헬퍼 (핸들러 공용)
            Player* ResolveOwnedPlayer(ChannelManager& cm, UINT64 sid);         // 세션 소유(채널 일치) + Player 조회 - IsDead 미검사 (실패 nullptr). OnMove/OnPortal/OnSkill/OnChat/OnChannelListRequest 공용
            Player* ResolveItemSender(UINT64 sid, GameSession*& outGs);         // 세션 소유 검증 + Player 조회 (실패 nullptr)
            Player* BeginItemHandler(UINT64 sid, GameSession*& outGs);          // 아이템 핸들러 공통 진입 - ResolveItemSender + 거래 중이면 LOCKED_IN_TRADE 통지 후 nullptr
            void SendItemResult(GameSession* gs, BYTE result);                  // SC_ITEM_RESULT (실패 사유 통지)
            void SendFullInventory(GameSession* gs, const Player* p);           // SC_INVENTORY (전량 스냅샷 - 줍기/거래 완료 후)
            void SendStatUpdate(GameSession* gs, const Player* p);              // SC_STAT_UPDATE (장착 변화 후 공/방 통지)
            void SendItemUpdate(GameSession* gs, const Player* p, const int* slots, int slotCount);   // SC_ITEM_UPDATE (변경 폭이 정확한 조작 - 이동/사용/장착/버림) - 지정 칸의 현재 내용을 통지 (빈 칸 = uid 0)
            // 한 수신자에게 B4 배치 누적(즉시 Send 대신 tick-end flush). 채팅/스킬 inline 송신을 이동 broadcast와 같은 경로로 통일.
            void BatchTo(GameSession* gs, const Player* owner, const BYTE* buf, int size);    // null + 채널 소유권 + 짝(gs 가 owner 를 자기 Player 로 인정하는가) 3가드 - 역포인터 수신자의 방어선. owner=nullptr 이면 짝 검사 생략
            void RemoveFromFlushList(GameSession* gs);   // 채널 이동 출발측 - mover를 tick-end flush 대상에서 제외(swap-pop) - cross-thread 배치 레이스 봉인
            void BeginChannelHandoff(UINT64 sid, GameSession* gs, Player* p, int targetCh, ChannelManager& cm);   // 채널 이관 실행 - 출발측 그리드 제거 + 배치 quiesce + MigrateChannel (검증은 호출부가 끝냄)
            void BatchToPlayersInAoi(const std::vector<GameObject*>& aoi, const BYTE* buf, int size);   // AOI 중 player 수신자에게만 배치 (스킬 SC_DAMAGE/SC_DEATH 공통)
            void ApplySkillHit(Player* caster, GameObject* target);   // 한 타겟에 데미지+SC_DAMAGE(대상 AOI 관찰자), 사망 전이 시 SC_DEATH

            // 데이터 멤버
            int                                   m_channelId;        // GetChannelId 반환값 (전부 게임 채널)

            KYS::GAMESERVER::THREAD::TwoLockJobQueue m_mailbox;      // critical lane (skill/chat/portal/whisper/ping) - Worker enqueue, Channel drain
            KYS::GAMESERVER::THREAD::TwoLockJobQueue m_moveMailbox;  // movement lane (CS_MOVE) - 과부하 시 먼저 shed(dead-reckoning이 메움), 별도 cap drain
            KYS::GAMESERVER::THREAD::TwoLockJobQueue m_sessionEventMailbox;  // 세션이벤트 큐 (GROW). 선언은 m_mailbox 뒤
            KYS::GAMESERVER::Timer                   m_tickTimer;
            UINT64                                   m_lastDbSaveMs;
            UINT64                                   m_lastSweepMs;          // idle 점검 간격 제한 기준시각
            volatile bool                            m_running;   // TickLoop 종료 플래그 (Stop이 false로)

            // 레이어 B 데이터 멤버
            MapManager                m_mapManager;   // 게임 도메인 오케스트레이터 (값/1:1)
            std::vector<GameObject*>  m_movedObjects;       // SendObjectUpdates의 이번 tick 바뀜 표시 mover 수집 재사용 버퍼 (매 tick 재할당 회피)
            std::vector<OutboundPacket>   m_pendingPackets;       // spawn/despawn 송신 대기 재사용 버퍼
            std::vector<UINT64>           m_idleSids;             // KickIdle용 idle 세션 sid 스냅샷 (매 sweep 재사용)
            long                          m_leaveReasonCount[LEAVE_REASON_COUNT] = { 0 };   // OnClientLeave reason 별 누적 (채널 스레드 단일 writer - 스냅샷/export 로 소비, 로그 drop 과 무관하게 생존하는 분포)
            std::vector<GameSession*>     m_flushList;            // B4: 이번 tick 송신 배치가 쌓인 수신자 (tick-end flush 대상)
            KYS::GAMECOMMON::PROTOCOL::CPacket m_broadcastPacket;    // SendObjectUpdates 이동 broadcast 직렬화 재사용 패킷 (mover당 new CPacket 할당 제거)

            // 서버 모니터: Channel 로컬 카운터
            UINT64 m_packetCount;     // OnRecvJob 처리수 (단조 증가)
            UINT64 m_bytesRecv;       // 수신 처리 바이트 누적
            UINT64 m_bytesSent;       // broadcast 송신 바이트 누적
            UINT64 m_totalTickUs;     // tick 처리시간 누적
            UINT64 m_maxTickUs;       // tick 처리시간 최대 (시작 이후)
            UINT64 m_overBudgetCount; // elapsedUs >= TICK_INTERVAL_US(예산 초과) 횟수
            UINT64 m_warnCount;       // 예산의 4/5 <= elapsedUs < 예산 횟수
            UINT64 m_tickCount;       // 총 tick 횟수

            // per-phase 계측 누적 (tick 병목 위치 진단 - send-loop이 broadcast의 몇 %인지)
            UINT64 m_phaseRecvUs;       // ProcessSessionEvent+ProcessJob 누적
            UINT64 m_phaseBroadcastUs;  // SendObjectUpdates 누적
            UINT64 m_phaseSendUs;       // SendObjectUpdates 내 gs->Send 루프 누적
            UINT64 m_phaseUpdateUs;     // UpdateFrame 누적

            // tick 정밀 지표용 표본 원형 버퍼 (최근 N tick elapsedUs).
            //   avg=누적합/max=단일변수로 충분하나 percentile/trimmed mean은 표본 정렬이 필요해 따로 보관.
            //   sliding window(최근 N개, 가장 오래된 것 덮어씀) - 무한 누적 회피 + "지금 부하" 반영.
            //   전체 max(all)은 위 m_maxTickUs가 영구 보존(window 밖 옛 spike도 유지).
            static const int TICK_SAMPLE_WINDOW = 600;   // 30Hz = 최근 20초 (표본 많을수록 percentile 안정, 게임 무관)
            UINT64 m_tickSamples[TICK_SAMPLE_WINDOW];    // 원형 버퍼 (elapsedUs 표본)
            int    m_tickSampleHead;                     // 다음 기록 위치 (원형 인덱스)
            int    m_tickSampleCount;                    // 채워진 표본 수 (<= WINDOW, 워밍업 동안 미만)

            // idle/burst 진단 (always-on 경량)
            UINT64 m_sleepUs;                            // 실제 Sleep 누적 (idle 정확 산출)
            int    m_lastDrainCount;                     // 직전 tick ProcessJob drain 수 (Sleep이 m_jobSamples에 기록)
            UINT64 m_jobSamples[TICK_SAMPLE_WINDOW];     // tick당 입력 수 원형 버퍼 (m_tickSampleHead 공유)

            volatile LONG64 m_droppedMove;       // movement lane DROP(shed) 누적 - 워커 다수 Interlocked (과부하 가시화, 모니터)
            volatile LONG64 m_droppedCritical;   // critical lane DROP 누적 - movement와 대칭 계측 (DROP은 best-effort safety valve, 소진 시 login/skill 손실 가시화)

        };
    }
}
