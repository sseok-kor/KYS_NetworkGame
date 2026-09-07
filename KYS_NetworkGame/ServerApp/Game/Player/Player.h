#pragma once
#include "../GameObject.h"
#include "../Item/Inventory.h"
#include "../Item/TradeState.h"
#include "../../../GameCommon/GameDefines.h"
#include "../../../GameCommon/GameTypes.h"
#include <unordered_set>

namespace KYS
{
    namespace SERVERAPP
    {
        class GameSession;   // 전방선언 (Player.h<->GameSession.h 순환 include 회피 - 포인터 멤버만 보유)

        // per-player 남용 방어 대상 행동 (각자 쿨다운 타임스탬프 - Player::TryAct 게이트). 채팅은 기존 별도 게이트(m_lastChatMs) 사용.
        enum class EPlayerAction { PORTAL, CHANNEL_CHANGE, CHANNEL_LIST, ACTION_COUNT };

        // 접속한 플레이어 한 명 - GameObject 를 상속해 위치/이동/전투/마나 상태를 갖고, 자기 상태를 broadcast/spawn 패킷으로 직렬화한다.
        class Player : public GameObject
        {
        public:
            // 생성 / 소멸
            explicit Player(UINT64 id);   // id=sid 를 GameObject(id) base 에 전달. PlayerId 는 spawn 때 발급
            ~Player() override;           // 다형 정리는 GameObject virtual ~ 가 담당

            // GameObject 다형 구현 (TickLoop 와 broadcast 가 호출)
            void Update(float deltaTime) override;                                       // 매 tick 위치 예측/리스폰/쿨다운 갱신
            virtual bool SerializeBroadcast(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const override;   // 현재 이동 상태를 SC_MOVE_BROADCAST 로 기록
            virtual bool SerializeSpawn(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const override;        // 초기 상태를 SC_SPAWN 으로 기록 (새 관찰자용)

            // 이동 동기화 (Channel 이 입력 검증/broadcast 기준으로 호출)
            Position PosSync() const;                  // 권위 위치 반환 (클라 보고 위치 대조용, 상태 안 바꿈)
            void     SetLastSeq(UINT32 seq);           // 마지막 처리한 클라 입력 seq 기록
            UINT32   GetLastSeq() const;               // 마지막 처리한 클라 입력 seq

            // 시야 객체 집합 연산 (spawn/despawn/맵 전환 시 갱신)
            void AddVisible(GameObject* obj);        // 시야에 객체 추가 (spawn 시)
            void RemoveVisible(GameObject* obj);     // 시야에서 객체 제거 (despawn 시)
            bool IsVisible(GameObject* obj) const;   // 지금 시야 안에 있는지
            void ClearVisible();                     // 시야 전체 비움 (맵 전환 시)
            const std::unordered_set<GameObject*>& GetVisibleObjects() const;   // 시야 집합 읽기 전용 참조 (맵 전환 순회용)

            // 전투 / 리스폰 연산
            bool ConsumeForSkill(UINT32 skillID);    // 스킬 시전 가능하면 쿨다운 소비 후 true
            void StartRespawn();                     // 사망 전환 시 리스폰 타이머 누산 시작
            bool IsInvulnerable() const;             // 무적 중? (데미지 적용 직전 가드)
            bool ConsumeRespawn();                   // 이번 tick 부활했으면 true + 플래그 내림 (SC_RESPAWN 통지용)

            // DB 저장 dirty 추적
            void   MarkDbDirty(UINT32 category);   // 변경된 저장 카테고리 비트 켜기
            void   ClearDbDirty();                 // 저장 후 비트 모두 끄기
            UINT32 GetDbDirty() const;             // 변경된 저장 카테고리 비트 조회

            // 단순 접근자
            PlayerId GetPlayerId() const;          // wire 에 싣는 플레이어 ID (SC_MOVE_BROADCAST 등)
            void     SetPlayerId(PlayerId id);     // 플레이어 ID 설정 (spawn 때 발급)
            UINT64   GetSid() const;               // 조회 키 sid (GameObject::m_id, wire 엔 안 실음)
            int  GetMp() const;            // 마나 조회 (DB 저장/로드 대상)
            void SetMp(int mp);            // 마나 설정 (로드/소비 시)
            const wchar_t* GetName() const;        // 캐릭터 이름
            void SetName(const wchar_t* name);     // 캐릭터 이름 설정

            UINT64 GetLastChatMs() const;          // 직전 채팅 시각(ms)
            void   SetLastChatMs(UINT64 ms);       // 직전 채팅 시각(ms) 갱신

            // 행동 쿨다운 게이트 - 마지막 이 행동 이후 minIntervalMs 지났으면 true(+시각 갱신), 아니면 false(도배 드롭). 채널 스레드 단독 호출이라 락 불요.
            bool TryAct(EPlayerAction action, UINT64 nowMs, UINT64 minIntervalMs);

            // 짝 게임세션 역포인터 (broadcast Send 게이트웨이 - 전역 SRWLock 우회). 포인터 저장/반환만이라 불완전 타입 OK
            GameSession* GetGameSession() const { return m_gameSession; }          // 핫패스 Send 대상
            void         SetGameSession(GameSession* gs) { m_gameSession = gs; }    // AttachPlayer set / OnSessionGone clear

            // 선택해 입장한 캐릭터의 char_id (DB PK). SaveFull 이 이 값으로 WHERE char_id 갱신.
            UINT32 GetCharId() const { return m_charId; }                          // 저장 스냅샷 키
            void   SetCharId(UINT32 charId) { m_charId = charId; }                 // 로드(DbResultJob)에서 세팅

            // 소지품 (가방 + 장착). 변경한 쪽이 MarkDbDirty(DB_DIRTY_INVENTORY) 를 함께 호출한다.
            Inventory&       GetInventory() { return m_inventory; }
            const Inventory& GetInventory() const { return m_inventory; }
            int GetAttackPower() const;   // 최종 공격력 = 기본 + 장착 무기 (전투 계산이 읽음)
            int GetDefPower() const;      // 최종 방어력 = 장착 방어구 (받는 피해 감산)

            // 거래 상태 (Channel 거래 핸들러 전용 - 진행 중이면 인벤 조작이 잠긴다)
            TradeState&       GetTrade() { return m_trade; }
            const TradeState& GetTrade() const { return m_trade; }

        private:
            PlayerId       m_playerId;          // 채널 안에서 단조 발급되는 ID (generation 핸들 미노출 - 보안)
            wchar_t        m_name[WHISPER_NAME_MAX];   // 캐릭터 이름 (귓속말 대상 조회용)
            UINT32         m_lastProcessedSeq;  // 마지막으로 처리한 클라 입력 seq (broadcast lastSeq 기준)
            // mapId/셀캐시는 GameObject base 멤버
            std::unordered_set<GameObject*> m_visibleObjects;   // 이 플레이어 시야 안의 객체 모음 (spawn 통지 대상)

            int m_mp;                  // 마나 (DB 저장/로드 대상)
            float m_skillCooldown;     // 스킬 쿨다운 남은 시간(초). Update 가 매 tick 줄임
            float m_respawnTimer;      // 사망 후 누산 시간(초). PLAYER_RESPAWN_DELAY_SEC 넘으면 부활
            float m_invulnUntil;       // 부활 직후 무적 남은 시간(초)
            bool m_justRespawned;      // 이번 tick 부활했는지 (SC_RESPAWN 통지용 1회 플래그)
            UINT64 m_lastChatMs;   // 직전 채팅 시각(ms). 채팅 도배 간격 제한용 (spawn 마다 0 으로 리셋)
            UINT64 m_lastActionMs[static_cast<int>(EPlayerAction::ACTION_COUNT)];   // 행동별 직전 수행 시각(ms). 포탈/채널변경/채널목록 도배 제한 (spawn 마다 ctor 가 0 리셋)
            UINT32 m_dbDirty;          // 변경된 저장 카테고리 비트 (저장 시 어디를 쓸지)
            UINT32 m_charId;           // 선택 입장한 캐릭터 char_id (DB PK, SaveFull WHERE 키)
            GameSession* m_gameSession;   // 짝 게임세션 역포인터 (broadcast Send 게이트웨이 - 전역락 우회. AttachPlayer set / OnSessionGone clear)
            Inventory m_inventory;     // 소지품 (가방 24 + 장착 2). 풀 재사용 시 ctor 가 비움
            TradeState m_trade;        // 거래 상태 (rAthena sd->deal 대응). ctor 가 Reset
        };
    };
}
