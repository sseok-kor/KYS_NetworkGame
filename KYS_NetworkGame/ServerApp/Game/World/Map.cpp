#include "pch_serverapp.h"
#include "Map.h"
#include "../GameObject.h"     // GetPos()/GetCellRow()/SetCell() - 완전 정의 필요(.cpp에서 include)
#include "../Player/Player.h"
#include "../Monster/Monster.h"
#include "../GameCommon/Protocol/GamePackets.h"
#include "../GameCommon/Protocol/PacketType.h"
#include <algorithm>           // std::find / std::swap (swap-and-pop erase 용)
#include "../GameServer/Core/Log/Logger.h"   // 고정 버퍼 캡 초과 거부를 첫 1회 기록 (그 뒤는 카운터)

namespace KYS
{
    namespace SERVERAPP
    {

        // 고정 버퍼 캡 초과로 적재를 거부한 횟수 (프로세스 누적) 와 첫 로그 1회성 게이트.
        //   채널 스레드 6개가 각자 자기 Map 을 돌므로 원자 연산으로 센다.
        static volatile LONG s_outboundOversizeCount  = 0;
        static volatile LONG s_outboundOversizeLogged = 0;


        // vector에서 객체 하나를 찾아 제거하는 헬퍼 - 마지막 원소와 바꿔치고 뒤를 잘라냄 (순서 비보존, 드물게 일어나는 제거에 적합).
        static void SwapAndPop(std::vector<GameObject*>& vec, GameObject* obj)
        {
            std::vector<GameObject*>::iterator it = std::find(vec.begin(), vec.end(), obj);
            if (it != vec.end())
            {
                std::swap(*it, vec.back());
                vec.pop_back();
            }
        }

        // rangePx(px)를 덮는 데 필요한 셀 반경(칸 수)을 구한다 - 정수 올림(ceil).
        //   예: range 400 / cell 250 -> ceil(1.6)=2 (즉 5x5 칸 수집). float 안 씀(C++14 traditional).
        static int CellRangeFor(int rangePx)
        {
            return (rangePx + CELL_SIZE - 1) / CELL_SIZE;
        }

        // 가로/세로 거리 차이가 둘 다 rangePx 이하인지 본다 (정사각형 범위 안 판정 - 거리 제곱/sqrt 없이 정수 비교).
        static bool IsInRange(int dx, int dy, int rangePx)
        {
            if (dx < 0) { dx = -dx; }
            if (dy < 0) { dy = -dy; }
            return dx <= rangePx && dy <= rangePx;
        }

        // 한 tick 처리: 각 객체 갱신(위치 예측) -> 셀 재배치 + 가시성 -> 몬스터 전투/부활 이벤트 적재. 송신은 안 함.
        void Map::Update(float deltaTime)
        {
            m_diedPlayers.clear();   // tick-local - 이번 tick 사망 플레이어만 담는다 (직전 tick 것은 Channel이 이미 소비)

            // 각 객체 다형 Update 호출(이동 시 위치 예측으로 SetPos + 바뀜 표시) 후 바뀐 객체를 목록에 담기 시작.
            //   바뀜 목록(m_movedObjects)은 직전 SendObjectUpdates이 비워둠 -> 이 시점 비어 있음.
            //   전체를 정확히 1회씩 순회하므로 IsDirty()면 그냥 push_back (중복 없음).
            //   OnMove(ProcessJob)는 바뀜 표시 flag만 세움(목록 미적재) -> 여기서 flag 기준으로 목록에 담음.
            for (std::vector<GameObject*>::iterator it = m_objects.begin(); it != m_objects.end(); ++it)
            {
                GameObject* obj = *it;
                obj->Update(deltaTime);                            // 다형 Update - 이동 시 위치 예측으로 SetPos/SetDirty

                if (obj->IsDirty() && !obj->IsDead())
                {
                    m_movedObjects.push_back(obj);
                }

                if (obj->GetObjectType() == EObjectType::MONSTER)
                {
                    Monster* mon = static_cast<Monster*>(obj);
                    int dmg = 0;
                    GameObject* victim = nullptr;
                    if (mon->ConsumeAttack(dmg, victim))                  // 이번 tick 공격 대상(same-tick transient)을 함께 받음 - 비타겟팅이라 저장된 타겟이 없음
                    {
                        if (victim != nullptr)
                        {
                            StageDamage(mon, victim, dmg);                // 피해를 주변 관찰자에게 송신 대기 적재
                            if (mon->ConsumeVictimDeath())
                            {
                                StageDeath(victim);
                                if (victim->GetObjectType() == EObjectType::PLAYER)
                                {
                                    m_diedPlayers.push_back(victim->GetId());   // 사망 플레이어 sid - Channel이 tick 후 거래 강제 종결(사망자는 거래 불가)
                                }
                            }
                        }
                    }
                }
            }

            // 셀 재배치 + 가시성 재평가 시작 (셀이 바뀐 객체만 SC_SPAWN/DESPAWN).
            RelocateInGrid();                                      // 전체 순회하며 셀 바뀜 판정 + 가시성 갱신

            // 부활 처리 시작 - 부활 요청한 player의 SC_RESPAWN 적재.
            for (std::vector<GameObject*>::iterator it = m_objects.begin(); it != m_objects.end(); ++it)
            {
                if ((*it)->GetObjectType() != EObjectType::PLAYER) { continue; }
                Player* p = static_cast<Player*>(*it);
                if (p->ConsumeRespawn())
                {
                    StageRespawn(p);
                }
            }

            // 바뀜 목록은 여기서 비우지 않음 - Channel::SendObjectUpdates이 broadcast 송신 후 MapManager::ClearMovedObjects로 비움.
            //   (순서: SendObjectUpdates(직전 tick 바뀜 소비 후 비움) -> UpdateFrame(이번 tick 바뀜 생성). 여기서 비우면 broadcast 대상이 사라짐)
        }

        // 객체를 맵 전체 목록과 해당 셀(격자)에 등록한다.
        void Map::Insert(GameObject* obj)
        {
            m_objects.push_back(obj);                    // 맵 전체 목록에 추가 (평균 O(1))
            const Position& p = obj->GetPos();           // 현재 위치
            const int r = RowOf(p.y);
            const int c = ColOf(p.x);
            m_grid[r][c].Insert(obj);                    // 해당 셀에 연결 (Cell::Insert)
            obj->SetCell(r, c);                          // 셀 캐시 갱신 (셀 바뀜 판정 기준)
        }

        // 객체를 맵 전체 목록과 셀에서 제거한다.
        void Map::Erase(GameObject* obj)
        {
            SwapAndPop(m_objects, obj);                  // 맵 전체 목록에서 제거 (순서 비보존)
            const int r = obj->GetCellRow();             // 캐시된 셀 (위치 역산 없이)
            const int c = obj->GetCellCol();
            if (r >= 0 && r < GRID_ROWS && c >= 0 && c < GRID_COLS)   // 미배치(-1 등) 방어
            {
                m_grid[r][c].Erase(obj);                 // 해당 셀에서 연결 해제 (Cell::Erase)
            }
            SwapAndPop(m_movedObjects, obj);
        }


        // origin 위치에서 rangePx(px) 안의 객체를 out에 모은다.
        //   먼저 범위를 덮는 셀들을 광역 수집한 뒤(GetObjectsAt), origin 좌표 기준 정사각 범위로 정확히 자른다.
        //   셀은 거칠어(범위보다 큼) 경계 셀에 범위 밖 객체가 섞이므로 좌표로 한 번 더 컷해야 진짜 시야가 된다.
        //   origin : 수집 중심 / rangePx : 수집 반경(시야=VIEW_RANGE, 어그로=몬스터 범위) / out : 결과(비우기는 호출자 몫)
        void Map::GetNearbyObjects(const GameObject* origin, int rangePx, std::vector<GameObject*>& out) const
        {
            const Position& pos = origin->GetPos();          // 현재 위치
            const size_t before = out.size();                // 이 호출이 추가한 부분만 trim (out 앞부분 보존)
            GetObjectsAt(RowOf(pos.y), ColOf(pos.x), CellRangeFor(rangePx), out);   // 범위를 덮는 셀 광역 수집(append)

            // 광역 수집분에서 정사각 범위 밖 객체를 제거 (swap-and-pop, 순서 무관)
            for (size_t i = before; i < out.size(); )
            {
                const Position& op = out[i]->GetPos();
                if (IsInRange(op.x - pos.x, op.y - pos.y, rangePx))
                {
                    ++i;                                     // 범위 안 - 유지
                }
                else
                {
                    out[i] = out.back();                     // 범위 밖 - 마지막 원소로 덮고 잘라냄 (재검사 위해 ++i 안 함)
                    out.pop_back();
                }
            }
        }

        // 중앙 셀(centerRow,centerCol)에서 cellRange 칸 반경의 셀 객체를 out에 모은다.
        //   좌표를 받지 않아 정사각 범위로 자르지 않는다(옛 위치처럼 셀 인덱스만 아는 호출에 사용 - 광역 수집).
        //   범위로 정확히 자르려면 좌표를 아는 GetNearbyObjects를 쓴다.
        void Map::GetObjectsAt(int centerRow, int centerCol, int cellRange, std::vector<GameObject*>& out) const
        {
            // cellRange 칸 반경의 (2*cellRange+1)^2 셀 순회 - 중앙 + 인접 칸들.
            for (int dr = -cellRange; dr <= cellRange; ++dr)
            {
                for (int dc = -cellRange; dc <= cellRange; ++dc)
                {
                    const int r = centerRow + dr;
                    const int c = centerCol + dc;
                    // 격자 밖 인덱스는 건너뜀 (범위 제한 아님! 제한하면 같은 셀이 중복돼 객체가 중복됨).
                    if (r < 0 || r >= GRID_ROWS || c < 0 || c >= GRID_COLS)
                        continue;

                    const std::vector<GameObject*>& objs = m_grid[r][c].GetObjects();   // 셀의 객체 목록 (연속 메모리)
                    for (std::vector<GameObject*>::const_iterator it = objs.begin(); it != objs.end(); ++it)
                    {
                        out.push_back(*it);              // 객체 추가 - self 포함(origin이 중앙 셀에 있음). 비우기는 호출자 몫.
                    }
                }
            }
        }

        // 몬스터 교전 대상 산출(비타겟팅 + 서버 권위): 신규 획득은 aggroRange 안 최근접 살아있는 player,
        //   점착(stickyId·직전 대상)은 maintainRange 안까지 유지(우회 추격 중 거리 확장을 견딤 - flicker 차단 + 놓침 방지).
        //   반환 포인터는 현재 grid의 live 객체 - 호출자(Monster::Update)가 같은 tick에만 쓴다. 몬스터는 raw Player*를
        //   저장하지 않고 매 tick 이 스캔으로 즉석 산출한다 - 저장 포인터 0이라 cross-tick dangling(옛 cross-map UAF) 자체가 불가능.
        GameObject* Map::ResolveTarget(const GameObject* origin, int aggroRange, int maintainRange, UINT32 stickyId)
        {
            // 수집은 둘 중 큰 반경으로 - 점착 대상이 aggro 밖 maintain 안에 있을 수 있다(우회로 거리가 벌어진 경우).
            const int scanRange = (maintainRange > aggroRange) ? maintainRange : aggroRange;
            m_targetCandidates.clear();
            GetNearbyObjects(origin, scanRange, m_targetCandidates);   // 범위 안 객체 수집(self 포함, 이동 broadcast와 동일 함수 재사용)

            const Position center = origin->GetPos();
            const long long aggro2    = static_cast<long long>(aggroRange) * aggroRange;
            const long long maintain2 = static_cast<long long>(maintainRange) * maintainRange;

            GameObject* nearest = nullptr;
            long long bestDist2 = aggro2;   // 신규 후보 = 어그로 범위 안만 (초기값 = 어그로 범위 제곱)
            for (size_t k = 0; k < m_targetCandidates.size(); ++k)
            {
                GameObject* cand = m_targetCandidates[k];
                if (cand->GetObjectType() != EObjectType::PLAYER) { continue; }   // 교전 대상 = player만 (몬스터끼리는 안 싸움)
                if (cand->IsDead()) { continue; }                                 // 죽은 player 제외 (시신 공격/타겟 진동 방지)

                const Position cp = cand->GetPos();
                const long long dx = cp.x - center.x;
                const long long dy = cp.y - center.y;
                const long long dist2 = dx * dx + dy * dy;

                // 점착: 직전 대상이면 유지 반경(maintain) 안에서 유지 - 최근접이 아니어도, aggro 밖이어도 놓치지 않는다.
                if (stickyId != 0 && static_cast<Player*>(cand)->GetPlayerId() == stickyId)
                {
                    if (dist2 <= maintain2) { return cand; }
                    continue;   // 유지 반경마저 벗어남 - 점착 해제 (신규 후보로도 안 잡히게 여기서 건너뜀)
                }
                // 신규 획득: 어그로 범위 안 최근접만
                if (dist2 > aggro2) { continue; }
                if (dist2 <= bestDist2)
                {
                    bestDist2 = dist2;
                    nearest = cand;
                }
            }
            return nearest;   // 점착 대상 못 찾았으면 최근접(어그로 범위 안 player 없으면 nullptr)
        }

        // 새로 진입한 객체와 시야 범위 안 객체 사이에 양방향 spawn 통지를 보낸다.
        void Map::SpawnVisibilityFor(GameObject* enter)
        {
            const bool enterIsPlayer = (enter->GetObjectType() == EObjectType::PLAYER);

            std::vector<GameObject*> nearbyObjects;
            GetNearbyObjects(enter, VIEW_RANGE, nearbyObjects);   // 진입 지점 시야 범위 (self 포함)
            for (std::vector<GameObject*>::iterator it = nearbyObjects.begin(); it != nearbyObjects.end(); ++it)
            {
                GameObject* o = *it;
                if (o == enter) { continue; }
                const bool oIsPlayer = (o->GetObjectType() == EObjectType::PLAYER);

                // (a) enter가 o를 본다 - enter가 player일 때만 (몬스터는 client/시야집합 없음)
                if (enterIsPlayer)
                {
                    if (NotifySpawn(enter, o))                      // 다형 SerializeSpawn(o) - 실제 적재 시에만 시야 등록
                    {
                        static_cast<Player*>(enter)->AddVisible(o);
                    }
                }
                // (b) 대칭: o가 enter를 본다 - o가 player일 때만
                if (oIsPlayer)
                {
                    if (NotifySpawn(o, enter))                      // 다형 SerializeSpawn(enter)
                    {
                        static_cast<Player*>(o)->AddVisible(enter);
                    }
                }
            }
        }

        // 떠나는 객체를 보던 관찰자들에게 despawn을 통지한다 (Channel::OnClientLeave가 위임 또는 직접 호출).
        void Map::DespawnVisibilityFor(GameObject* leaver)
        {
            // leaver를 보던 모든 player 관찰자에게 despawn 통지 + 그들 시야집합에서 leaver 제거.
            //   관찰자 후보 = leaver 옛 셀 블록(superset) 전수 - IsVisible 가드로 실제 보던 자만 거른다.
            //   과거엔 player leaver는 "자기 시야집합"만 순회(대칭 가정)했으나, death-grace 중 죽은
            //   관찰자가 산 leaver를 단방향으로만 담는 비대칭이 생기면 그 관찰자를 놓쳐 dangling이 남았다.
            //   그래서 player/monster 모두 셀 블록 역스캔으로 통일 - 비대칭 관찰자까지 빠짐없이 제거한다.
            //   trim된 시야(400)로 좁히면 lazy 갱신 탓에 leaver를 아직 보던 margin 관찰자(블록 안, 400 밖)를 놓친다.
            std::vector<GameObject*> nearbyObjects;
            GetObjectsAt(leaver->GetCellRow(), leaver->GetCellCol(), CellRangeFor(VIEW_RANGE), nearbyObjects);
            for (std::vector<GameObject*>::iterator it = nearbyObjects.begin(); it != nearbyObjects.end(); ++it)
            {
                GameObject* o = *it;
                if (o == leaver) { continue; }
                if (o->GetObjectType() != EObjectType::PLAYER) { continue; }   // 몬스터는 관찰 안 함(시야집합 없음)
                Player* op = static_cast<Player*>(o);
                if (op->IsVisible(leaver)) { NotifyDespawn(o, leaver); op->RemoveVisible(leaver); }
            }

            // player leaver는 자기 시야집합도 비운다(풀 반납 전 위생).
            if (leaver->GetObjectType() == EObjectType::PLAYER)
            {
                static_cast<Player*>(leaver)->ClearVisible();
            }
        }

        // to 에게 who 의 despawn 한 건을 통지한다 (맵 이동 시 옛 객체들 정리에 사용 - 본체 NotifyDespawn 재사용).
        void Map::NotifyDespawnTo(GameObject* to, GameObject* who)
        {
            NotifyDespawn(to, who);   // 직렬화 + 송신 대기 push (본체 재사용)
        }

        // 맵 전체 객체 목록을 돌려준다 (전수 순회용 - 연속 메모리).
        const std::vector<GameObject*>& Map::GetObjects() const
        {
            return m_objects;
        }

        // 이번 tick 위치가 바뀐 객체 목록을 돌려준다 (broadcast 대상).
        const std::vector<GameObject*>& Map::GetMovedObjects() const
        {
            return m_movedObjects;
        }

        // 바뀐 객체 목록을 비운다.
        void Map::ClearMovedObjects()
        {
            m_movedObjects.clear();
        }

        const std::vector<UINT64>& Map::GetDiedPlayers() const
        {
            return m_diedPlayers;
        }

        // 송신 대기 패킷 목록을 돌려준다.
        const std::vector<OutboundPacket>& Map::GetOutboundPackets() const
        {
            return m_outboundPackets;
        }

        // 송신 대기 패킷을 비운다 (송신 후).
        void Map::ClearOutboundPackets()
        {
            m_outboundPackets.clear();
        }

        // 전체 객체를 순회하며 셀이 바뀐 객체만 격자에서 옮긴다 (셀 캐시로 바뀜 판정).
        void Map::RelocateInGrid()
        {
            // 전체 목록 순회 + 객체 셀 캐시로 셀 바뀜 판정. 바뀜 목록(m_movedObjects)은 안 씀 -
            //   바뀜 목록은 broadcast 수집용이고, 여기서는 "셀이 바뀌었나"를 객체 셀 캐시(GetCellRow/Col)로 직접 판정.
            //   이동 속도(200)와 셀 크기(250)가 비슷해 잔 격자라 셀을 자주 넘나, 나눗셈 2회 + int 비교라 순회 비용은 저렴.
            //   순회 중 m_objects 자체는 안 바꾸고 셀 격자만 Erase/Insert -> iterator 유효.
            for (std::vector<GameObject*>::iterator it = m_objects.begin(); it != m_objects.end(); ++it)
            {
                GameObject* obj = *it;
                const Position& cur = obj->GetPos();                  // 현재 위치
                const int newRow = RowOf(cur.y);                      // 좌표 [0,4000] -> 인덱스 [0,16) (ColOf/RowOf가 끝 좌표 클램프)
                const int newCol = ColOf(cur.x);

                if (newRow == obj->GetCellRow() && newCol == obj->GetCellCol())
                {
                    continue;                                         // 셀 안 바뀜 - 변화 없음 (대부분 tick)
                }

                // 셀이 바뀐 순간만: 가시성 재평가(옛 시야는 이전 셀로) -> 격자 재배치 -> 셀 캐시 갱신
                UpdateVisibility(obj);                                // 옛 시야를 obj->GetCellRow()로 계산(재배치 전)

                m_grid[obj->GetCellRow()][obj->GetCellCol()].Erase(obj);   // 옛 셀에서 연결 해제 (캐시 셀)
                m_grid[newRow][newCol].Insert(obj);                  // 새 셀에 연결
                obj->SetCell(newRow, newCol);                        // 셀 캐시 갱신 (바뀜 판정 기준)
            }
        }

        // 셀이 바뀐 객체의 새 시야 범위를 보고 새로 보이는/안 보이는 객체에 spawn/despawn을 양방향 통지한다.
        //   newNearby=새 시야(좌표 trim된 정확한 집합), oldNearby=옛 셀 블록(옛 좌표가 없어 trim 못 한 superset).
        //   spawn: newNearby 전수를 IsVisible(실제 통지 여부) 가드로 판정 - 이미 본 건 skip, 신규만 spawn.
        //     (oldNearby 멤버십으로 '이미 봄'을 판정하면 안 된다: superset이라 시야 밖 halo 객체를 오판해 spawn 누락.)
        //   despawn: oldNearby(superset 후보)에서 IsVisible 가드로 거름 - 안 보던 건 가드가 막아 over-fire 0, 옛시야가 oldNearby에 포함이라 under-fire 0.
        void Map::UpdateVisibility(GameObject* mover)
        {
            const bool moverIsPlayer = (mover->GetObjectType() == EObjectType::PLAYER);

            std::vector<GameObject*> oldNearby;
            GetObjectsAt(mover->GetCellRow(), mover->GetCellCol(), CellRangeFor(VIEW_RANGE), oldNearby);   // 이전 셀 블록 (재배치 전, 좌표 없어 trim 못 함)
            std::vector<GameObject*> newNearby;
            GetNearbyObjects(mover, VIEW_RANGE, newNearby);                   // 현재 위치 시야 범위 (self 포함, trim됨)

            SpawnNewlyVisible(mover, moverIsPlayer, newNearby);                       // 새 시야 진입 객체와 양방향 spawn
            DespawnNoLongerVisible(mover, moverIsPlayer, oldNearby, newNearby);       // 시야 이탈 객체와 양방향 despawn (spawn 뒤 순서 유지)
        }

        // 새 시야(newNearby)에 들어온 객체들과 mover 사이에 양방향 spawn을 통지한다 (이미 본 객체는 IsVisible 가드가 skip).
        void Map::SpawnNewlyVisible(GameObject* mover, bool moverIsPlayer, const std::vector<GameObject*>& newNearby)
        {
            // 새로 보이게 됨 = 새 시야 안에서 아직 안 본(IsVisible=false) 객체 - 양방향 spawn (가드가 신규만 통과).
            for (std::vector<GameObject*>::const_iterator it = newNearby.begin(); it != newNearby.end(); ++it)
            {
                GameObject* o = *it;
                if (o == mover) { continue; }
                const bool oIsPlayer = (o->GetObjectType() == EObjectType::PLAYER);

                // (a) mover가 o를 본다 - mover가 player일 때만 (몬스터는 시야집합/client 없음)
                if (moverIsPlayer)
                {
                    Player* mp = static_cast<Player*>(mover);
                    if (!mp->IsVisible(o) && NotifySpawn(mover, o)) { mp->AddVisible(o); }   // 적재 성공 시에만 시야 등록
                }
                // (b) 대칭: o가 mover를 본다 - o가 player일 때만
                if (oIsPlayer)
                {
                    Player* op = static_cast<Player*>(o);
                    if (!op->IsVisible(mover) && NotifySpawn(o, mover)) { op->AddVisible(mover); }   // 적재 성공 시에만 시야 등록
                }
            }
        }

        // 옛 시야(oldNearby)엔 있고 새 시야(newNearby)엔 없는 객체들과 mover 사이에 양방향 despawn을 통지한다.
        void Map::DespawnNoLongerVisible(GameObject* mover, bool moverIsPlayer, const std::vector<GameObject*>& oldNearby, const std::vector<GameObject*>& newNearby)
        {
            // 안 보이게 됨 = 옛 범위에 있고 새 범위엔 없는 것 - 양방향 despawn
            for (std::vector<GameObject*>::const_iterator it = oldNearby.begin(); it != oldNearby.end(); ++it)
            {
                GameObject* o = *it;
                if (o == mover) { continue; }
                if (std::find(newNearby.begin(), newNearby.end(), o) != newNearby.end()) { continue; }   // 새에도 있으면 여전히 보임
                const bool oIsPlayer = (o->GetObjectType() == EObjectType::PLAYER);

                if (moverIsPlayer)
                {
                    Player* mp = static_cast<Player*>(mover);
                    if (mp->IsVisible(o)) { NotifyDespawn(mover, o); mp->RemoveVisible(o); }
                }
                if (oIsPlayer)
                {
                    Player* op = static_cast<Player*>(o);
                    if (op->IsVisible(mover)) { NotifyDespawn(o, mover); op->RemoveVisible(mover); }
                }
            }
        }

        // to 에게 who 의 spawn 패킷을 만들어 송신 대기 버퍼에 적재한다. 항상 적재하고 true 반환(호출자가 AddVisible).
        // 송신 대기 버퍼에 한 건 적재. 고정 버퍼 경계 검사를 여기 한 곳에 모은다.
        //   _ASSERTE 는 Release 에서 사라지므로(NDEBUG) 검사 자체는 if 로 항상 켜 두고,
        //   Debug 에서만 그 자리에 멈춰 개발자가 즉시 알게 한다 - 검사는 상시, 멈춤만 선택.
        //   넘치는 방아쇠는 외부 입력이 아니라 코드 변경이다(캡 상향/필드 추가/새 패킷을 이 경로에 연결).
        //   현재 최대는 SC_SPAWN 58B(28 + 이름 15자x2) 로 캡 64B 에 6B 여유뿐이다.
        bool Map::StageOne(UINT64 toSid, KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
        {
            const int size = pkt.GetSize();
            if (size <= 0 || size > OUTBOUND_PACKET_MAX)
            {
                _ASSERTE(false && "OutboundPacket capacity exceeded - OUTBOUND_PACKET_MAX 를 넘는 패킷을 이 경로에 실었다");

                // 첫 건만 상세히 남기고 그 뒤는 카운터로만 관측한다 (매 tick 재시도라 로그가 초당 수십 건 쌓인다).
                //   같은 규약이 ChannelManager 의 pre-auth drop 에도 있다 - "drop 은 조용하되 카운터로 관측".
                if (::InterlockedCompareExchange(&s_outboundOversizeLogged, 1, 0) == 0)
                {
                    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                        KYS::GAMESERVER::LOG::LogChannel::SERVER,
                        KYS::GAMESERVER::LOG::LogLevel::LL_ERROR,
                        L"map",
                        L"송신 대기 적재 거부 - 패킷 %d B 가 고정 버퍼 %d B 를 넘었다 (sid=%llu). 이후 같은 사유는 카운터로만 센다.",
                        size, OUTBOUND_PACKET_MAX, static_cast<unsigned long long>(toSid));
                }
                ::InterlockedIncrement(&s_outboundOversizeCount);
                return false;
            }

            OutboundPacket op;
            op.sid  = toSid;
            op.size = size;
            memcpy(op.data, pkt.GetBuffer(), static_cast<size_t>(size));
            m_outboundPackets.push_back(op);
            return true;
        }

        bool Map::NotifySpawn(GameObject* to, GameObject* who)
        {
            // death grace 중 죽은 객체(hp=0 시체)도 그대로 spawn 한다 - SC_SPAWN/SC_MONSTER_SPAWN 이 hp=0 을 실어 보내고
            //   클라가 hp<=0 을 시신(회색)으로 렌더하므로 신규 시야 진입자도 시체를 대칭으로 본다. (과거엔 여기서 죽은 who 를 skip 해
            //   "죽은 관찰자는 산 자를 보는데 신규 진입자는 시체를 못 보는" 단방향 비대칭이 생겼고, 그 비대칭이 시야집합 dangling 의 근원이었다.
            //   이동 broadcast IsDead 가드(Map::Update)는 유지 - 시체는 스폰은 되나 이동 broadcast 는 안 한다[정지].)
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(MAX_PACKET_SIZE);
            if (!who->SerializeSpawn(pkt)) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", pkt.GetType(), pkt.GetSize()); return false; }   // 다형 - Player::SerializeSpawn이 Begin/End 내부 처리

            return StageOne(to->GetId(), pkt);   // 경계 검사 + 고정 버퍼 memcpy 1회 (힙 0)
        }

        // to 에게 who 의 despawn 패킷을 만들어 송신 대기 버퍼에 적재한다.
        void Map::NotifyDespawn(GameObject* to, GameObject* who)
        {
            SC_DESPAWN d;
            if (who->GetObjectType() == EObjectType::PLAYER)
                d.playerId = static_cast<Player*>(who)->GetPlayerId();   // player 전송 id = m_playerId
            else
                d.playerId = static_cast<UINT32>(who->GetId());          // monster 전송 id = m_id (통합 카운터)

            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(MAX_PACKET_SIZE);
            pkt.Begin(static_cast<USHORT>(PacketType::SC_DESPAWN));
            d.Serialize(pkt);
            if (!pkt.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", pkt.GetType(), pkt.GetSize()); return; }

            StageOne(to->GetId(), pkt);   // 반환 무시 - despawn 은 못 보내도 호출자가 할 일이 없다
        }

        // x 좌표를 셀 열 인덱스로 변환한다. 좌표 [0, 맵 폭(<=MAX_MAP_SIZE_PX)] -> 인덱스 [0, GRID_COLS).
        //   맵 끝 좌표(x==폭)는 폭/250 이 그 폭의 마지막 칸을 벗어나므로 클램프로 흡수한다.
        //   좌표가 맵 안인 것은 호출자(이동 예측 / OnMove 수용 / 포탈 진입)가 보장 - 음수/초과는 끝 좌표뿐이라 클램프로 충분.
        //   (작은 맵은 격자 위쪽 칸이 빈 채 남는다 - 배열 차원은 상한 고정, 무해.)
        int Map::ColOf(int x)
        {
            const int c = x / CELL_SIZE;
            if (c < 0) { return 0; }                       // 음수 좌표 방어 (defense-in-depth - 정상 좌표 [0,4000]엔 안 걸림)
            return c < GRID_COLS ? c : GRID_COLS - 1;      // 끝 좌표(4000)는 마지막 칸으로 흡수
        }

        // y 좌표를 셀 행 인덱스로 변환한다 (ColOf와 동일 - 양끝 클램프).
        int Map::RowOf(int y)
        {
            const int r = y / CELL_SIZE;
            if (r < 0) { return 0; }
            return r < GRID_ROWS ? r : GRID_ROWS - 1;
        }

        // 객체의 wire 전송 id - player 는 m_playerId(GetPlayerId), 그 외(monster 등)는 통합 카운터 m_id(GetId).
        static UINT32 WireIdOf(GameObject* obj)
        {
            return (obj->GetObjectType() == EObjectType::PLAYER)
                ? static_cast<UINT32>(static_cast<Player*>(obj)->GetPlayerId())
                : static_cast<UINT32>(obj->GetId());
        }

        // 완성 패킷(pkt)을 anchor 시야 범위 안 player 관찰자 전원에게 송신 대기 버퍼에 적재한다 (Stage* 3종 공통 루프).
        //   inline 송신 불가(UpdateFrame에 GetSession 없음) -> 버퍼에 쌓아 다음 tick SendObjectUpdates이 송신.
        void Map::StageToObservers(GameObject* anchor, KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
        {
            std::vector<GameObject*> observers;
            GetNearbyObjects(anchor, VIEW_RANGE, observers);
            for (std::vector<GameObject*>::iterator o = observers.begin(); o != observers.end(); ++o)
            {
                if ((*o)->GetObjectType() != EObjectType::PLAYER) { continue; }   // 수신자=player만
                StageOne((*o)->GetId(), pkt);   // 다음 tick SendObjectUpdates 이 drain
            }
        }

        // 피해 이벤트(SC_DAMAGE)를 만들어 victim 시야 범위의 player 관찰자에게 송신 대기 적재한다.
        //   앵커=victim - 피해 표시는 대상을 보는 이들이 봐야 한다(공격자 앵커면 대상만 보이는 관찰자가 누락돼 HP 불일치).
        void Map::StageDamage(GameObject* attacker, GameObject* victim, int damage)
        {
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(MAX_PACKET_SIZE);
            if (!BuildDamagePacket(pkt, WireIdOf(attacker), WireIdOf(victim), damage, victim->GetHp())) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", pkt.GetType(), pkt.GetSize()); return; }
            StageToObservers(victim, pkt);
        }

        // 사망 이벤트(SC_DEATH)를 만들어 victim 시야 범위의 player 관찰자에게 송신 대기 적재한다.
        void Map::StageDeath(GameObject* victim)
        {
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(MAX_PACKET_SIZE);
            if (!BuildDeathPacket(pkt, WireIdOf(victim))) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", pkt.GetType(), pkt.GetSize()); return; }
            StageToObservers(victim, pkt);
        }

        // 부활 이벤트(SC_RESPAWN)를 만들어 부활 player 시야 범위의 player 관찰자에게 송신 대기 적재한다.
        void Map::StageRespawn(GameObject* player)
        {
            SC_RESPAWN evt;
            evt.playerId = static_cast<Player*>(player)->GetPlayerId();
            const Position& pos = player->GetPos();
            evt.x = pos.x; evt.y = pos.y;
            evt.hp = player->GetHp(); evt.maxHp = player->GetMaxHp();
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(MAX_PACKET_SIZE);
            pkt.Begin(static_cast<USHORT>(PacketType::SC_RESPAWN));
            evt.Serialize(pkt);
            if (!pkt.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", pkt.GetType(), pkt.GetSize()); return; }
            StageToObservers(player, pkt);
        }

    }
}
