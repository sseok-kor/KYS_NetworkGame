#include "pch_serverapp.h"
#include "MapManager.h"
#include "../Channel/ChannelManager.h"   // GameSession.m_player set (Spawn) - 전방선언 -> 완전 정의
#include "../Combat/CombatFormula.h"     // RandomRange (그룹 구역 안 랜덤 스폰 위치)
#include "../Monster/MonsterTable.h"     // MonsterIsBoss (리스폰 보스 그룹 우선 처리)
#include "../../../GameCommon/MapData/MapTable.h"        // MapTableCount(순회/가드 상한)/MapTableAt(부활 좌표)/MapWidthFor·HeightFor(드랍 클램프)
#include "../../../GameCommon/MapData/WalkableTable.h"   // IsWalkable (스폰/드랍 좌표의 벽 칸 회피)
#include "../../../GameServer/Core/Log/Logger.h"   // 그룹 회계 불변식 파손 에러 로그 (조용한 흡수 금지)

namespace KYS
{
    namespace SERVERAPP
    {

        // user-defined ctor (=default 불가). ObjectPool<Monster>는 explicit ObjectPool(UINT32) 생성자뿐이라
        //   값 멤버 m_monsterPool의 capacity를 명시해야 한다. (선례: Channel.cpp ctor)
        MapManager::MapManager(int channelId)
            : m_channelId(channelId)                                            // 소유 채널 id - 스폰 몬스터에 stamp (길찾기 결과 회신 대상)
            , m_monsterPool(static_cast<UINT32>(DEFAULT_MONSTER_POOL_CAPACITY))
            , m_groundItemPool(static_cast<UINT32>(DEFAULT_GROUND_ITEM_POOL_CAPACITY))
            , m_nextObjectId(1)                                                 // 채널 안에서만 쓰는 단조 증가 id 시작값(1부터)
        {
            // m_maps는 기본 생성. 배열 멤버라 카운터/천장/그룹 회계만 본체에서 초기화.
            //   생성자는 maps.csv/spawns.csv 적재보다 먼저 돌 수 있으므로 상한 전 칸을 초기화한다 (미사용 칸 0 = 무해).
            m_spawnGroupCount = 0;         // 쓰는 그룹 수 (InitSpawns가 설정)
            for (int i = 0; i < MAX_MAP_COUNT; ++i)
            {
                m_monsterCount[i] = 0;     // 맵별 몬스터 수 (시신 grace 포함 - 스폰/디스폰 증감)
                m_mapCap[i] = 0;           // 맵별 안전 천장 (InitSpawns가 maps.csv monsterCap으로 설정)
                m_groundItemCount[i] = 0;  // 바닥 드랍 수 (cap 판정)
            }
            for (int i = 0; i < MAX_SPAWN_GROUPS; ++i)
            {
                m_spawnGroups[i].aliveCount = 0;
                m_spawnGroups[i].pendingCount = 0;
            }
        }

        // 게임 시뮬레이션 phase - 맵들을 순회하며 갱신만 한다(위치 예측/재배치/가시성/바뀜 표시). 송신은 Channel이 따로.
        void MapManager::Update(float deltaTime, UINT64 nowMs)
        {
            for (int i = 0; i < MapTableCount(); ++i)
            {
                m_maps[i].Update(deltaTime);   // 각 맵: 다형 Update(위치 예측 + 몬스터 교전 대상 즉석 스캔)->셀 재배치->바뀜 표시 (송신 안 함)
            }
            SweepDead(nowMs);   // 시신 유지 시간 지난 몬스터 회수 - 맵 루프 뒤 (이번 tick 갓 죽은 몹은 다음 tick부터 누산). 회수 시점에 그 몹의 그룹에 부활 예약(now+delay)이 걸린다
            RespawnDueGroups(nowMs);   // 부활 시각이 된 예약을 그룹 구역 랜덤 위치에 스폰 (스폰 성공 시에만 예약 소비 - 실패/천장 보류면 잔류 재시도)
            SweepExpiredGroundItems(nowMs);   // 만료된 바닥 드랍 회수 (수집 후 제거 2단계 - SweepDead와 동일 패턴)
        }

        // 바닥 드랍 생성 - 몬스터 사망 드랍/버리기 공용. cap 초과 시 그 맵의 가장 오래된 드랍부터 밀어낸다.
        GroundItem* MapManager::SpawnGroundItem(int mapId, const Position& pos, int templateId, int quantity,
                                                const UINT32 ownerCharIds[3], UINT64 nowMs)
        {
            if (mapId < 0 || mapId >= MapTableCount()) { return nullptr; }   // 범위 가드 (Allocate 앞 - 슬롯 누수 차단)

            // 맵당 상한 - 초과 시 가장 오래된 드랍 제거 (버리기 고속 반복으로 맵을 채우는 도배 방어).
            if (m_groundItemCount[mapId] >= GROUND_ITEM_MAP_CAP)
            {
                GroundItem* oldest = nullptr;
                const std::vector<GameObject*>& objs = m_maps[mapId].GetObjects();
                for (std::vector<GameObject*>::const_iterator it = objs.begin(); it != objs.end(); ++it)
                {
                    if ((*it)->GetObjectType() != EObjectType::GROUND_ITEM) { continue; }
                    GroundItem* g = static_cast<GroundItem*>(*it);
                    if (oldest == nullptr || g->GetSpawnTimeMs() < oldest->GetSpawnTimeMs()) { oldest = g; }
                }
                if (oldest != nullptr) { DespawnGroundItem(oldest); }
            }

            GroundItem* item = m_groundItemPool.Allocate(static_cast<UINT64>(m_nextObjectId++));   // id 발급 (몬스터와 공유 발급기)
            if (item == nullptr) { --m_nextObjectId; return nullptr; }   // 풀 초과 - 소모한 id 회수 (SpawnMonster와 대칭)

            // 좌표는 그 맵 경계로 클램프 (사망 좌표 주변 분산이 경계를 넘을 수 있음. 맵마다 크기 다름).
            //   벽 칸 회피는 호출자 몫 - 분산 전 원점(사망 좌표)을 아는 쪽만 통행 칸으로 되돌릴 수 있다
            //   (GenerateDrops 가 분산 직후 세탁. 버리기는 플레이어 발밑 = 통행 보장이라 무검사).
            const int mapW = MapWidthFor(mapId);
            const int mapH = MapHeightFor(mapId);
            Position clamped = pos;
            clamped.x = (clamped.x < 0) ? 0 : ((clamped.x > mapW) ? mapW : clamped.x);
            clamped.y = (clamped.y < 0) ? 0 : ((clamped.y > mapH) ? mapH : clamped.y);

            item->SetMapId(mapId);
            item->SetPos(clamped);
            item->InitDrop(templateId, quantity, nowMs, ownerCharIds);
            // --- 완전 초기화 끝 ---

            m_maps[mapId].Insert(item);                // 그 맵 격자에 등록 (다형 GameObject*)
            m_maps[mapId].SpawnVisibilityFor(item);    // 관찰 player에 SC_GROUND_ITEM_SPAWN (SerializeSpawn 다형)
            ++m_groundItemCount[mapId];
            return item;
        }

        // 드랍 회수 (줍기 성공/만료/밀어내기). DespawnMonster와 같은 순서 - 통지 먼저, 그다음 격자 제거와 풀 반납.
        void MapManager::DespawnGroundItem(GameObject* obj)
        {
            if (obj == nullptr) { return; }
            const int mapId = obj->GetMapId();
            m_maps[mapId].DespawnVisibilityFor(obj);                     // 관찰 player에 SC_DESPAWN (기존 경로 재사용)
            m_maps[mapId].Erase(obj);                                    // 맵 목록 + 셀에서 제거
            m_groundItemPool.Deallocate(static_cast<GroundItem*>(obj));  // 호출자가 GROUND_ITEM만 넘김
            if (mapId >= 0 && mapId < MapTableCount()) { --m_groundItemCount[mapId]; }
        }

        // 줍기 요청의 objectId로 그 맵의 드랍을 찾는다. 줍기는 저빈도라 선형 탐색으로 충분 (맵당 cap 이하).
        GroundItem* MapManager::FindGroundItem(int mapId, UINT32 objectId)
        {
            if (mapId < 0 || mapId >= MapTableCount()) { return nullptr; }
            const std::vector<GameObject*>& objs = m_maps[mapId].GetObjects();
            for (std::vector<GameObject*>::const_iterator it = objs.begin(); it != objs.end(); ++it)
            {
                if ((*it)->GetObjectType() != EObjectType::GROUND_ITEM) { continue; }
                if (static_cast<UINT32>((*it)->GetId()) == objectId) { return static_cast<GroundItem*>(*it); }
            }
            return nullptr;
        }

        // 만료된 바닥 드랍 회수. 순회 중 격자를 바꾸면 반복자가 깨지므로 수집 후 제거 2단계 (SweepDead와 동일).
        void MapManager::SweepExpiredGroundItems(UINT64 nowMs)
        {
            std::vector<GameObject*> expired;
            for (int i = 0; i < MapTableCount(); ++i)
            {
                if (m_groundItemCount[i] <= 0) { continue; }   // 드랍 없는 맵은 순회 생략
                const std::vector<GameObject*>& objs = m_maps[i].GetObjects();
                for (std::vector<GameObject*>::const_iterator it = objs.begin(); it != objs.end(); ++it)
                {
                    if ((*it)->GetObjectType() != EObjectType::GROUND_ITEM) { continue; }
                    GroundItem* g = static_cast<GroundItem*>(*it);
                    if (g->IsExpired(nowMs)) { expired.push_back(*it); }
                }
            }
            for (std::vector<GameObject*>::iterator it = expired.begin(); it != expired.end(); ++it)
            {
                DespawnGroundItem(*it);   // 통지(SC_DESPAWN) -> Erase -> 풀 반납 -> --count
            }
        }

        // join - 풀에 이미 할당된 player를 조회해 id 발급 후 맵 격자에 등록 + 진입 시야 범위 양방향 spawn.
        //   sid   : 들어온 세션
        //   channelManager : GameSession에서 player 조회용
        void MapManager::AddPlayer(UINT64 sid, ChannelManager* channelManager)
        {
            Player* p = channelManager->GetPlayer(sid);   // Allocate 대신 조회 (이미 AttachPlayer가 할당)
            if (p == nullptr)
            {
                return;
            }
            // mapId는 producer(DBResultJob)가 정한 값을 읽음 (DBResultJob이 범위 검증/세탁을 이미 함 - 여긴 2차 방어).
            int mapId = p->GetMapId();
            if (mapId < 0 || mapId >= MapTableCount())
            {
                mapId = 0;            // DB 손상 등 범위 밖 방어 - m_maps 배열 보호 + 안 보이는 좀비 회피
                p->SetMapId(0);
                p->SetPos(MapTableAt(0).respawn);   // 옛 맵 좌표를 그대로 두면 맵0 의 엉뚱한 지점(벽 안 가능)에 서게 됨 - 부활 지점으로 함께 리셋
            }
            p->SetPlayerId(m_nextObjectId++);
            // pos도 producer가 세팅(로그인=DB 저장 좌표, 신규 캐릭은 INSERT 기본값=맵0 부활 지점, 채널 이관=직전 위치) - Insert가 GetPos()로 셀 계산하므로 여기서 SetPos 안 함(덮어쓰기 회피).
            m_maps[mapId].Insert(p);
            m_maps[mapId].SpawnVisibilityFor(p);
        }

        // 몬스터 스폰 - 완전 초기화 후 격자에 등록, 그 다음 관찰자에 통지 (순서 불변, 2단계 윈도우 봉인).
        //   info     : 스폰 정보(맵/위치/종류/배회 기준점)
        //   groupIdx : 소속 스폰 그룹 인덱스 (시신 회수가 이 그룹에 부활 예약을 걸기 위한 역참조)
        // 반환 false = 스폰 실패(범위/풀 고갈) - 호출자가 예약을 소비하지 않아야 인구가 새지 않는다.
        bool MapManager::SpawnMonster(const MonsterSpawnInfo& info, int groupIdx)
        {
            // mapId 범위 가드 (Allocate 앞 - 범위 밖이면 풀 슬롯 누수 없이 차단. ChangeMap 선례).
            if (info.mapId < 0 || info.mapId >= MapTableCount()) { return false; }

            Monster* m = m_monsterPool.Allocate(static_cast<UINT64>(m_nextObjectId++));   // id 발급(ctor가 m_id에)
            if (m == nullptr) { --m_nextObjectId; return false; }   // 풀 초과 - 소모한 id 회수(실패 반복이 id 공간을 갉지 않게) + 예약 잔류

            m->SetMapId(info.mapId);
            m->SetPos(info.pos);
            m->SetMonsterType(info.monsterType);            // 종류 보존 (스폰 후 종류 소실 버그 수정)
            m->SetSpawnGroupIdx(groupIdx);                  // 소속 그룹 (시신 회수 -> 부활 예약 경로)
            const MonsterTemplate& monsterTemplate = MonsterTemplateFor(info.monsterType);   // 종류별 스탯
            m->SetMaxHp(monsterTemplate.hp);                  // 기본 HP 세팅
            m->SetHp(monsterTemplate.hp);
            m->SetPatrolAnchor(info.patrolAnchor); // 집(배회 기준점) + 배회 시작점. 상태는 ctor가 IDLE - state setter 호출 없음
            m->SetChannelId(m_channelId);          // 소속 채널 (길찾기 요청/결과 회신 대상 - 몬스터는 sid 가 없어 채널을 직접 안다)
            m->SetMap(&m_maps[info.mapId]);        // 자기 맵 back-pointer (Update가 ResolveTarget로 교전 대상 스캔에 사용). Insert 전 set - Update가 m_map을 쓰므로 선행.
            // (SetSyncMode 없음 - 전 몬스터 단일 event-driven)
            // --- 완전 초기화 끝 ---

            m_maps[info.mapId].Insert(m);                 // 그 맵 격자에 등록 (다형 GameObject*)
            m_maps[info.mapId].SpawnVisibilityFor(m);     // 관찰 player에 SC_MONSTER_SPAWN (관찰자 필터)
            ++m_monsterCount[info.mapId];                 // 맵별 몬스터 수 증가 (천장 판정 + 모니터링)
            return true;
        }

        // leave - 맵 격자에서 player 제거 (통지는 호출자가 이미 함). 비타겟팅이라 몬스터 타겟 해제는 불필요.
        //   sid   : 나가는 세션
        //   channelManager : GameSession/player 조회용
        void MapManager::RemovePlayer(UINT64 sid, ChannelManager* channelManager)
        {
            GameSession* gs = channelManager->GetSession(sid);
            if (gs == nullptr)
            {
                return;
            }

            Player* p = gs->GetPlayer();
            if (p == nullptr)
            {
                return;
            }

            // 파괴 전 정리 - 셀에서 제거. 가시성 통지(SC_DESPAWN)는 OnClientLeave가 이미 함(일원화).
            //   비타겟팅 전환으로 몬스터가 raw Player*를 저장하지 않으므로 타겟 해제 정리(옛 ClearMonsterTargetsOnPlayer)는 불필요해졌다.
            m_maps[p->GetMapId()].Erase(p);                         // 맵 목록 + 셀에서 제거 (캐시 셀)
        }

        // 몬스터 디스폰 - 통지 후 격자에서 제거하고 풀에 반납 (반납을 맨 마지막에 해 끊긴 포인터 봉인).
        //   그룹 회계(alive 감소 + 부활 예약)는 여기가 아니라 호출자(SweepDead)가 한다 - 이 함수는 소멸 절차만.
        //   (미래에 그룹 소속 몹을 이 함수로 직접 제거하는 호출자는 그룹 alive 감산까지 스스로 책임져야 한다 - 여기서 예약을 걸면 GM 제거류가 자동 리스폰을 유발하므로 분리 유지.)
        void MapManager::DespawnMonster(GameObject* obj)
        {
            if (obj == nullptr) { return; }
            const int mapId = obj->GetMapId();
            m_maps[mapId].DespawnVisibilityFor(obj);                  // 관찰 player에 despawn 통지 (먼저)
            m_maps[mapId].Erase(obj);                                 // 맵 목록 + 셀 + 바뀜 목록에서 제거
            m_monsterPool.Deallocate(static_cast<Monster*>(obj));     // obj=monster 보장 (호출자가 monster만 넘김)
            if (mapId >= 0 && mapId < MapTableCount())
            {
                --m_monsterCount[mapId];                              // 맵별 몬스터 수 감소 (천장 판정 + 모니터링)
            }
        }

        // 포탈 - 옛 맵에서 떼어내고(격자/가시성) 새 맵으로 옮긴다(좌표 적용/격자/가시성). 풀은 안 건드림(주소 불변).
        //   mover   : 이동하는 객체 (레이어 B는 Player만)
        //   toMapId : 도착 맵
        //   dst     : 도착 좌표 (서버 테이블이 정함, 서버 권위)
        bool MapManager::ChangeMap(GameObject* mover, int toMapId, const Position& dst)
        {
            // 레이어 B는 Player만 포탈한다 - monster면 아래 Player 캐스트가 UB. 진입부 가드로 불변식 강제.
            //   (중복 ClearVisible 제거로 '시야집합 비움'이 DespawnVisibilityFor PLAYER 분기에 의존하므로 더 중요.)
            if (mover == nullptr || mover->GetObjectType() != EObjectType::PLAYER) { return false; }

            // 도착 맵 검증 (서버 권위 - 텔레포트 핵 차단)
            if (toMapId < 0 || toMapId >= MapTableCount()) { return false; }      // 범위 밖 = 무시(no-op)
            const int fromMapId = mover->GetMapId();
            if (toMapId == fromMapId) { return false; }                     // 같은 맵 = 변화 없음(no-op)
            Player* mp = static_cast<Player*>(mover);                 // 가시집합/playerId용 (레이어 B Player만)

            // 옛 가시성은 SetPos 전에 - mover->GetPos()가 옛 위치여야 옛 시야 범위를 본다.

            DetachFromOldMap(mover, fromMapId, mp);   // 옛 맵 격자 제거 + 양방향 despawn ((b) despawn 후 (a) 순서)

            // 맵 이동 + 도착 좌표 적용 (서버 결정, 서버 권위)
            mover->SetMapId(toMapId);
            mover->SetPos(dst);                                      // 포탈 테이블 도착 좌표 (하드코딩 좌표 안 씀)

            // 새 맵 격자에 등록
            m_maps[toMapId].Insert(mover);

            // 새 맵 가시성 - 양방향 spawn (진입 시야 범위)
            m_maps[toMapId].SpawnVisibilityFor(mover);

            // ChangeMap은 바뀜 표시를 안 한다 - 새 위치는 위 SC_SPAWN으로 이미 보냈고,
            //    정지면 다음 tick broadcast 불필요. 포탈 직후 CS_MOVE면 그 OnMove가 바뀜 표시 -> 자연 처리.
            return true;
        }

        // 옛 맵에서 mover를 떼어낸다 - 격자 제거 + 양방향 despawn ((b) despawn 후 (a) 순서).
        void MapManager::DetachFromOldMap(GameObject* mover, int fromMapId, Player* mp)
        {
            // 비타겟팅 전환으로 몬스터가 raw Player*를 저장하지 않는다 - 포탈로 떠난 player를 옛 맵 몬스터가 계속
            //   가리킬 수 없으니 옛 cross-map use-after-free가 구조적으로 불가. 타겟 해제 정리(옛 ClearMonsterTargetsOnPlayer) 불필요.

            // 옛 맵 격자에서 떼어냄 (캐시된 셀로 위치 역산 없이)
            m_maps[fromMapId].Erase(mover);

            // 옛 맵 가시성 정리 - 양방향 despawn. 둘 다 mover 시야집합을 읽으므로 (b)를 (a) 앞에 둔다.
            //   (a) DespawnVisibilityFor가 내부에서 mover 시야집합을 비우므로, (b)가 뒤면 빈 집합을 돌아
            //   mover 본인이 옛 객체 SC_DESPAWN을 못 받아 화면에 ghost로 남는다 -> (b)를 먼저.
            //   (b) mover가 보던 옛 객체 전부 -> mover에 SC_DESPAWN (시야 비우기 전에 먼저)
            const std::unordered_set<GameObject*>& vis = mp->GetVisibleObjects();
            for (std::unordered_set<GameObject*>::const_iterator it = vis.begin(); it != vis.end(); ++it)
            {
                m_maps[fromMapId].NotifyDespawnTo(mover, *it);        // mover에게 *it의 despawn
            }
            //   (a) mover를 보던 옛 맵 관찰자 -> SC_DESPAWN{mover} + (내부에서) mover 시야집합 비움
            m_maps[fromMapId].DespawnVisibilityFor(mover);
        }

        // forwarder - mover의 맵에 근처 객체 수집을 위임한다 (m_maps가 private이라 Channel이 직접 못 만짐).
        void MapManager::GetNearbyObjectsFor(const GameObject* origin, int rangePx, std::vector<GameObject*>& out)
        {
            m_maps[origin->GetMapId()].GetNearbyObjects(origin, rangePx, out);   // Channel::SendObjectUpdates broadcast - Channel이 GetId->Send
        }

        // forwarder - leaver의 맵에 떠남 시 despawn 통지를 위임한다.
        void MapManager::DespawnVisibilityForOnLeave(GameObject* leaver)
        {
            m_maps[leaver->GetMapId()].DespawnVisibilityFor(leaver);   // leave 통지
        }

        // 모든 맵에서 이번 tick 위치가 바뀐 객체를 out에 모은다 (Channel::SendObjectUpdates이 mover별로 broadcast).
        void MapManager::CollectMovedObjects(std::vector<GameObject*>& out)
        {
            for (int i = 0; i < MapTableCount(); ++i)
            {
                const std::vector<GameObject*>& movedObjects = m_maps[i].GetMovedObjects();   // 맵별 바뀜 목록
                for (std::vector<GameObject*>::const_iterator it = movedObjects.begin(); it != movedObjects.end(); ++it)
                {
                    out.push_back(*it);
                }
            }
        }

        // 모든 맵의 바뀜 목록을 비운다 (SendObjectUpdates 송신 후 - Map::Update는 안 비움).
        void MapManager::ClearMovedObjects()
        {
            for (int i = 0; i < MapTableCount(); ++i)
            {
                m_maps[i].ClearMovedObjects();
            }
        }

        // 이번 tick 죽은 플레이어 sid를 모든 맵에서 모은다 (Channel::UpdateFrame이 거래 강제 종결에 사용).
        //   맵별 목록은 각 Map::Update 시작 시 자체 clear 하므로 여기선 읽기만 (별도 Clear 불필요).
        void MapManager::CollectDiedPlayers(std::vector<UINT64>& out)
        {
            for (int i = 0; i < MapTableCount(); ++i)
            {
                const std::vector<UINT64>& died = m_maps[i].GetDiedPlayers();
                for (std::vector<UINT64>::const_iterator it = died.begin(); it != died.end(); ++it)
                {
                    out.push_back(*it);
                }
            }
        }

        // 모든 맵의 송신 대기 패킷을 out에 모은다.
        void MapManager::CollectOutbound(std::vector<OutboundPacket>& out)
        {
            for (int i = 0; i < MapTableCount(); ++i)
            {
                const std::vector<OutboundPacket>& pkts = m_maps[i].GetOutboundPackets();
                for (std::vector<OutboundPacket>::const_iterator it = pkts.begin(); it != pkts.end(); ++it)
                {
                    out.push_back(*it);   // 패킷 복사(sid + bytes) - spawn/despawn은 드물어 비용 무시 가능
                }
            }
        }

        // 모든 맵의 송신 대기 패킷을 비운다 (송신 후).
        void MapManager::ClearOutbound()
        {
            for (int i = 0; i < MapTableCount(); ++i)
            {
                m_maps[i].ClearOutboundPackets();
            }
        }

        // 모든 맵의 player만 모아 out에 담는다 (DB 저장 대상 - monster는 영속성 없음).
        void MapManager::CollectPlayersForSave(std::vector<Player*>& out)
        {
            for (int i = 0; i < MapTableCount(); ++i)
            {
                const std::vector<GameObject*>& objects = m_maps[i].GetObjects();   // 맵 전체 순회
                for (std::vector<GameObject*>::const_iterator it = objects.begin(); it != objects.end(); ++it)
                {
                    // EObjectType 판별 필수 - monster도 같은 컬렉션에 섞여 있음.
                    //    player만 DB 저장 대상(monster는 영속성 없음, 배회만, 재생성).
                    if ((*it)->GetObjectType() == EObjectType::PLAYER)
                    {
                        out.push_back(static_cast<Player*>(*it));
                    }
                }
            }
        }

        // 같은 맵에서 이름이 일치하는 다른 player 를 찾는다 (거래 상대 조회 - 같은 맵 게이트).
        //   monster 도 같은 컬렉션에 섞여 있어 EObjectType 판별 필수. 자기 자신(excludeSid)은 건너뛴다.
        Player* MapManager::FindPlayerByNameInMap(int mapId, const wchar_t* name, UINT64 excludeSid)
        {
            if (mapId < 0 || mapId >= MapTableCount()) { return nullptr; }
            const std::vector<GameObject*>& objects = m_maps[mapId].GetObjects();
            for (std::vector<GameObject*>::const_iterator it = objects.begin(); it != objects.end(); ++it)
            {
                GameObject* obj = *it;
                if (obj->GetObjectType() != EObjectType::PLAYER) { continue; }
                Player* other = static_cast<Player*>(obj);
                if (other->GetSid() == excludeSid) { continue; }   // 자기 자신은 상대가 아님
                if (wcscmp(other->GetName(), name) == 0) { return other; }
            }
            return nullptr;
        }

        // 한 맵의 모든 player에게 같은 바이트를 보낸다.
        //   mapId : 대상 맵
        //   data  : 보낼 바이트 (이미 직렬화/프레이밍됨)
        //   size  : 바이트 수
        //   flushList : tick-end에 한 번에 flush할 세션 목록 (배치 대상 누적)
        int MapManager::BroadcastToMapBatched(int mapId, int ownerChannelId, const BYTE* data, int size, std::vector<GameSession*>& flushList)
        {
            if (mapId < 0 || mapId >= MapTableCount())
            {
                return 0;   // 범위 가드
            }

            int recipients = 0;
            const std::vector<GameObject*>& objects = m_maps[mapId].GetObjects();   // 맵 전체 순회
            for (std::vector<GameObject*>::const_iterator it = objects.begin(); it != objects.end(); ++it)
            {
                GameObject* obj = *it;
                if (obj->GetObjectType() != EObjectType::PLAYER)
                {
                    continue;   // 수신자=player만 - monster GetId()는 전송용 id(sid 아님)라 오송신 봉인
                }
                // 역포인터(Player->GameSession)로 송신 대상 획득 - 전역 SRW read락 우회(broadcast hot-path, 이동 broadcast와 동일 경로).
                GameSession* gs = static_cast<Player*>(obj)->GetGameSession();
                if (gs != nullptr && gs->GetChannelId() == ownerChannelId)   // 채널 소유권 - 이관된 세션의 배치를 옛 채널이 만지면 두 스레드가 같은 버퍼를 쓴다(Channel::BatchTo 와 같은 판정)
                {
                    // 배치 누적(즉시 Send 아님) - flush 대상 1회 등록 + 수신자 버퍼에 누적. tick-end FlushBatch가 1 WSASend.
                    if (!gs->IsBatchQueued()) { flushList.push_back(gs); gs->SetBatchQueued(true); }
                    gs->AppendToBatch(data, size);
                    ++recipients;
                }
            }
            return recipients;   // 호출자가 송신 바이트 계측(size x recipients)
        }

        // 맵별 현재 몬스터 수 (모니터링용 - 채널 스레드가 갱신, 메인이 근사값으로 읽음).
        int MapManager::GetMonsterCount(int mapId) const
        {
            if (mapId < 0 || mapId >= MapTableCount()) { return 0; }
            return m_monsterCount[mapId];
        }

        // 길찾기 결과 재주입 - mapId 맵을 순회해 wire id 일치하는 살아있는 몬스터 반환 (없으면 nullptr).
        //   맵당 객체 <=수십이라 선형 순회 무해. 시신(IsDead)은 제외 - 결과가 늦게 온 사이 죽었으면 폐기.
        //   [ABA 방어의 유일 축 = wire id 무재사용(단조)]: 길찾기 중 대상이 죽고 슬롯이 재사용돼도 새 몬스터는
        //   m_nextObjectId++ 로 항상 새 id 를 받으므로 옛 id 의 in-flight 결과는 여기서 매칭 실패로 폐기된다.
        //   seq 재검증은 "같은 몬스터 생애 내 늦은 결과"만 거른다(풀 재사용 시 ctor 가 seq=0 리셋 - ABA 무방비).
        //   -> id 발급을 슬롯 인덱스형/재사용형으로 바꾸면 이 방어가 조용히 뚫린다(변경 시 결과에 스폰 세대 동봉 필요).
        Monster* MapManager::FindMonsterForPath(int mapId, UINT32 monsterId)
        {
            if (mapId < 0 || mapId >= MapTableCount()) { return nullptr; }
            const std::vector<GameObject*>& objects = m_maps[mapId].GetObjects();
            for (size_t i = 0; i < objects.size(); ++i)
            {
                GameObject* o = objects[i];
                if (o->GetObjectType() != EObjectType::MONSTER) { continue; }
                if (o->IsDead()) { continue; }
                if (static_cast<UINT32>(o->GetId()) != monsterId) { continue; }
                return static_cast<Monster*>(o);
            }
            return nullptr;
        }

        // 시신 유지 시간(MONSTER_DEATH_GRACE_SEC)이 지난 몬스터를 맵에서 회수하고, 그 몹의 그룹에 부활 예약을 건다.
        //   순회 중 격자를 바꾸면 안 되므로 1차 순회로 만료분만 모으고, 끝난 뒤 일괄 회수 (지연 회수).
        //   부활 예약 시각 = 회수 시점 + 그룹 respawnDelayMs. 체감 부활 = 시신 유지(1s) + respawnDelayMs (delay는 회수 시점부터 기산 - 합산이지 포함이 아님).
        //   nowMs : 채널 tick 시계 ms (예약 시각의 기준 - 부활 판정(RespawnDueGroups)과 같은 시계여야 한다)
        void MapManager::SweepDead(UINT64 nowMs)
        {
            std::vector<GameObject*> expired;
            for (int i = 0; i < MapTableCount(); ++i)
            {
                const std::vector<GameObject*>& objs = m_maps[i].GetObjects();
                for (std::vector<GameObject*>::const_iterator it = objs.begin(); it != objs.end(); ++it)
                {
                    GameObject* o = *it;
                    if (o->GetObjectType() != EObjectType::MONSTER) { continue; }
                    Monster* m = static_cast<Monster*>(o);
                    if (m->IsDeadExpired()) { expired.push_back(o); }
                }
            }
            for (std::vector<GameObject*>::iterator it = expired.begin(); it != expired.end(); ++it)
            {
                // 그룹 역참조는 풀 반납(DespawnMonster 안 Deallocate) 전에 확보한다 (반납 후 접근 금지).
                const int groupIdx = static_cast<Monster*>(*it)->GetSpawnGroupIdx();

                DespawnMonster(*it);   // 통지 -> Erase(셀 unlink + 바뀜 목록 제거) -> 풀 반납 -> --m_monsterCount

                // 그룹 회계 - alive 감소 + 부활 예약. 불변식 aliveCount + pendingCount == count 라 가드는 정상 경로에서 발동하지 않는다 -
                //   발동했다면 회계 버그(예: 다른 코드가 그룹 몹을 회계 없이 제거)이므로 조용히 흡수하지 않고 에러 로그로 드러낸다.
                if (groupIdx >= 0 && groupIdx < m_spawnGroupCount)
                {
                    SpawnGroupRuntime& group = m_spawnGroups[groupIdx];
                    if (group.aliveCount > 0)
                    {
                        --group.aliveCount;
                    }
                    else
                    {
                        KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                            KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"spawn",
                            L"스폰 그룹 %d 회계 이상 - alive 0 인데 시신 회수 (불변식 파손)", groupIdx);
                    }
                    if (group.pendingCount < group.def.count)   // 불변식 기준 가드 (배열 상한이 아니라 그룹 정원)
                    {
                        group.pendingDue[group.pendingCount] = nowMs + static_cast<UINT64>(group.def.respawnDelayMs);
                        ++group.pendingCount;
                    }
                    else
                    {
                        KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                            KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"spawn",
                            L"스폰 그룹 %d 회계 이상 - 예약 %d 가 정원 %d 도달 상태서 추가 시도 (불변식 파손)", groupIdx, group.pendingCount, group.def.count);
                    }
                }
            }
        }

        // 그룹 구역 랜덤 위치에 1마리 스폰을 시도한다. 성공하면 그룹 alive까지 증가시켜 회계가 스폰과 같은 지점에서 닫힌다.
        //   실패(false) 사유 = 맵 천장 도달(안전망 - 로더가 count 합<=cap 을 검증하므로 정상 데이터에선 도달하지 않음)
        //   또는 풀 고갈/범위 밖. 호출자는 실패 시 예약을 소비하지 않는다 (다음 tick 재시도 - 열린 루프 영구 결손 봉인).
        bool MapManager::TrySpawnFromGroup(int groupIdx)
        {
            SpawnGroupRuntime& group = m_spawnGroups[groupIdx];
            const int mapId = group.def.mapId;

            if (mapId >= 0 && mapId < MapTableCount() && m_monsterCount[mapId] >= m_mapCap[mapId])
            {
                return false;   // 맵 천장 - 스폰 보류 (예약은 호출자가 잔류시킴)
            }

            // 구역 안 랜덤 - 벽 칸이 나오면 다시 뽑는다 (상한 있음 - 전부 실패면 이번 tick 포기 = 예약 잔류 재시도.
            //   로더가 "구역 안 통행 칸 존재"를 부팅에서 검증하므로 정상 데이터에선 몇 회 안에 뽑힌다).
            MonsterSpawnInfo info;
            info.mapId = mapId;
            bool foundSpot = false;
            for (int attempt = 0; attempt < WALK_SPOT_RETRY_MAX; ++attempt)
            {
                info.pos.x = CombatFormula::RandomRange(group.def.x0, group.def.x1);
                info.pos.y = CombatFormula::RandomRange(group.def.y0, group.def.y1);
                if (IsWalkable(mapId, info.pos.x, info.pos.y)) { foundSpot = true; break; }
            }
            if (!foundSpot) { return false; }   // 벽만 뽑힘 - 예약 잔류 (다음 tick 재시도)

            info.monsterType = group.def.monsterType;
            info.patrolAnchor = info.pos;   // 부활 위치 == 배회 기준점 불변식

            if (!SpawnMonster(info, groupIdx)) { return false; }   // 풀 고갈 등 - 예약 잔류
            ++group.aliveCount;
            return true;
        }

        // 부활 시각이 된 예약을 스폰한다. 스폰 성공 시에만 예약을 소비한다 (실패/천장 보류 = 잔류, due 불변, 다음 tick 재시도).
        //   2-pass: 보스 그룹을 먼저 처리한다 - 맵 천장이 발동하는 비상 상황에서 보스 자리가 잡몹 예약에 밀리지 않게
        //   (정상 데이터는 로더 검증으로 천장이 발동하지 않아 순서 무관 - 미래 유입 대비).
        void MapManager::RespawnDueGroups(UINT64 nowMs)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                const bool bossPass = (pass == 0);
                for (int i = 0; i < m_spawnGroupCount; ++i)
                {
                    SpawnGroupRuntime& group = m_spawnGroups[i];
                    if (MonsterIsBoss(group.def.monsterType) != bossPass) { continue; }

                    for (int k = 0; k < group.pendingCount; )   // 소비 시 swap-pop 이라 수동 진행
                    {
                        if (group.pendingDue[k] > nowMs) { ++k; continue; }     // 아직 부활 시각 아님
                        if (!TrySpawnFromGroup(i)) { ++k; continue; }           // 천장 보류/풀 실패 - 예약 잔류 (다음 tick 재시도)
                        --group.pendingCount;                                   // 스폰 성공 - 예약 소비 (swap-pop)
                        group.pendingDue[k] = group.pendingDue[group.pendingCount];
                    }
                }
            }
        }

        // 부팅 1회: 스폰 그룹 표를 등록 + 맵별 안전 천장 설정 + 그룹별 count 초기 채움. (호출 1회 전제 - 재호출 시 그룹 상태가 리셋된다.)
        //   groups  : 스폰 그룹 배열 (spawns.csv 적재 표 - SpawnGroupData. 로더가 count/구역/천장 정합을 이미 검증)
        //   count   : groups 원소 수
        //   mapCaps : 맵별 안전 천장 배열 (MAX_MAP_COUNT 크기 - 쓰는 칸은 MapTableCount()개, maps.csv monsterCap)
        void MapManager::InitSpawns(const SpawnGroup* groups, size_t count, const int* mapCaps)
        {
            if (m_spawnGroupCount != 0)
            {
                // 재호출 방어 - 그룹 회계만 리셋되고 기존 몬스터/맵 카운터는 남아 정원 초과 성장으로 이어지므로 거부한다 (부팅 1회 계약).
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"spawn",
                    L"InitSpawns 재호출 거부 - 부팅 1회 계약 (기존 그룹 %d개 유지)", m_spawnGroupCount);
                return;
            }
            m_spawnGroupCount = (count > static_cast<size_t>(MAX_SPAWN_GROUPS)) ? MAX_SPAWN_GROUPS : static_cast<int>(count);   // 로더가 상한을 이미 검증 - 방어
            for (int i = 0; i < m_spawnGroupCount; ++i)
            {
                m_spawnGroups[i].def = groups[i];
                m_spawnGroups[i].aliveCount = 0;
                m_spawnGroups[i].pendingCount = 0;
            }
            for (int i = 0; i < MapTableCount(); ++i)
            {
                m_mapCap[i] = mapCaps[i];
            }

            // 초기 채움 - 그룹당 count 마리. 실패분(풀 고갈 등)은 due=0 예약으로 이월한다 -
            //   열린 루프(예약이 유일한 스폰 트리거)에서 실패를 버리면 그 자리는 영구 결손이기 때문.
            //   부팅 시점은 채널 타이머 미초기화 구간이라 시계를 읽지 않는다 - due 0 = "채널 가동 후 즉시"(첫 tick nowMs>=0 에서 due<=nowMs 성립).
            for (int i = 0; i < m_spawnGroupCount; ++i)
            {
                SpawnGroupRuntime& group = m_spawnGroups[i];
                for (int k = 0; k < group.def.count; ++k)
                {
                    if (TrySpawnFromGroup(i)) { continue; }
                    if (group.pendingCount < MAX_GROUP_COUNT)
                    {
                        group.pendingDue[group.pendingCount] = 0;   // 채널 가동 후 즉시 재시도
                        ++group.pendingCount;
                    }
                }
            }
        }

    }
}
