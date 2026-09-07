#include "pch_gameclient.h"
#include "GameState.h"
#include "GameDefines.h"     // MAX_MOVE_SPEED (per-엔티티 속도 폴백)
#include "MapData/WalkableTable.h"   // AdvanceMove (원격 외삽 - 자기 예측/서버와 같은 수식 + 같은 벽 정지)

// 원격 렌더 수렴 스무딩 파라미터 (하드 스냅 -> 부드러운 수렴). 숫자 파라미터라 관찰 후 튜닝 가능.
//   렌더 위치 = 권위 위치 + renderErr, renderErr 은 매 30Hz 스텝 kRenderSmoothDecay 배로 감쇠(0 으로).
static const float kRenderSmoothDecay = 0.80f;   // 스텝당 오차 잔존율 - 오차가 ~8스텝(266ms)에 83% 소멸(작을수록 빨리 스냅에 가까움)
static const float kRenderSnapDist    = 150.0f;  // 이보다 큰 보정(px)은 순간이동류로 보고 렌더도 즉시 스냅(맵 가로지르는 슬라이드 방지)
static const float kRenderSmoothEps   = 0.5f;    // 이보다 작은 오차(px)는 0 으로 스냅 - float 잔류 누적 방지

void GameState::Reset()
{
	m_entities.clear();
	m_groundItems.clear();
	m_chatLog.clear();
}

void GameState::OnSpawn(const RemoteEntity& e)
{
	// 표시 이름 보존 - SC_SPAWN 은 이름을 싣고 오지만 SC_MONSTER_SPAWN 은 이름 없이 온다.
	// 새로 온 이름이 비어 있을 때만 이미 학습한 이름을 살린다(시야 재진입 시 이름표 깜빡임 방지).
	std::unordered_map<unsigned int, RemoteEntity>::iterator it = m_entities.find(e.id);
	RemoteEntity stored = e;
	if (stored.name[0] == L'\0' && it != m_entities.end() && it->second.name[0] != L'\0')
		::wcscpy_s(stored.name, CHAT_SENDER_MAX, it->second.name);
	m_entities[e.id] = stored;   // 시야 진입(또는 갱신)
}

void GameState::OnEntityDeath(unsigned int id)
{
	std::unordered_map<unsigned int, RemoteEntity>::iterator it = m_entities.find(id);
	if (it == m_entities.end())
		return;   // 모르는 객체 - 무시
	// 제거하지 않고 시신 상태로만 전환 - 즉시 지우면 같은 프레임에 온 막타 데미지 숫자까지 사라진다.
	// 실제 제거는 서버가 시신 유예(1초) 뒤 보내는 SC_DESPAWN 이 담당(OnDespawn).
	it->second.dead = true;
	it->second.hp = 0;
	it->second.moveState = EMoveState::STOP;   // 시신 이동 외삽 중지
}

void GameState::OnDespawn(unsigned int id)
{
	m_entities.erase(id);      // 시야 이탈/사망 - 타입 구분 없이 id 로 제거
	m_groundItems.erase(id);   // 바닥 드랍도 SC_DESPAWN 공용 - 두 컨테이너에서 제거 시도(줍힘/만료)
}

void GameState::OnGroundItemSpawn(const GroundItemView& g)
{
	m_groundItems[g.objectId] = g;   // 시야 진입(또는 갱신 - 같은 objectId 재수신 방어)
}

void GameState::OnEntityMove(unsigned int id, EMoveState state, EMoveDirection dir, int x, int y)
{
	std::unordered_map<unsigned int, RemoteEntity>::iterator it = m_entities.find(id);
	if (it == m_entities.end())
		return;   // 모르는 객체(spawn 못 받음) - 무시
	RemoteEntity& e = it->second;
	// 권위 위치를 덮어쓰기 전에 "지금 렌더되던 위치"를 기억한다(수렴 스무딩 연속성 보존).
	const float oldRenderX = static_cast<float>(e.x) + e.renderErrX;
	const float oldRenderY = static_cast<float>(e.y) + e.renderErrY;
	e.moveState = state;
	e.direction = dir;
	e.x = x;   // 서버 권위 anchor 로 갱신(보이지 않는 참값 - 렌더는 아래 renderErr 로 부드럽게 따라온다)
	e.y = y;
	e.dead = false;   // 서버는 시신 이동을 broadcast 하지 않으므로 이동 수신 = 생존 - 부활 통지를 놓친 관찰자(부활 지점 400px 밖)의 회색 잔상을 첫 이동에 해제
	// renderErr 을 "새 권위와의 차"로 세팅하면 렌더 위치(권위+err)가 수신 직전과 같아 튀지 않는다.
	//   이후 AdvanceDeadReckon 이 매 스텝 이 오차를 0 으로 감쇠 = 하드 스냅이 부드러운 수렴으로 바뀐다.
	float ex = oldRenderX - static_cast<float>(x);
	float ey = oldRenderY - static_cast<float>(y);
	if (ex * ex + ey * ey > kRenderSnapDist * kRenderSnapDist) { ex = 0.0f; ey = 0.0f; }   // 큰 보정(순간이동류) = 렌더도 스냅
	e.renderErrX = ex;
	e.renderErrY = ey;
}

void GameState::OnDamage(unsigned int id, int damage, int remainingHp)
{
	std::unordered_map<unsigned int, RemoteEntity>::iterator it = m_entities.find(id);
	if (it == m_entities.end())
		return;   // 모르는 객체 - 무시
	it->second.hp = remainingHp;          // 서버 권위 hp 로 갱신(체력바 동기)
	it->second.lastDamage = damage;       // 머리 위에 띄울 데미지 숫자
	it->second.damagePopupTimer = 1.0f;   // 1초간 표시
}

void GameState::OnAttackerFlash(unsigned int attackerId)
{
	std::unordered_map<unsigned int, RemoteEntity>::iterator it = m_entities.find(attackerId);
	if (it == m_entities.end())
		return;   // 공격자가 시야 밖(예: 목표만 보임) - 공격범위 원 생략
	it->second.attackFlashTimer = ATTACK_FLASH_SEC;   // 잠깐 공격범위 원 표시(공격 순간 알림)
}

void GameState::OnRespawn(unsigned int id, int x, int y, int hp, int maxHp)
{
	std::unordered_map<unsigned int, RemoteEntity>::iterator it = m_entities.find(id);
	if (it == m_entities.end())
		return;   // 시야 밖에서 부활 - 다시 보이면 SC_SPAWN 으로 들어옴
	it->second.x = x;        // 부활 위치로 복원
	it->second.y = y;
	it->second.hp = hp;      // 회복된 hp
	it->second.maxHp = maxHp;
	it->second.moveState = EMoveState::STOP;
	it->second.dead = false;   // 부활 = 시신 상태 해제(회색->정상 색). 렌더가 hp 아닌 dead 플래그로 색을 정하므로 명시 해제 필수
	it->second.renderErrX = 0.0f;   // 사망 위치->부활 위치는 순간이동이라 렌더도 즉시 스냅(수렴 슬라이드 방지)
	it->second.renderErrY = 0.0f;
}

void GameState::AdvanceDeadReckon(float dt, int mapId)
{
	// 자기 예측과 동일한 공유 함수(AdvanceMove)로 anchor 이후 같은 방향 외삽 - 원격도 서버와 같은 벽에서 멈춘다.
	//   mapId 는 자기 맵 하나면 충분: 원격 목록은 맵 전환 때마다 Reset 되어 항상 자기 맵 엔티티만 담는다.
	for (std::unordered_map<unsigned int, RemoteEntity>::iterator it = m_entities.begin(); it != m_entities.end(); ++it)
	{
		RemoteEntity& e = it->second;
		if (e.damagePopupTimer > 0.0f)
			e.damagePopupTimer -= dt;   // 데미지 숫자 표시 시간 소진(이동 여부와 무관)
		if (e.chatBubbleTimer > 0.0f)
			e.chatBubbleTimer -= dt;    // 말풍선 표시 시간 소진
		if (e.attackFlashTimer > 0.0f)
			e.attackFlashTimer -= dt;   // 공격범위 원 표시 시간 소진

		// 권위 위치 외삽 - 살아있고 이동 중이고 방향이 유효할 때만(시신/정지는 권위 고정).
		//   시체 스폰이 사망 직전의 START 를 실어 와도 dead 게이트가 활주를 막는다(SC_DEATH/부활 STOP 과 대칭).
		const int d = static_cast<int>(e.direction);
		if (!e.dead && e.moveState == EMoveState::START
			&& d >= 0 && d < static_cast<int>(EMoveDirection::COUNT))   // 손상/변조 방향 방어 - 서버는 0..7만 보내지만 신뢰경계서 검증
		{
			const int predictSpeed = (e.moveSpeed > 0) ? e.moveSpeed : MAX_MOVE_SPEED;   // 몬스터 = 스폰 스냅샷 속도, 플레이어 = 공유 기본값
			const Position moved = AdvanceMove(mapId, Position{ e.x, e.y }, e.direction, predictSpeed, dt);
			e.x = moved.x;
			e.y = moved.y;
		}

		// 렌더 오차 감쇠 - 모든 엔티티(정지/시신 포함)에 돌려 렌더가 권위로 부드럽게 안착한다.
		//   스냅샷 수신 때 생긴 오차를 매 스텝 0 으로 수렴 = 대각/벽/공격경계의 잦은 하드 스냅 떨림을 흡수.
		e.renderErrX *= kRenderSmoothDecay;
		e.renderErrY *= kRenderSmoothDecay;
		if (e.renderErrX < kRenderSmoothEps && e.renderErrX > -kRenderSmoothEps) { e.renderErrX = 0.0f; }
		if (e.renderErrY < kRenderSmoothEps && e.renderErrY > -kRenderSmoothEps) { e.renderErrY = 0.0f; }
	}
}

void GameState::AddChat(const wchar_t* sender, const wchar_t* message, bool isWhisper)
{
	ChatLine line;
	::wcscpy_s(line.sender, CHAT_SENDER_MAX, sender);
	::wcscpy_s(line.message, CHAT_MSG_MAX, message);
	line.isWhisper = isWhisper;
	m_chatLog.push_back(line);
	if (m_chatLog.size() > 50)
		m_chatLog.pop_front();   // 최근 50줄만 유지(오래된 것부터 제거)
}

void GameState::SetChatBubble(unsigned int id, const wchar_t* message)
{
	std::unordered_map<unsigned int, RemoteEntity>::iterator it = m_entities.find(id);
	if (it == m_entities.end())
		return;   // 시야 밖 발화자 - 말풍선 생략(채팅창엔 이미 들어감)
	::wcscpy_s(it->second.chatBubble, CHAT_MSG_MAX, message);
	it->second.chatBubbleTimer = 4.0f;   // 4초간 머리 위 표시
}

void GameState::SetEntityName(unsigned int id, const wchar_t* name)
{
	std::unordered_map<unsigned int, RemoteEntity>::iterator it = m_entities.find(id);
	if (it == m_entities.end())
		return;   // 시야 밖 발화자 - 이름 학습 생략(채팅창엔 이미 표시됨, 시야 진입 시 다시 발화하면 학습)
	::wcscpy_s(it->second.name, CHAT_SENDER_MAX, name);
}
