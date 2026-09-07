#pragma once
#include "GameTypes.h"   // EObjectType / EMonsterType / EMoveState / EMoveDirection
#include "GameDefines.h"   // CHAT_MSG_MAX / WHISPER_NAME_MAX (채팅 버퍼 크기)
#include <unordered_map>
#include <deque>

// 채팅창 발신자 표시 칸 - 이름(WHISPER_NAME_MAX) + 귓속말 '>' 접두 1칸. (RemoteEntity.name 이름표도 재사용)
static const int CHAT_SENDER_MAX = WHISPER_NAME_MAX + 1;

// 엔티티가 공격했을 때 공격범위 원을 띄우는 시간(초). 자기(LocalPlayer)와 원격(RemoteEntity)이 공유.
static const float ATTACK_FLASH_SEC = 0.35f;

// 시야(AOI) 안 원격 객체 하나의 캐시 상태 (타플레이어 + 몬스터 공용 - id 공간 공유).
struct RemoteEntity
{
	unsigned int   id;
	EObjectType    objectType;    // PLAYER / MONSTER
	EMonsterType   monsterType;   // MONSTER 일 때만 의미 (종류별 글리프)
	int            attackRange;   // MONSTER 일 때만 의미 (공격 사거리 px - SC_MONSTER_SPAWN 로 서버가 전달, 공격범위 표시)
	int            moveSpeed;     // 위치 예측 속도 px/s (몬스터 = SC_MONSTER_SPAWN 스냅샷 값. 0이면 공유 기본값 MAX_MOVE_SPEED 로 예측 - 플레이어 엔티티)
	bool           isBoss;        // MONSTER 일 때만 의미 (보스 렌더 강조 - 서버가 monsters.yml 값 전달, 종류 하드코딩 판정 대체)
	int            x;             // 권위(외삽) 위치 - 렌더는 여기에 renderErr 를 더한 점에 그린다(수렴 스무딩)
	int            y;
	EMoveState     moveState;     // 외삽 on/off
	EMoveDirection direction;     // 외삽 방향
	float          renderErrX;    // 렌더 위치 - 권위 위치 (px). 스냅샷 수신 시 연속성 보존값으로 세팅, 매 스텝 0으로 감쇠 = 하드 스냅 대신 부드러운 수렴
	float          renderErrY;
	int            hp;
	int            maxHp;
	bool           dead;               // SC_DEATH 수신 - 시신 상태(회색 표시, 이동 정지). 제거는 SC_DESPAWN 이 담당(서버 시신 유예 1초와 대칭)
	float          damagePopupTimer;   // 피격 데미지 숫자 표시 잔여 시간(초) - 0이면 미표시
	int            lastDamage;         // 마지막 피격량(플로팅 데미지로 띄울 값)
	float          attackFlashTimer;   // 이 객체가 공격 중임을 알리는 공격범위 원 표시 잔여 시간(초) - 0이면 미표시
	float          chatBubbleTimer;        // 머리 위 말풍선 표시 잔여 시간(초) - 0이면 미표시
	wchar_t        chatBubble[CHAT_MSG_MAX];   // 말풍선에 띄울 채팅 내용
	wchar_t        name[CHAT_SENDER_MAX];      // 표시 이름(채팅 senderName 으로 학습) - 비어 있으면 P{id} 임시 라벨
};

// 바닥에 떨어진 아이템 하나 (SC_GROUND_ITEM_SPAWN 로 시야 등장, SC_DESPAWN 로 이탈). 위치가 있어 월드에 렌더된다.
struct GroundItemView
{
	unsigned int objectId;    // 맵 개체 번호 (줍기 요청 키 = CS_ITEM_PICKUP.objectId)
	int          templateId;  // 종류 (이름은 클라도 ItemTable 에서 조회)
	int          x;
	int          y;
	int          quantity;
};

// 채팅창 한 줄 (일반 채팅 + 귓속말 공용).
struct ChatLine
{
	wchar_t sender[CHAT_SENDER_MAX];   // 발신자 이름(귓속말은 '>' 접두 포함)
	wchar_t message[CHAT_MSG_MAX];      // 내용
	bool    isWhisper;                  // true=귓속말(별도 표기)
};

// 서버 권위 월드 캐시 - 시야 안 원격 객체 맵 + dead-reckon 외삽. 자기 자신은 LocalPlayer 가 보유(여기 없음).
class GameState
{
public:
	GameState() = default;
	~GameState() = default;

	GameState(const GameState&) = delete;
	GameState& operator=(const GameState&) = delete;

	void Reset();   // 맵 전환/연결 종료 시 전부 비움

	void OnSpawn(const RemoteEntity& e);                                                       // SC_SPAWN / SC_MONSTER_SPAWN
	void OnDespawn(unsigned int id);                                                           // SC_DESPAWN - 시야 이탈/시신 회수(실제 제거)
	void OnEntityDeath(unsigned int id);                                                       // SC_DEATH - 시신 상태 전환(제거 아님 - 막타 데미지/시신 1초 표시 유지)
	void OnEntityMove(unsigned int id, EMoveState state, EMoveDirection dir, int x, int y);    // 타인/몬스터 이동 anchor
	void OnDamage(unsigned int id, int damage, int remainingHp);                               // SC_DAMAGE - hp 갱신 + 플로팅 데미지 세팅
	void OnAttackerFlash(unsigned int attackerId);                                             // SC_DAMAGE - 공격자 머리에 공격범위 원 flash 세팅(공격자가 시야 안일 때만)
	void OnRespawn(unsigned int id, int x, int y, int hp, int maxHp);                          // SC_RESPAWN - hp/위치 복원
	void AdvanceDeadReckon(float dt, int mapId);                                               // 30Hz 외삽(이동 중 객체·mapId=자기 맵 - 벽 정지 조회) + renderErr 수렴 감쇠(하드 스냅 스무딩) + 데미지/말풍선 타이머 감소
	void AddChat(const wchar_t* sender, const wchar_t* message, bool isWhisper);               // SC_CHAT_BROADCAST / SC_WHISPER - 채팅창 누적
	void SetChatBubble(unsigned int id, const wchar_t* message);                               // 발화자 머리 위 말풍선 세팅
	void SetEntityName(unsigned int id, const wchar_t* name);                                  // SC_CHAT_BROADCAST - 발화자 표시 이름 학습(이름표)

	void OnGroundItemSpawn(const GroundItemView& g);                                           // SC_GROUND_ITEM_SPAWN - 바닥 드랍 시야 등장

	const std::unordered_map<unsigned int, RemoteEntity>& Entities() const { return m_entities; }
	const std::deque<ChatLine>& ChatLog() const { return m_chatLog; }
	const std::unordered_map<unsigned int, GroundItemView>& GroundItems() const { return m_groundItems; }

private:
	std::unordered_map<unsigned int, RemoteEntity> m_entities;
	std::unordered_map<unsigned int, GroundItemView> m_groundItems;   // 시야 안 바닥 드랍 (objectId 키 - SC_DESPAWN 이 엔티티와 공용 id 로 제거)
	std::deque<ChatLine>                           m_chatLog;   // 최근 채팅(앞=오래된 것, 상한 넘으면 pop_front)
};
