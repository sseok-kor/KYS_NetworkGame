#pragma once
#include "GameTypes.h"   // EMoveDirection / EMoveState
#include "GameDefines.h"   // CHAT_MSG_MAX (말풍선 버퍼)

// 내 아바타 - 서버 권위 위치 + 클라 예측/식별.
// 자기 자신은 GameState 의 원격 엔티티 맵에 없다(서버가 자기 SC_SPAWN 을 안 보냄).
// 자기 상태는 로그인 시 SC_CHAR_INFO(hp 포함), 이후 포탈 SC_MAP_CHANGE 로 배치되고, 이동은 클라가 30Hz 로 예측 후 SC_MOVE_BROADCAST self-echo 로 reconcile.
class LocalPlayer
{
public:
	LocalPlayer();

	void Reset();   // 연결 종료 시 미배치 상태로 되돌림

	// SC_MAP_CHANGE 수신 - 서버 권위 좌표/맵/내 id 확정(자기 배치, 포탈).
	void OnMapChange(unsigned int playerId, int mapId, int x, int y);
		// SC_CHAR_INFO 수신 - 로그인 시 자기 캐릭터 전체 초기 상태(배치 + hp/maxHp). DB 로드값으로 기본 hp(max) 보정.
		void OnCharInfo(unsigned int playerId, int mapId, int x, int y, int hp, int maxHp);

	// 입력 -> 이동 의도. 매 프레임 갱신(키 없으면 STOP).
	void SetMoveIntent(EMoveDirection dir, EMoveState state);
	// 30Hz 고정 스텝 자기 예측(이동 중이면 MOVE_DIR_DELTA LUT x 속도 x dt, 맵 경계 클램프). 서버 tick 과 비트 일치.
	void Update(float dt);
	// SC_MOVE_BROADCAST self-echo -> lastSeq 로 그 시점 내 예측을 찾아 서버 권위와 대조 reconcile.
	//   허용오차(서버 수용 경계와 동일) 안이면 무시(현재 예측 유지=되당김 없음) / 초과면 그만큼 보정.
	void OnMoveEcho(unsigned int lastSeq, int x, int y);

	void SetHp(int hp) { m_hp = hp; }                    // SC_DAMAGE(내가 피격) - 남은 hp 반영
	void OnRespawn(int x, int y, int hp, int maxHp);     // SC_RESPAWN(내 부활) - 위치/hp 복원
	void SetChatBubble(const wchar_t* message);          // 자기 채팅 시 머리 위 말풍선(자기는 GameState에 없어 여기 보유)
	void TriggerAttackFlash();                           // SC_DAMAGE(내가 공격자) - 내 공격범위 원 flash(자기는 GameState에 없어 여기 보유)
	void TriggerDamagePopup(int damage);                 // SC_DAMAGE(내가 피격) - 내 머리 위 데미지 숫자(자기는 GameState에 없어 여기 보유)

	unsigned int NextSeq();   // CS_MOVE 송신 seq 발급 + 그 시점 예측 위치를 reconcile 히스토리에 남긴다

	bool           IsPlaced() const { return m_placed; }
	unsigned int   GetId() const { return m_selfPlayerId; }
	int            GetX() const { return m_x; }
	int            GetY() const { return m_y; }
	int            GetMapId() const { return m_mapId; }
	EMoveDirection GetDir() const { return m_dir; }
	EMoveState     GetMoveState() const { return m_moveState; }
	int            GetHp() const { return m_hp; }
	int            GetMaxHp() const { return m_maxHp; }
	bool           IsDead() const { return m_placed && m_hp <= 0; }   // hp 0 이하 = 사망(입력 차단, 사망 화면)
	const wchar_t* GetChatBubble() const { return m_chatBubble; }
	float          GetChatBubbleTimer() const { return m_chatBubbleTimer; }
	float          GetAttackFlashTimer() const { return m_attackFlashTimer; }
	float          GetDamagePopupTimer() const { return m_damagePopupTimer; }
	int            GetLastDamage() const { return m_lastDamage; }
	void           SetName(const wchar_t* name);                       // 내 이름표 (로그인 직후 = 계정 id 임시, 캐릭터 선택 시 캐릭터 이름으로 대체)
	const wchar_t* GetName() const { return m_name; }

private:
	void ClearMoveHistory();   // reconcile 히스토리 무효화 - 권위가 위치를 리셋(스폰/포탈/부활/보정)하면 옛 예측 표본을 버려 stale 오적용 차단

	unsigned int   m_selfPlayerId;   // 내 플레이어 id (self-echo 식별 키)
	int            m_x;              // 예측/서버 권위 위치
	int            m_y;
	int            m_mapId;          // 현재 맵
	EMoveDirection m_dir;            // 현재 이동 방향
	EMoveState     m_moveState;      // 멈춤/이동
	unsigned int   m_seq;            // CS_MOVE 시퀀스(서버가 echo 의 lastSeq 로 되실음)

	// self-echo reconcile 히스토리 - NextSeq 가 (그 seq, 그때 예측 x/y) 를 남기고,
	//   OnMoveEcho 가 lastSeq 로 조회해 "그 시점 내 예측 == 서버 권위?" 를 대조한다(러버밴드 제거).
	static const int MOVE_HISTORY_CAP = 64;   // 최근 CS_MOVE 예측 좌표 링(seq % CAP). 왕복지연 << 링 수명이라 충분.
	struct MoveSample { unsigned int seq; int x; int y; };
	MoveSample     m_moveHistory[MOVE_HISTORY_CAP];   // seq % CAP 슬롯 - seq 필드로 덮어쓰기/미기록 판별
	bool           m_placed;         // SC_CHAR_INFO(로그인)/SC_MAP_CHANGE(포탈) 를 한 번이라도 받았는가(배치됨)
	int            m_hp;             // 내 현재 hp (로그인 시 SC_CHAR_INFO 로 DB값 보정, 이후 SC_DAMAGE/RESPAWN 으로 갱신)
	int            m_maxHp;          // 내 최대 hp (기본 PLAYER_DEFAULT_MAX_HP, SC_RESPAWN 으로 정확값)
	float          m_chatBubbleTimer;        // 자기 말풍선 표시 잔여(초)
	wchar_t        m_chatBubble[CHAT_MSG_MAX];   // 자기 말풍선 내용
	float          m_attackFlashTimer;       // 자기 공격범위 원 표시 잔여(초) - 0이면 미표시
	float          m_damagePopupTimer;       // 내 피격 데미지 숫자 표시 잔여(초) - 0이면 미표시
	int            m_lastDamage;             // 마지막으로 받은 피해량(머리 위 숫자)
	wchar_t        m_name[WHISPER_NAME_MAX];     // 내 이름표(로그인 id) - 비어 있으면 "@ me"
};
