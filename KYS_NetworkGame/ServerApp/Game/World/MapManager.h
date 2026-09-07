#pragma once
#include "Map.h"
#include "../Player/Player.h"                       // Player* 시그니처 (저장 대상 수집/이름 조회/포탈 정리)
#include "../Monster/Monster.h"
#include "../Monster/SpawnTable.h"                  // SpawnGroup (그룹 정의 - 런타임 회계 구조가 값 보유)
#include "../Item/GroundItem.h"                     // ObjectPool<GroundItem> 완전 타입 (바닥 드랍)
#include "../../../GameServer/Core/Memory/ObjectPool.h"   // KYS::GAMESERVER::MEMORY::ObjectPool<T>
#include "../../../GameCommon/GameDefines.h"        // MAX_MAP_COUNT
#include "../../../GameCommon/GameTypes.h"          // PlayerId
#include <vector>

namespace KYS
{
    namespace SERVERAPP
    {

        class ChannelManager;   // 전방 선언 - Spawn이 GameSession.m_player set에 사용(.cpp에서 include)
        class GameSession;      // 전방 선언 - BroadcastToMapBatched flushList 원소 타입(포인터만, .cpp에서 완전 정의)

        // 한 채널의 맵들(m_maps)과 몬스터 풀을 소유. 객체 생성/삭제/맵이동 + tick 갱신 + AOI/바뀜/송신 수집의 진입점.
        class MapManager
        {
        public:
            explicit MapManager(int channelId); // user-defined ctor - m_monsterPool capacity 명시 필수 + channelId(스폰 몬스터 stamp = 길찾기 결과 회신 대상)
            ~MapManager() = default;            // 비가상 (값 멤버, 다형 base 아님)

            // 풀/맵(이동 불가 자원) 보유 + Channel 값 멤버 거주 -> 복사/이동 차단
            MapManager(const MapManager&) = delete;
            MapManager& operator=(const MapManager&) = delete;
            MapManager(MapManager&&) = delete;
            MapManager& operator=(MapManager&&) = delete;

            // 게임 시뮬레이션 phase - 맵 순회 갱신만(위치 예측/재배치/가시성/바뀜 표시). 송신은 Channel이 따로.
            void Update(float deltaTime, UINT64 nowMs);   // dt=고정 sim timestep, nowMs=채널 tick 시계 ms(부활 예약 시각 비교용)

            void AddPlayer(UINT64 sid, ChannelManager* channelManager);          // join - player 조회->id 발급->맵 등록 + 진입 시야 범위 spawn
            bool SpawnMonster(const MonsterSpawnInfo& info, int groupIdx);       // monster 스폰 - 풀 Allocate + 완전 초기화 + 등록/통지. 실패(false)면 호출자가 예약을 소비하지 않는다
            void InitSpawns(const SpawnGroup* groups, size_t count, const int* mapCaps);   // 부팅 - 스폰 그룹 등록 + 맵별 안전 천장 설정 + 그룹별 count 초기 채움

            void RemovePlayer(UINT64 sid, ChannelManager* channelManager);       // leave - 맵에서 player 제거 (통지는 호출자가 이미 함, 비타겟팅이라 타겟 해제 불필요)
            void DespawnMonster(GameObject* obj);                       // monster 디스폰 - 통지 + 셀 제거 + 풀 반납

            // 바닥 드랍 - 몬스터 사망/버리기가 호출. cap 초과면 그 맵의 가장 오래된 드랍부터 제거 (버리기 도배 방어).
            //   ownerCharIds = 데미지 기여 1~3순위 (버리기는 버린 사람 단독). 반환 = 생성된 GroundItem (실패 nullptr).
            GroundItem* SpawnGroundItem(int mapId, const Position& pos, int templateId, int quantity,
                                        const UINT32 ownerCharIds[3], UINT64 nowMs);
            void        DespawnGroundItem(GameObject* obj);             // 드랍 회수 - 통지(SC_DESPAWN) + 셀 제거 + 풀 반납
            GroundItem* FindGroundItem(int mapId, UINT32 objectId);     // 줍기 요청 키(objectId)로 그 맵의 드랍 조회 (없으면 nullptr)

            bool ChangeMap(GameObject* mover, int toMapId, const Position& dst);   // 포탈 - 옛 맵 정리 + 새 맵 등록 (도착 좌표=서버 권위). 무효 dst/동일맵이면 false(no-op)

            // forwarder - m_maps가 private이라 Channel이 직접 Map 못 만짐, mover의 맵에 위임
            void GetNearbyObjectsFor(const GameObject* origin, int rangePx, std::vector<GameObject*>& out);   // mover 맵의 GetNearbyObjects 위임 (범위 내 객체 수집)
            void DespawnVisibilityForOnLeave(GameObject* leaver);                          // leaver 맵의 DespawnVisibilityFor 위임 (leave 통지)

            // Channel::SendObjectUpdates이 맵별 바뀐 객체를 순회하도록 노출 (m_maps private 보존).
            void CollectMovedObjects(std::vector<GameObject*>& out);                        // 맵 전수에서 이번 tick 바뀐 객체를 out에 모음
            void ClearMovedObjects();                                                             // 맵별 바뀜 목록 비움 (SendObjectUpdates 송신 후)
            void CollectDiedPlayers(std::vector<UINT64>& out);                              // 맵 전수에서 이번 tick 죽은 플레이어 sid를 out에 모음 (거래 강제 종결용)

            void CollectOutbound(std::vector<OutboundPacket>& out);   // 각 맵 송신 대기 패킷을 out에 모음
            void ClearOutbound();                                 // 각 맵 송신 대기 패킷 비움 (송신 후)

            void CollectPlayersForSave(std::vector<Player*>& out);    // 모든 맵의 player만 모음 (DB 저장 대상)

            Player* FindPlayerByNameInMap(int mapId, const wchar_t* name, UINT64 excludeSid);   // 같은 맵에서 이름이 일치하는 다른 player 조회 (거래 상대 - 자기 자신 제외, 없으면 nullptr)

            int BroadcastToMapBatched(int mapId, int ownerChannelId, const BYTE* data, int size, std::vector<GameSession*>& flushList);   // 한 맵 모든 player에 배치 누적(역포인터 송신, 전역락 우회) - null + 채널 소유권 가드는 Channel::BatchTo 와 같은 판정, 반환=수신자 수

            // 모니터링: 맵별 현재 몬스터 수 (스폰/디스폰 시 증감하는 카운터, 채널 스레드 갱신, 메인 근사 read).
            int GetMonsterCount(int mapId) const;

            // 길찾기 결과 재주입 - mapId 맵을 순회해 wire id 가 일치하는 살아있는(!IsDead) 몬스터 반환 (없으면 nullptr).
            //   맵당 몬스터 <=30 이라 선형 순회 무해 (포인터 결과 금지 - id 재조회로 stale/ABA 봉인).
            Monster* FindMonsterForPath(int mapId, UINT32 monsterId);

        private:
            bool TrySpawnFromGroup(int groupIdx);   // 그룹 구역 랜덤 위치에 1마리 스폰 시도 - 맵 천장/풀 실패면 false(호출자가 예약을 소비하지 않음). 성공 시 그룹 alive 증가까지
            void SweepDead(UINT64 nowMs);   // 시신 유지 시간 지난 몬스터 회수 (Update 끝에서) - 풀 반납 + 그룹 회계(alive 감소 + 부활 예약 등록)
            void SweepExpiredGroundItems(UINT64 nowMs);   // 만료된 바닥 드랍 회수 (Update 끝에서) - 부활 예약과 같은 채널 시계 timestamp 판정
            void RespawnDueGroups(UINT64 nowMs);   // 부활 시각이 된 예약을 그룹 구역 랜덤 위치에 스폰 (보스 그룹 먼저 - cap 천장 발동 시 대비). 스폰 성공 시에만 예약 소비
            void DetachFromOldMap(GameObject* mover, int fromMapId, Player* mp);   // 포탈 - 옛 맵 격자/가시성에서 mover 제거 ((b) despawn 후 (a) 순서)

            int        m_channelId;   // 이 MapManager 소유 채널 id - 스폰 몬스터에 stamp (길찾기 요청/결과 회신 대상)
            KYS::GAMESERVER::MEMORY::ObjectPool<Monster> m_monsterPool;   // 몬스터 단일 풀 (capacity=DEFAULT_MONSTER_POOL_CAPACITY)
            KYS::GAMESERVER::MEMORY::ObjectPool<GroundItem> m_groundItemPool;   // 바닥 드랍 풀 (capacity=DEFAULT_GROUND_ITEM_POOL_CAPACITY)
            // 맵 배열들은 전부 컴파일타임 상한(MAX_MAP_COUNT) 크기 - 실제 쓰는 칸 수는 maps.csv 행 수(MapTableCount()).
            //   순회/범위 가드는 MapTableCount() 까지만 돈다 (미사용 슬롯을 돌면 빈 맵 스폰/통계 오염).
            Map        m_maps[MAX_MAP_COUNT];                             // 맵별 격자 (값 배열, Fixed Array)
            UINT32     m_nextObjectId;                                    // 통합 단조 증가 id 발급 (player+monster+ground item 공유 - 충돌 방지)
            int        m_groundItemCount[MAX_MAP_COUNT];                  // 맵별 바닥 드랍 수 (cap 판정 - 스폰 ++ / 디스폰 --)

            // 스폰 그룹 리스폰: 그룹(사각 구역 + 종류 + count)이 자기 마릿수를 per-몹 부활 예약으로 유지한다.
            //   사망->시신->회수(SweepDead) 시점에 그 몹의 그룹에 부활 예약(now + respawnDelayMs)이 걸리고,
            //   RespawnDueGroups가 시각이 된 예약을 구역 랜덤 위치에 스폰한다 (rAthena 그룹 스폰 + MapleStory 맵 천장).
            //   불변식: aliveCount + pendingCount == count (스폰 성공 시에만 예약 소비 - 실패/보류 시 잔류 재시도라 영구 결손 없음).
            struct SpawnGroupRuntime
            {
                SpawnGroup def;                          // 그룹 정의 (spawns.csv 행 복사 - 구역/종류/count/부활 지연)
                int        aliveCount;                   // 현재 이 그룹 소속으로 존재하는 수 (시신 grace 포함 - SpawnMonster ++ / SweepDead 회수 --)
                int        pendingCount;                 // 부활 대기 예약 수
                UINT64     pendingDue[MAX_GROUP_COUNT];  // 예약별 부활 시각 (채널 tick 시계 ms. 예약 수 <= count <= MAX_GROUP_COUNT 라 넘칠 수 없음)
            };
            SpawnGroupRuntime m_spawnGroups[MAX_SPAWN_GROUPS];   // 전 그룹 (로더 행 순서 - Monster::m_spawnGroupIdx 가 이 배열 인덱스)
            int    m_spawnGroupCount;              // 쓰는 그룹 수 (InitSpawns 가 설정)
            int    m_mapCap[MAX_MAP_COUNT];        // 맵별 몬스터 수 안전 천장 (maps.csv monsterCap - 로더 검증[count 합<=cap] 덕에 정상 데이터에선 발동하지 않는 이중 안전망)
            int    m_monsterCount[MAX_MAP_COUNT];  // 맵별 현재 몬스터 수 (시신 grace 포함 - SpawnMonster ++ / DespawnMonster --. 천장 판정 + 모니터 표출 공용)
        };

    }
}
