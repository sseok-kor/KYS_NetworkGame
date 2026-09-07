#include "pch_serverapp.h"
#include "Monster.h"
#include "MonsterTable.h"                 // MonsterIsBoss (스폰 스냅샷 isBoss 필드)
#include "../Combat/CombatFormula.h"
#include "../Player/Player.h"
#include "../World/Map.h"                 // Map::ResolveTarget (교전 대상 근접 스캔 위임 - back-pointer)
#include "../GameCommon/GameDefines.h"    // MONSTER_THINK_INTERVAL / MONSTER_PATROL_RADIUS 등
#include "../GameCommon/Protocol/GamePackets.h"    // SC_MONSTER_MOVE / SC_MONSTER_SPAWN (직렬화 본체)
#include "../GameCommon/Protocol/PacketType.h"     // PacketType::SC_MONSTER_MOVE / SC_MONSTER_SPAWN
#include "../GameCommon/MapData/MapTable.h"       // MapWidthFor/MapHeightFor (배회 목적지 경계 클램프 - 맵마다 크기 다름)
#include "../GameCommon/MapData/WalkableTable.h"  // AdvanceMove(이동 한 걸음)/IsWalkable(배회 목적지 벽 회피)/HasLineOfSight(repath LoS 재검사)
#include "../GameCommon/Pathfinding/Direction.h"      // DirectionToward (목표 방향 8방 양자화 - 조향 공유 헬퍼)
#include "../Path/PathfinderThread.h"     // PathfindRequest / PathfinderThread (우회 경로 요청 발행)

namespace KYS
{
    namespace SERVERAPP
    {
        static Position RandomPatrolDestination(const Position& anchor, int mapId);   // 집 반경 안 랜덤 배회 목적지 (경계 클램프 + 벽 칸 재추첨)
        static long long DistSq(int dx, int dy) { return static_cast<long long>(dx) * dx + static_cast<long long>(dy) * dy; }   // 오버플로 안전 제곱거리 (int 곱 전 long long 승격)

        // 종류별 스탯 테이블은 Data/monsters.yml 로 외부화 - 적재/보관/조회(MonsterTemplateFor 정의)는 MonsterTable.cpp.
        //   스탯 조정은 재컴파일 대신 파일 한 줄 (부팅 검증이 전 종류 적재를 보장).

        Monster::Monster(UINT64 id)
            : GameObject(id, EObjectType::MONSTER) // base가 m_id = 전송용 id 확정 + 타입(MONSTER) 한 번만 set
            , m_map(nullptr)                                 // SpawnMonster가 자기 맵 back-pointer로 덮어씀(Insert 전)
            , m_targetId(0)                            // 교전 대상 없음 (점착 id, 값)
            , m_attackVictim(nullptr)                        // 이번 tick 공격 대상 없음 (same-tick transient)
            , m_state(EMonsterState::IDLE)
            , m_thinkTimer(0.0f)
            , m_patrolAnchor{ 0, 0 }                     // SetPatrolAnchor가 스폰 시 덮어씀(m_patrolPoint도 함께)
            , m_patrolPoint{ 0, 0 }
            , m_returningToAnchor(false)
            , m_attackCooldown(0.0f)
            , m_didAttack(false)
            , m_lastDamage(0)
            , m_victimDied(false)
            , m_deathTimer(0.0f)
            , m_monsterType(EMonsterType::GOBLIN)             // SpawnMonster가 SetMonsterType으로 덮어씀 (안전 기본값)
            , m_spawnGroupIdx(-1)                             // SpawnMonster가 SetSpawnGroupIdx로 덮어씀 (풀 재사용 시 ctor가 리셋)
            , m_channelId(-1)                                 // SpawnMonster가 SetChannelId로 stamp
            , m_pathSeq(0)
            , m_pathPending(false)
            , m_pathPendingElapsed(0.0f)
            , m_pathRetryCooldown(0.0f)
            , m_pathFailElapsed(0.0f)
            , m_waypointCount(0)                              // m_waypoints 배열은 count 만큼만 유효 - 초기 0이라 미기록 칸 안 읽음
            , m_waypointIndex(0)
        {
            // m_moveState/m_direction은 GameObject base 멤버 - base ctor가 초기화. Monster ctor 재초기화 X.
            for (int i = 0; i < MAX_DAMAGE_CREDITS; ++i)   // 기여 표 비움 (풀 재사용 시 ctor 재실행이 보장)
            {
                m_damageCredits[i].charId = 0;
                m_damageCredits[i].total = 0;
            }
        }

        // 타격마다 기여 누적. 같은 charId면 합산, 새 기여자는 빈 칸에, 표가 가득이면 최소 기여자를 교체.
        void Monster::AddDamageCredit(UINT32 charId, int damage)
        {
            if (charId == 0 || damage <= 0) { return; }

            int emptyIdx = -1;
            int minIdx = 0;
            for (int i = 0; i < MAX_DAMAGE_CREDITS; ++i)
            {
                if (m_damageCredits[i].charId == charId)
                {
                    m_damageCredits[i].total += damage;   // 기존 기여자 합산
                    return;
                }
                if (m_damageCredits[i].charId == 0 && emptyIdx < 0) { emptyIdx = i; }
                if (m_damageCredits[i].total < m_damageCredits[minIdx].total) { minIdx = i; }
            }
            const int slot = (emptyIdx >= 0) ? emptyIdx : minIdx;   // 빈 칸 우선, 없으면 최소 기여자 자리
            m_damageCredits[slot].charId = charId;
            m_damageCredits[slot].total = damage;
        }

        // 기여 상위 3명의 char_id (드랍 소유권 1~3순위). 없는 순위는 0으로 채운다.
        void Monster::GetTopDamageCredits(UINT32 outCharIds[3]) const
        {
            outCharIds[0] = 0; outCharIds[1] = 0; outCharIds[2] = 0;
            int topTotals[3] = { 0, 0, 0 };
            for (int i = 0; i < MAX_DAMAGE_CREDITS; ++i)
            {
                const UINT32 charId = m_damageCredits[i].charId;
                const int total = m_damageCredits[i].total;
                if (charId == 0 || total <= 0) { continue; }

                // 상위 3 삽입 정렬 - 한 자리씩 밀어 내리며 끼워 넣는다.
                for (int rank = 0; rank < 3; ++rank)
                {
                    if (total > topTotals[rank])
                    {
                        for (int shift = 2; shift > rank; --shift)
                        {
                            topTotals[shift] = topTotals[shift - 1];
                            outCharIds[shift] = outCharIds[shift - 1];
                        }
                        topTotals[rank] = total;
                        outCharIds[rank] = charId;
                        break;
                    }
                }
            }
        }

        Monster::~Monster()
        {
            // 고유 동적 자원 없음. m_map(back-pointer)/m_attackVictim(transient)은 비소유 - delete 안 함.
        }

        void Monster::Update(float deltaTime)
        {
            // 종류별 스탯 (이동속도/공격 사거리/쿨다운). moveSpeed는 종류별 값 - 스폰 스냅샷(SC_MONSTER_SPAWN)으로
            //   클라에 전달되어 클라도 같은 속도로 위치를 예측한다.
            const MonsterTemplate& monsterStats = MonsterTemplateFor(m_monsterType);

            // 시신: 행동 0, 유지 시간만 누산(IsDeadExpired가 만료 판정). 교전 대상 스캔/역참조 없음 - 산 몬스터만 교전.
            if (m_state == EMonsterState::DEAD)
            {
                m_deathTimer += deltaTime;
                return;
            }

            // 변경 감지용 이전 값 보관 (이동 상태/방향이 바뀔 때만 broadcast)
            const EMoveState     prevMoveState = GetMoveState();
            const EMoveDirection prevDirection = GetDirection();
            const Position myPos = GetPos();

            // 이번 tick 이동 의도(로컬). 상태 기계 끝나고 SetMoveState로 1회 반영.
            EMoveState     moveState = prevMoveState;
            EMoveDirection direction = prevDirection;

            // "도착" 판정 = 이번 tick 이동량 이내
            int step = static_cast<int>(monsterStats.moveSpeed * deltaTime);
            if (step < 1) { step = 1; }
            const long long arrive2 = static_cast<long long>(step) * step;

            // 재평가 간격 게이트 - 대기 중 획득 throttle + 배회 목적지 재선정에만 쓴다(추격 중엔 매 tick 추적).
            const bool think = ShouldRethink(deltaTime);

            // 길찾기 타이머 진행 (매 tick) - 재요청 쿨다운 감쇠 + pending TTL(결과 드랍 유실 자기치유).
            if (m_pathRetryCooldown > 0.0f) { m_pathRetryCooldown -= deltaTime; }
            if (m_pathPending)
            {
                m_pathPendingElapsed += deltaTime;
                if (m_pathPendingElapsed >= MONSTER_PATH_PENDING_TTL_SEC)
                {
                    m_pathPending = false;        // 결과가 안 옴(요청/결과 드랍) - 강제 해제해 재요청 가능하게
                    m_pathPendingElapsed = 0.0f;
                }
            }

            // 교전 대상 즉석 산출 (저장 raw 포인터 없음 -> cross-map dangling 자체가 불가능): 자기 맵 AOI 스캔으로 매 tick 지역 포인터를 얻는다.
            //   - 복귀(returning) 중엔 재교전 안 함(leash 보존) -> 스캔 skip, target=nullptr.
            //   - 이미 교전 중(m_targetId!=0)이면 매 tick 추적, 대기 중이면 think tick에만 획득 시도(스캔 비용 절감).
            //   - ResolveTarget가 어그로 범위 안 점착(같은 id) 우선, 없으면 최근접 살아있는 player를 현재 grid에서 골라 반환(없으면 nullptr).
            //   - m_targetId는 전송 id(값)라 dangling 불가 - 다음 tick 점착 기준으로만 쓰인다.
            GameObject* target = nullptr;
            if (!m_returningToAnchor && m_map != nullptr && (m_targetId != 0 || think))
            {
                // 점착 대상 보유 중(m_targetId!=0 = 이미 교전 시작)엔 유지 반경을 leash 까지 넓혀, 우회로/도주로
                //   거리가 벌어져도 놓치지 않는다(D-H3). 신규 획득 반경은 aggro (처음 발견 거리 불변).
                //   "commit 후 넓게 추격"은 rAthena range2(chase range > view range) 동형이고, 무한 추격은
                //   아래 anchor leash 컷(집에서 leash 초과 시 포기)이 상한. 경로 객체 보유가 아니라 점착 자체를
                //   기준으로 삼아, 우회로 코너를 돌아 시야를 확보한 tick 이나 NOPATH 쿨다운 창에서 반경이
                //   aggro 로 붕괴해 점착이 조기 해제되던 이음새를 봉인한다.
                const int maintainRange = (m_targetId != 0) ? MONSTER_LEASH_RANGE : monsterStats.aggroRange;
                target = m_map->ResolveTarget(this, monsterStats.aggroRange, maintainRange, m_targetId);
                m_targetId = (target != nullptr) ? static_cast<Player*>(target)->GetPlayerId() : 0;
            }

            // 상태 주도 상태 기계: 각 case = "그 상태의 행동 + 전이 검사". (DEAD는 위에서 early-return)
            switch (m_state)
            {
            case EMonsterState::IDLE:
            {
                moveState = EMoveState::STOP;
                if (target != nullptr)
                {
                    m_state = EMonsterState::CHASE;   // 교전 시작 (이동은 다음 tick부터)
                    break;
                }
                // 대기 중 새 배회 목적지 선정 (think tick에만 - 매 tick 재선정하면 목적지가 떨려 못 감).
                if (think)
                {
                    m_patrolPoint = RandomPatrolDestination(m_patrolAnchor, GetMapId());
                }
                const Position patrolPoint = GetPatrolPoint();
                const int wx = patrolPoint.x - myPos.x;
                const int wy = patrolPoint.y - myPos.y;
                const long long wanderDist2 = DistSq(wx, wy);
                if (wanderDist2 > arrive2)
                {
                    m_state = EMonsterState::PATROL;  // 판단이 새 배회 목적지를 set함 - 출발
                }
                break;
            }

            case EMonsterState::PATROL:
            {
                if (target != nullptr)
                {
                    m_state = EMonsterState::CHASE;   // 배회 중 교전 대상 발견 (복귀 중엔 위 스캔이 skip되어 target=nullptr)
                    moveState = EMoveState::STOP;
                    break;
                }
                // 복귀(returning) 중엔 앵커가, 아니면 배회 목적지가 목적지
                const Position dest = m_returningToAnchor ? m_patrolAnchor : GetPatrolPoint();
                const int dx = dest.x - myPos.x;
                const int dy = dest.y - myPos.y;
                const long long destDist2 = DistSq(dx, dy);
                if (destDist2 <= arrive2)
                {
                    m_returningToAnchor = false;      // (복귀였다면) 집 도착 - 추격 탐지/배회 재개
                    m_state = EMonsterState::IDLE;
                    moveState = EMoveState::STOP;
                }
                else
                {
                    moveState = EMoveState::START;
                    direction = DirectionToward(dx, dy);
                }
                break;
            }

            case EMonsterState::CHASE:
            {
                if (target == nullptr)
                {
                    m_state = EMonsterState::PATROL;  // 교전 대상 소실(범위 이탈/사망) - 복귀/배회로
                    moveState = EMoveState::STOP;
                    ClearPath();
                    break;
                }
                const Position tgtPos = target->GetPos();
                const int dx = tgtPos.x - myPos.x;
                const int dy = tgtPos.y - myPos.y;
                const long long dist2 = DistSq(dx, dy);

                // 사거리 안 + 시선 확보 시에만 공격 진입 (결정7=B - 벽 너머엔 공격 안 하고 우회해 시선을 튼다).
                if (dist2 <= static_cast<long long>(monsterStats.attackRange) * monsterStats.attackRange
                    && HasLineOfSight(GetMapId(), myPos, tgtPos))
                {
                    m_state = EMonsterState::ATTACK;  // 사거리 진입 - 이번 tick은 멈춰 조준
                    moveState = EMoveState::STOP;
                    direction = DirectionToward(dx, dy);
                    ClearPath();                      // 교전 진입 - 우회 경로 폐기
                    break;
                }

                // 추격 지속? 집에서 추격 한계 거리(leash)를 넘으면 추격 포기 (도망 약올리기 방지)
                const int ax = m_patrolAnchor.x - myPos.x;
                const int ay = m_patrolAnchor.y - myPos.y;
                const long long anchorDist2 = DistSq(ax, ay);
                if (anchorDist2 > static_cast<long long>(MONSTER_LEASH_RANGE) * MONSTER_LEASH_RANGE)
                {
                    m_targetId = 0;             // 교전 포기 (점착 해제 - 다음 tick 스캔 skip)
                    m_returningToAnchor = true;       // 복귀 중 - 다음 tick 스캔 skip(재교전 차단)
                    m_state = EMonsterState::PATROL;
                    moveState = EMoveState::START;
                    direction = DirectionToward(ax, ay);   // 집 방향으로 즉시 출발
                    ClearPath();
                    break;
                }

                // 경로 재계산 판단 (think tick에만 - 매 tick LoS/재요청은 낭비): 시선이 직접 뚫리면 경로 폐기(직선 복귀),
                //   막혔는데 타깃이 경로 목적지에서 많이 벗어났으면 재요청 (기존 경로는 새 결과 올 때까지 계속 추종).
                if (think && m_waypointCount > 0)
                {
                    if (HasLineOfSight(GetMapId(), myPos, tgtPos))
                    {
                        m_waypointCount = 0;
                        m_waypointIndex = 0;
                    }
                    else
                    {
                        const Position pathGoal = m_waypoints[m_waypointCount - 1];
                        const long long drift2 = DistSq(tgtPos.x - pathGoal.x, tgtPos.y - pathGoal.y);
                        if (drift2 > static_cast<long long>(MONSTER_PATH_REPATH_DRIFT) * MONSTER_PATH_REPATH_DRIFT
                            && !m_pathPending && m_pathRetryCooldown <= 0.0f)
                        {
                            RequestPath(tgtPos);
                        }
                    }
                }

                // 조향: 우회 경로(waypoint)가 있으면 다음 코너를 향해, 없으면 타깃을 향해 직진.
                moveState = EMoveState::START;
                if (m_waypointIndex < m_waypointCount)
                {
                    const Position wp = m_waypoints[m_waypointIndex];
                    const long long wdist2 = DistSq(wp.x - myPos.x, wp.y - myPos.y);
                    if (wdist2 <= arrive2)
                    {
                        ++m_waypointIndex;   // 이 코너 도착 - 다음 코너로
                        if (m_waypointIndex >= m_waypointCount) { m_waypointCount = 0; m_waypointIndex = 0; }   // 경로 소진(pending은 안 건드림)
                    }
                    if (m_waypointIndex < m_waypointCount)
                    {
                        const Position nwp = m_waypoints[m_waypointIndex];
                        direction = DirectionToward(nwp.x - myPos.x, nwp.y - myPos.y);
                    }
                    else
                    {
                        direction = DirectionToward(dx, dy);   // 경로 소진 - 타깃 직진
                    }
                }
                else
                {
                    direction = DirectionToward(dx, dy);       // 경로 없음 - 타깃 직진
                }
                break;
            }

            case EMonsterState::ATTACK:
            {
                if (target == nullptr)
                {
                    m_state = EMonsterState::PATROL;  // 교전 대상 소실 - 복귀/배회로
                    moveState = EMoveState::STOP;
                    break;
                }
                const Position tgtPos = target->GetPos();
                const int dx = tgtPos.x - myPos.x;
                const int dy = tgtPos.y - myPos.y;
                const long long dist2 = DistSq(dx, dy);
                if (dist2 > static_cast<long long>(monsterStats.attackRange) * monsterStats.attackRange)
                {
                    m_state = EMonsterState::CHASE;   // 사거리 이탈 - 재추격 (이번 tick부터 이동)
                    moveState = EMoveState::START;
                    direction = DirectionToward(dx, dy);
                    break;
                }
                // 시선이 막혔으면(타깃이 벽 뒤로 숨음) 재추격 - 단 think tick에만 판정해 순간 끊김에 안 떨린다(D-M9 비대칭 유예).
                if (think && !HasLineOfSight(GetMapId(), myPos, tgtPos))
                {
                    m_state = EMonsterState::CHASE;
                    moveState = EMoveState::START;
                    direction = DirectionToward(dx, dy);
                    break;
                }
                // 사거리 안 + 시선 확보: 정지/조준 유지 + 쿨다운 도달 시 공격
                moveState = EMoveState::STOP;
                direction = DirectionToward(dx, dy);
                ApplyAttackTick(target, monsterStats, deltaTime);   // 쿨다운 누산 + 도달 시 교전 대상에 1회 타격 (무적 가드, 사망 전이)
                break;
            }
            }

            // 이동 시도 - 벽/맵 끝에 막히면 "이동 의도(로컬 moveState)"를 정지로 되쓴다.
            //   되쓰기가 SetMoveState/변화 게이트보다 먼저라, 막힌 첫 tick 만 START->STOP 변화로 1회 broadcast 되고
            //   막혀 있는 동안은 STOP==STOP 이라 침묵한다 - 감지 지점에서 곧장 상태를 전이시키면 벽에 몰린
            //   몬스터마다 매 tick(30Hz) 같은 내용이 재전송되는 스팸이 된다.
            //   (클라 외삽은 STOP 을 받는 순간 멈추므로 벽 너머로 미끄러지는 유령 활주도 함께 사라진다.)
            if (moveState == EMoveState::START)
            {
                const Position moved = AdvanceMove(GetMapId(), myPos, direction, monsterStats.moveSpeed, deltaTime);
                if (moved.x == myPos.x && moved.y == myPos.y)
                {
                    moveState = EMoveState::STOP;   // 막힘 - 이동 의도 취소 (아래 게이트가 전이 1회만 알림)

                    if (m_state == EMonsterState::PATROL)
                    {
                        if (m_returningToAnchor)
                        {
                            // 복귀 길이 막힘 - 여기를 새 집으로 삼는다. 복귀 플래그는 집 도착에서만 풀리는데
                            //   길이 막히면 영원히 안 풀려 교전 탐지까지 잠긴 무저항 몬스터로 고착되기 때문.
                            m_returningToAnchor = false;
                            SetPatrolAnchor(myPos);
                        }
                        else
                        {
                            // 배회 목적지가 벽 너머 - 목적지를 제자리로 리셋하고 대기로. 리셋 없이 대기만 가면
                            //   다음 tick "목적지가 멀다" 판정이 같은 막힌 목적지로 즉시 재출발해 출발/정지를 반복한다.
                            m_patrolPoint = myPos;
                        }
                        m_state = EMonsterState::IDLE;
                    }
                    else if (m_state == EMonsterState::CHASE && target != nullptr)
                    {
                        // 추격 중 벽에 막힘 - 우회 경로 요청 (막힘 = 유일 트리거·결정6=A). 오래 우회 실패면 추격 포기(집으로·결정5=B).
                        m_pathFailElapsed += deltaTime;
                        if (m_pathFailElapsed >= MONSTER_PATH_GIVEUP_SEC)
                        {
                            m_targetId = 0;
                            m_returningToAnchor = true;
                            m_state = EMonsterState::PATROL;
                            ClearPath();
                        }
                        else if (!m_pathPending && m_pathRetryCooldown <= 0.0f)
                        {
                            RequestPath(target->GetPos());   // pending 이거나 쿨다운 중이면 대기 (자기치유 재시도)
                        }
                    }
                    // (ATTACK 중 막힘은 상태 유지 - 조준만 하므로 이동 없음)
                }
                else
                {
                    SetPos(moved);          // 권위 위치 갱신 (broadcast 여부는 아래 게이트가 결정)
                    m_pathFailElapsed = 0.0f;   // 전진 성공 - 우회 실패 누산 리셋
                }
            }

            // base 이동 상태/방향 반영 (base 상속 접근자)
            SetMoveState(moveState, direction);

            // broadcast 게이트 - 변화가 있을 때만 보냄 (전 몬스터 단일 방식).
            //   이동 상태/방향이 바뀔 때만 바뀜 표시 -> ProcessPacket이 그 순간만 SerializeBroadcast -> 시야(AOI) 송신.
            //   클라는 마지막 broadcast 이후 같은 방향 표(LUT)로 위치 예측하므로 직선 이동 중엔 패킷 0(대역폭 절감).
            if (moveState != prevMoveState || direction != prevDirection)
            {
                SetDirty(true);
            }
        }

        // ATTACK 사거리 안: 공격 쿨다운을 누산하고 도달 시 교전 대상에 1회 타격한다.
        //   무적 player에 대한 헛스윙은 쿨다운을 소거하지 않는다 - 무적이 풀리면 다음 tick 바로 타격.
        void Monster::ApplyAttackTick(GameObject* target, const MonsterTemplate& monsterStats, float deltaTime)
        {
            m_attackCooldown += deltaTime;
            if (m_attackCooldown >= monsterStats.attackCooldown)
            {
                // 무적 가드를 쿨다운 리셋 앞에 둔다 - 무적 player에 대한 헛스윙이 쿨다운을
                //   소거하지 않게 (무적이 풀리면 다음 tick에 바로 정상 타격이 나간다).
                if (target->GetObjectType() == EObjectType::PLAYER
                    && static_cast<Player*>(target)->IsInvulnerable())
                {
                    // 리셋 안 함 - 누산 유지
                }
                else
                {
                    m_attackCooldown = 0.0f;
                    int  dmg = CombatFormula::Compute(this, target);
                    bool wasAlive = !target->IsDead();                   // 전이 캡처 (적용 직전)
                    target->TakeDamage(dmg);                            // hp 적용 (잔여 hp 초과분은 내부에서 잘림)
                    m_lastDamage = dmg;                                 // 표시용 = 원시 판정치 (잘린 값을 보내면 막타가 "잔여 hp"로 보임)
                    m_attackVictim = target;                            // same-tick: Map::Update가 SC_DAMAGE 송신 대상으로 소비
                    m_didAttack = true;                                 // 대기 - Map::Update가 SC_DAMAGE 준비
                    if (wasAlive && target->IsDead())                    // alive -> dead 전이 1회
                    {
                        target->SetMoveState(EMoveState::STOP, target->GetDirection());   // 시신 이동 정지 - 시체 스폰 통지가 사망 직전의 START 를 실어 보내지 않게 (몬스터 MarkDead 와 대칭)
                        static_cast<Player*>(target)->StartRespawn();    // 리스폰 타이머 시작
                        m_victimDied = true;                             // 대기 - Map::Update가 SC_DEATH 준비
                    }
                }
            }
        }

        bool Monster::SerializeBroadcast(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
        {
            const Position pos = GetPos();
            // 이동 상태/방향은 GameObject base 멤버 - base 접근자로 읽는다(const 메서드라 const 접근자 OK).
            SC_MONSTER_MOVE body{ static_cast<UINT32>(GetId()), GetMoveState(), GetDirection(), pos.x, pos.y };
            pkt.Begin(static_cast<USHORT>(PacketType::SC_MONSTER_MOVE));   // 헤더 예약 + type 태깅
            body.Serialize(pkt);
            return pkt.End();                                                     // 전체 길이 채워 넣음
        }

        bool Monster::SerializeSpawn(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
        {
            const Position pos = GetPos();
            SC_MONSTER_SPAWN body;
            body.monsterId = static_cast<UINT32>(GetId());
            body.moveState = GetMoveState();   // GameObject base 멤버 - base 접근자
            body.direction = GetDirection();
            body.x = pos.x;
            body.y = pos.y;
            body.hp = GetHp();        // GameObject base (Combat 클러스터가 추가)
            body.maxHp = GetMaxHp();
            body.objectType = static_cast<BYTE>(GetObjectType());
            body.monsterType = static_cast<BYTE>(GetMonsterType());   // 종류 (클라 N종류 스프라이트 렌더)
            body.attackRange = MonsterTemplateFor(GetMonsterType()).attackRange;   // 공격 사거리 (클라 공격범위 표시)
            body.moveSpeed = MonsterTemplateFor(GetMonsterType()).moveSpeed;        // 이동 속도 (클라가 이 값으로 위치 예측 - 종류별 속도 지원)
            body.isBoss = MonsterIsBoss(GetMonsterType()) ? 1 : 0;                  // 보스 여부 (클라 렌더 강조 - 하드코딩 종류 판정 대체)
            // (body.syncMode 없음 - 단일 방식)
            pkt.Begin(static_cast<USHORT>(PacketType::SC_MONSTER_SPAWN));
            body.Serialize(pkt);
            return pkt.End();
        }

        bool Monster::ShouldRethink(float deltaTime)
        {
            m_thinkTimer += deltaTime;
            if (m_thinkTimer >= MONSTER_THINK_INTERVAL)   // 임계 도달 시 1회 판단 허용
            {
                m_thinkTimer = 0.0f;
                return true;
            }
            return false;
        }

        bool Monster::ConsumeAttack(int& outDamage, GameObject*& outVictim)
        {
            if (!m_didAttack) { return false; }
            outDamage = m_lastDamage;
            outVictim = m_attackVictim;
            m_didAttack = false;       // 소비 -> 한 tick 한 번만 준비
            m_attackVictim = nullptr;  // 위생 - 소비 후엔 다음 공격 전까지 대상 안 들고 있음(stale read 차단)
            return true;
        }

        bool Monster::ConsumeVictimDeath()
        {
            if (!m_victimDied) { return false; }
            m_victimDied = false;   // 소비 -> SC_DEATH(player) 1회 준비
            return true;
        }

        void Monster::MarkDead()
        {
            m_state = EMonsterState::DEAD;   // DEAD enum (값 4)
            m_deathTimer = 0.0f;             // 시신 유지 타이머 시작
            SetMoveState(EMoveState::STOP, GetDirection());   // 시신 이동 정지 - 시체 스폰 통지가 사망 직전의 START 를 실어 보내 관찰자 화면에서 시체가 활주하지 않게
        }

        bool Monster::IsDeadExpired() const
        {
            return (m_state == EMonsterState::DEAD) && (m_deathTimer >= MONSTER_DEATH_GRACE_SEC);
        }

        void            Monster::SetMap(Map* m) { m_map = m; }
        EMonsterState   Monster::GetState() const { return m_state; }
        void            Monster::SetState(EMonsterState s) { m_state = s; }
        EMonsterType    Monster::GetMonsterType() const { return m_monsterType; }
        void            Monster::SetMonsterType(EMonsterType t) { m_monsterType = t; }
        int             Monster::GetSpawnGroupIdx() const { return m_spawnGroupIdx; }
        void            Monster::SetSpawnGroupIdx(int idx) { m_spawnGroupIdx = idx; }
        void            Monster::SetChannelId(int channelId) { m_channelId = channelId; }

        // 경로/pending 초기화 - CHASE 이탈/추격 포기/사망/직선 복귀 시 (다음 요청이 깨끗한 상태에서 시작).
        //   실패 누산/재시도 쿨다운도 함께 리셋 - 안 그러면 giveup(누산 만료) 직후 재교전 시 옛 누산이 남아
        //   첫 벽 tick 에 경로 요청 없이 즉시 재-giveup 하는 잠복 경로가 생긴다(깨끗한 시작 보장).
        void Monster::ClearPath()
        {
            m_waypointCount = 0;
            m_waypointIndex = 0;
            m_pathPending = false;
            m_pathPendingElapsed = 0.0f;
            m_pathFailElapsed = 0.0f;
            m_pathRetryCooldown = 0.0f;
        }

        // 우회 경로 요청 발행 - pathfinder 스레드로 값 스냅샷을 보낸다. 큐 포화(발행 실패)면 pending 을 세우지 않는다(D-H1).
        void Monster::RequestPath(const Position& targetPos)
        {
            PathfindRequest req;
            req.channelId = m_channelId;
            req.monsterId = static_cast<UINT32>(GetId());
            req.mapId     = GetMapId();
            req.from      = GetPos();
            req.to        = targetPos;
            req.targetId  = m_targetId;
            req.seq       = m_pathSeq + 1;   // 다음 요청 번호 (발행 성공 시에만 확정)

            if (PathfinderThread::GetInstance().Enqueue(req))
            {
                m_pathSeq            = req.seq;   // 발행 성공 - 이 seq 의 결과만 유효
                m_pathPending        = true;
                m_pathPendingElapsed = 0.0f;
            }
            // 발행 실패(false) = 큐 포화 -> pending 미설정 -> 다음 tick 재시도 (자기치유)
        }

        // 길찾기 결과 도착 (채널 스레드) - seq/대상/AI 상태를 재검증하고 waypoint 를 적용하거나 폐기한다.
        //   재조회(FindMonsterForPath)로 존재/!IsDead/mapId 는 이미 확인됨 - 여기선 seq/targetId/CHASE 를 본다.
        void Monster::OnPathResult(UINT32 seq, UINT32 targetId, bool success, const Position* waypoints, int count)
        {
            // 이 결과가 지금 대기 중인 요청의 것이 아니면(늦게 온 옛 결과) 아무것도 안 건드리고 무시.
            if (!m_pathPending || seq != m_pathSeq) { return; }

            m_pathPending = false;
            m_pathPendingElapsed = 0.0f;

            // 대상이 바뀌었거나(다른 player/소실) 더는 추격 상태가 아니면 이 경로는 쓸모없음 - 폐기.
            if (targetId != m_targetId || m_targetId == 0 || m_state != EMonsterState::CHASE)
            {
                m_waypointCount = 0;
                m_waypointIndex = 0;
                return;
            }

            if (!success || count <= 0)
            {
                // NOPATH(도달불가/예산초과) - 재요청 쿨다운 + 연속 실패 누산은 Update 가 GIVEUP 판정에 쓴다.
                m_waypointCount = 0;
                m_waypointIndex = 0;
                m_pathRetryCooldown = MONSTER_PATH_RETRY_SEC;
                return;
            }

            int n = count;
            if (n > PATH_WAYPOINT_MAX) { n = PATH_WAYPOINT_MAX; }
            for (int i = 0; i < n; ++i) { m_waypoints[i] = waypoints[i]; }
            m_waypointCount = n;
            m_waypointIndex = 0;
            m_pathFailElapsed = 0.0f;   // 경로 확보 = 연속 실패 리셋
        }
        // SetMoveState/GetMoveState/GetDirection은 GameObject base에 정의 - Monster 재정의 안 함.
        void            Monster::SetPatrolAnchor(const Position& a)
        {
            m_patrolAnchor = a;
            m_patrolPoint = a;   // 스폰 직후엔 집(anchor)을 배회 목적지로 - 첫 판단이 새 목적지 선택
        }
        const Position& Monster::GetPatrolPoint() const { return m_patrolPoint; }

        // 집(anchor) 반경 MONSTER_PATROL_RADIUS 안 랜덤 배회 목적지 - 맵 경계로 클램프하고 벽 칸이면 다시 뽑는다.
        //   전부 실패하면 집(anchor)을 준다 - 집은 스폰 검증이 통행을 보장하는 칸이고, 가는 길이 막히면
        //   이동 차단 처리가 대기로 되돌리므로 자기수렴. 스레드 안전 난수(채널별 xorshift32) 사용.
        static Position RandomPatrolDestination(const Position& anchor, int mapId)
        {
            const int w = MapWidthFor(mapId);
            const int h = MapHeightFor(mapId);
            for (int attempt = 0; attempt < WALK_SPOT_RETRY_MAX; ++attempt)
            {
                const int dx = CombatFormula::RandomRange(-MONSTER_PATROL_RADIUS, MONSTER_PATROL_RADIUS);
                const int dy = CombatFormula::RandomRange(-MONSTER_PATROL_RADIUS, MONSTER_PATROL_RADIUS);
                Position dest = { anchor.x + dx, anchor.y + dy };
                dest.x = (dest.x < 0) ? 0 : ((dest.x > w) ? w : dest.x);
                dest.y = (dest.y < 0) ? 0 : ((dest.y > h) ? h : dest.y);
                if (IsWalkable(mapId, dest.x, dest.y)) { return dest; }
            }
            return anchor;
        }
    }
}
