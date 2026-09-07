#include "pch_gameclient.h"
#include "LocalPlayer.h"
#include "GameState.h"       // ATTACK_FLASH_SEC (자기/원격이 공유하는 공격범위 원 표시 시간)
#include "GameDefines.h"     // MAX_MOVE_SPEED / MOVE_SYNC_TOLERANCE
#include "MapData/MapTable.h"        // MapWidthFor/MapHeightFor (에코 보정 클램프 - 맵마다 크기 다름)
#include "MapData/WalkableTable.h"   // AdvanceMove (이동 예측 - 서버와 같은 수식 + 같은 벽 정지)

LocalPlayer::LocalPlayer()
	: m_selfPlayerId(0)
	, m_x(0)
	, m_y(0)
	, m_mapId(0)
	, m_dir(EMoveDirection::DOWN)
	, m_moveState(EMoveState::STOP)
	, m_seq(0)
	, m_placed(false)
	, m_hp(PLAYER_DEFAULT_MAX_HP)
	, m_maxHp(PLAYER_DEFAULT_MAX_HP)
	, m_chatBubbleTimer(0.0f)
	, m_attackFlashTimer(0.0f)
	, m_damagePopupTimer(0.0f)
	, m_lastDamage(0)
{
	m_chatBubble[0] = L'\0';
	m_name[0] = L'\0';
	ClearMoveHistory();   // 미기록 상태로 시작(첫 실제 seq 는 1)
}

void LocalPlayer::Reset()
{
	m_selfPlayerId = 0;
	m_x = 0;
	m_y = 0;
	m_mapId = 0;
	m_dir = EMoveDirection::DOWN;
	m_moveState = EMoveState::STOP;
	m_seq = 0;
	m_placed = false;
	m_hp = PLAYER_DEFAULT_MAX_HP;
	m_maxHp = PLAYER_DEFAULT_MAX_HP;
	m_chatBubbleTimer = 0.0f;
	m_attackFlashTimer = 0.0f;
	m_damagePopupTimer = 0.0f;
	m_lastDamage = 0;
	m_chatBubble[0] = L'\0';
	m_name[0] = L'\0';
	ClearMoveHistory();   // 새 세션 - 옛 예측 히스토리 버림
}

// self-echo reconcile 히스토리를 전부 미기록(seq=0)으로 되돌린다. 첫 실제 seq 는 1 이라 0 은 "빈 슬롯" 센티널.
//   권위가 위치를 리셋하는 지점(스폰/포탈=OnMapChange, 부활=OnRespawn, 보정=OnMoveEcho 초과분기)에서 호출한다 -
//   그 전 예측 표본은 새 기준과 무관하므로, 옛 lastSeq 에코가 와도 미기록 판정으로 snap 폴백된다.
void LocalPlayer::ClearMoveHistory()
{
	for (int i = 0; i < MOVE_HISTORY_CAP; ++i) m_moveHistory[i].seq = 0;
}

void LocalPlayer::OnMapChange(unsigned int playerId, int mapId, int x, int y)
{
	m_selfPlayerId = playerId;
	m_mapId = mapId;
	m_x = x;
	m_y = y;
	m_moveState = EMoveState::STOP;   // 스폰/포탈 직후 정지
	m_placed = true;
	ClearMoveHistory();   // 포탈/스폰 순간이동 - 구맵 좌표 예측 표본을 버려 옛 seq 에코의 cross-map 오적용 차단
}

void LocalPlayer::OnCharInfo(unsigned int playerId, int mapId, int x, int y, int hp, int maxHp)
{
	OnMapChange(playerId, mapId, x, y);   // 배치(id/맵/위치/정지) 재사용
	m_hp = hp;        // DB 로드 hp - 기본값 max를 정확값으로 보정 (로그인 시 자기 hp를 아는 유일 경로)
	m_maxHp = maxHp;
}

void LocalPlayer::SetMoveIntent(EMoveDirection dir, EMoveState state)
{
	m_dir = dir;
	m_moveState = state;
}

void LocalPlayer::Update(float dt)
{
	if (m_chatBubbleTimer > 0.0f)
		m_chatBubbleTimer -= dt;   // 말풍선 표시 시간 소진(이동 여부와 무관)
	if (m_attackFlashTimer > 0.0f)
		m_attackFlashTimer -= dt;  // 공격범위 원 표시 시간 소진
	if (m_damagePopupTimer > 0.0f)
		m_damagePopupTimer -= dt;  // 내 피격 데미지 숫자 표시 시간 소진
	if (!m_placed || m_moveState != EMoveState::START)
		return;

	// 전진/경계 클램프/벽 정지를 서버와 같은 공유 함수로 - 같은 비트 결과 + 같은 벽에서 멈춰야
	//   서버 보정(되당김)이 안 생긴다. 지형 미적재(강등 모드)면 함수 내부가 전-통행/경계 4000 으로 폴백.
	const Position moved = AdvanceMove(m_mapId, Position{ m_x, m_y }, m_dir, MAX_MOVE_SPEED, dt);
	m_x = moved.x;
	m_y = moved.y;
}

unsigned int LocalPlayer::NextSeq()
{
	++m_seq;
	MoveSample& s = m_moveHistory[m_seq % MOVE_HISTORY_CAP];   // 이 seq 로 보낼 위치를 기록(에코 도착 시 대조 기준)
	s.seq = m_seq;
	s.x = m_x;   // CS_MOVE 의 req.x = GetX() 와 같은 시점 값
	s.y = m_y;
	return m_seq;
}

void LocalPlayer::OnMoveEcho(unsigned int lastSeq, int x, int y)
{
	// 서버가 이 lastSeq 로 되싣은 권위 좌표를, 내가 그 seq 로 예측했던 위치와 대조한다.
	const MoveSample& s = m_moveHistory[lastSeq % MOVE_HISTORY_CAP];
	if (lastSeq == 0 || s.seq != lastSeq)
	{
		// 대조 불가 - 서버 권위로 스냅(보수적 폴백).
		//   lastSeq==0 = 아직 내 CS_MOVE 가 서버에 안 닿음(첫 실제 seq 는 1). 미기록 슬롯의 센티널 seq 도 0 이라
		//   이 조건이 없으면 거짓일치하는데, 미기록 칸의 x/y 는 아무도 쓴 적 없는 미정의 값이라 대조 기준이 못 된다.
		//   s.seq 불일치 = 링이 이미 덮어쓴 오래된 에코.
		m_x = x;
		m_y = y;
		return;
	}

	const int errX = x - s.x;   // 서버 보정량 = 권위 - 내 예측(그 seq 시점)
	const int errY = y - s.y;
	if (errX * errX + errY * errY <= MOVE_SYNC_TOLERANCE * MOVE_SYNC_TOLERANCE)
		return;   // 서버가 내 보고를 수용(오차 <= 서버 수용 경계 64px) - 현재 예측 유지(되당김 없음)

	// 서버가 보정함(오차 > 경계) - 그 시점 권위로 스냅한 것과 등가로 현재 위치에 보정량을 얹는다
	//   (= lastSeq 지점 서버 권위 + 그 뒤 내가 예측한 이동을 다시 얹음). 그 맵 경계로 클램프 (맵마다 크기 다름).
	//   보정 결과가 벽 칸일 수 있으나 그대로 둔다 - 직후 ClearMoveHistory 로 다음 에코가 권위 스냅이라 자기수렴.
	m_x += errX;
	m_y += errY;
	const int w = MapWidthFor(m_mapId);
	const int h = MapHeightFor(m_mapId);
	if (m_x < 0) m_x = 0; else if (m_x > w) m_x = w;
	if (m_y < 0) m_y = 0; else if (m_y > h) m_y = h;
	ClearMoveHistory();   // 보정 발생 - 이후 in-flight 에코가 같은 델타를 재적용(지그재그)하지 않게 무효화(다음 에코는 권위로 snap)
}

void LocalPlayer::OnRespawn(int x, int y, int hp, int maxHp)
{
	m_x = x;            // 서버가 지정한 부활 위치
	m_y = y;
	m_hp = hp;          // 회복된 hp(보통 maxHp)
	m_maxHp = maxHp;
	m_moveState = EMoveState::STOP;
	ClearMoveHistory();   // 부활 순간이동 - 사망 전 좌표 예측 표본 버림(방어적)
}

void LocalPlayer::SetChatBubble(const wchar_t* message)
{
	::wcscpy_s(m_chatBubble, CHAT_MSG_MAX, message);
	m_chatBubbleTimer = 4.0f;   // 4초간 머리 위 표시
}

void LocalPlayer::TriggerAttackFlash()
{
	m_attackFlashTimer = ATTACK_FLASH_SEC;   // 내가 공격한 순간 공격범위 원 표시(SC_DAMAGE atkId=나)
}

void LocalPlayer::TriggerDamagePopup(int damage)
{
	m_lastDamage = damage;         // 머리 위에 띄울 받은 피해량
	m_damagePopupTimer = 1.0f;     // 1초간 표시(원격 엔티티와 동일)
}

void LocalPlayer::SetName(const wchar_t* name)
{
	::wcsncpy_s(m_name, _countof(m_name), name, _TRUNCATE);   // 로그인 id(버퍼 초과 시 잘라 라벨로)
}
