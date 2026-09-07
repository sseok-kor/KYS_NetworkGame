#pragma once
#include "Cell.h"
#include "../../../GameServer/Types/Defines.h"
#include "../../../GameCommon/GameDefines.h"     // GRID_ROWS/COLS, CELL_SIZE, VIEW_RANGE
#include "../../../GameCommon/CommonStructs.h"    // Position (전역 정의)
#include <vector>

namespace KYS { namespace GAMECOMMON { namespace PROTOCOL { class CPacket; } } }   // StageToObservers 참조 인자 (완성 패킷)

namespace KYS
{
    namespace SERVERAPP
    {

        class GameObject;

        static const int OUTBOUND_PACKET_MAX = 64;   // OutboundPacket 고정 버퍼 크기(바이트)

        // 송신 대기 패킷 한 건 - 수신자 sid + 직렬화된 바이트(고정 버퍼라 힙 할당 0).
        struct OutboundPacket
        {
            UINT64            sid;     // 수신자 sid (Channel이 GetSession(sid)->Send 로 보냄)
            BYTE   data[OUTBOUND_PACKET_MAX];   // 직렬화 바이트(프레이밍 포함) - 고정 버퍼, 힙 할당 0
            int    size;                   // 실제 바이트 수 (data 앞 size 바이트만 유효)
        };

        // 한 맵 = 격자(grid, 셀 배열) + 그 맵의 전체 객체 목록. tick마다 객체 갱신/셀 재배치/가시성/근처 객체 수집 담당.
        class Map
        {
        public:
            Map() = default;
            ~Map() = default;

            Map(const Map&) = delete;
            Map& operator=(const Map&) = delete;
            Map(Map&&) = delete;
            Map& operator=(Map&&) = delete;

            void Update(float deltaTime);   // 한 tick: 객체 갱신(위치 예측)->셀 재배치->가시성->바뀐 객체 표시 (송신 안 함)

            void Insert(GameObject* obj);                                                     // 객체를 맵 목록 + 해당 셀에 등록
            void Erase(GameObject* obj);                                                      // 객체를 맵 목록 + 셀에서 제거

            // origin 위치에서 rangePx(px) 안의 객체를 out에 모은다 (셀 광역 수집 후 정사각 범위로 정확히 자름).
            void GetNearbyObjects(const GameObject* origin, int rangePx, std::vector<GameObject*>& out) const;
            // 중앙 셀 좌표에서 cellRange 칸 반경의 셀 객체를 out에 모은다 (좌표 미보유 -> 범위로 안 자름, 옛 위치 수집용).
            void GetObjectsAt(int centerRow, int centerCol, int cellRange, std::vector<GameObject*>& out) const;

            // 몬스터 교전 대상 산출(비타겟팅): 신규 획득은 aggroRange 안 최근접 player, 점착(stickyId·직전 대상) 유지는
            //   maintainRange 안까지(우회 추격 중 거리 확장을 견딤 - 신규 획득 반경보다 넓게). 없으면 nullptr.
            //   반환 포인터는 현재 grid의 live 객체 - 호출자(Monster::Update)가 같은 tick에만 쓰고 저장 안 함(저장 raw 포인터 0 = cross-tick dangling 불가).
            GameObject* ResolveTarget(const GameObject* origin, int aggroRange, int maintainRange, UINT32 stickyId);

            void SpawnVisibilityFor(GameObject* enter);     // 진입 객체와 시야 범위 안 객체 사이 양방향 spawn 통지
            void DespawnVisibilityFor(GameObject* leaver);  // 떠나는 객체를 보던 관찰자들에게 despawn 통지
            void NotifyDespawnTo(GameObject* to, GameObject* who);   // to 에게 who 의 despawn 한 건 통지 (맵 이동용)

            const std::vector<GameObject*>& GetObjects() const;                              // 맵 전체 객체 목록 (전수 순회용)
            const std::vector<GameObject*>& GetMovedObjects() const;                         // 이번 tick 위치가 바뀐 객체 목록 (broadcast 대상)
            void ClearMovedObjects();                                                               // 바뀐 객체 목록 비움 (broadcast 송신 후)
            const std::vector<UINT64>& GetDiedPlayers() const;                               // 이번 tick 몬스터에게 죽은 플레이어 sid 목록 (거래 강제 종결 대상)
            const std::vector<OutboundPacket>& GetOutboundPackets() const;   // 송신 대기 패킷 목록 (Channel/MapManager가 drain)
            void ClearOutboundPackets();                                     // 송신 대기 패킷 비움 (송신 후)



        private:
            void RelocateInGrid();                       // 전수 순회하며 셀이 바뀐 객체만 격자에서 옮김
            void UpdateVisibility(GameObject* mover);     // 셀이 바뀐 객체의 옛/새 시야 범위를 비교해 spawn/despawn 통지
            void SpawnNewlyVisible(GameObject* mover, bool moverIsPlayer, const std::vector<GameObject*>& newNearby);     // 새 시야에 들어온 객체들과 mover 사이 양방향 spawn 통지
            void DespawnNoLongerVisible(GameObject* mover, bool moverIsPlayer, const std::vector<GameObject*>& oldNearby, const std::vector<GameObject*>& newNearby);   // 시야에서 빠진 객체들과 mover 사이 양방향 despawn 통지
            bool NotifySpawn(GameObject* to, GameObject* who);     // to 에게 who 의 spawn 패킷을 적재. 시체도 적재(시야 대칭 - 죽은 객체 skip 이 시야집합 비대칭/dangling 의 근원이었음) - 항상 true 반환(호출자가 AddVisible)
            void NotifyDespawn(GameObject* to, GameObject* who);   // to 에게 who 의 despawn 패킷을 만들어 송신 대기에 적재

            static int ColOf(int x);   // x 좌표 -> 셀 열 인덱스 (맵 끝 좌표 4000 보호 위해 GRID_COLS-1 클램프)
            static int RowOf(int y);    // y 좌표 -> 셀 행 인덱스 (GRID_ROWS-1 클램프)

            void StageDamage(GameObject* attacker, GameObject* victim, int damage);   // 피해 이벤트를 victim 주변 관찰자에게 송신 대기 적재
            void StageDeath(GameObject* victim);     // 사망 이벤트를 victim 주변 관찰자에게 송신 대기 적재
            void StageRespawn(GameObject* player);   // 부활 이벤트를 부활 지점 주변 관찰자에게 송신 대기 적재
            // 완성 패킷(pkt)을 anchor 시야 범위 안 player 관찰자 전원에게 송신 대기 적재 (Stage* 3종 공통 - 관찰자 수집 + 고정버퍼 memcpy 루프)
            //   pkt 는 비-const 참조 - CPacket::GetBuffer 가 비-const(가변 BYTE*)라 const 로는 못 읽는다.
            void StageToObservers(GameObject* anchor, KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);

            // 완성 패킷을 송신 대기 버퍼에 한 건 적재한다. 세 적재 자리(NotifySpawn/NotifyDespawn/StageToObservers)의
            //   공통 골격 - 고정 버퍼 경계 검사를 한 곳으로 모아, 앞으로 적재 자리가 늘어도 자동으로 지켜진다.
            //   크기가 캡을 넘으면 복사하지 않고 false (거부) - 넘친 바이트가 data 바로 뒤의 size 필드를 덮어
            //   그 오염된 길이가 하류 AppendToBatch 의 memcpy 길이가 되는 2차 오염을 원천에서 끊는다.
            //   반환 false = 이 수신자는 이 패킷을 못 받는다(그 객체가 안 보인다).
            bool StageOne(UINT64 toSid, KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);

            Cell m_grid[GRID_ROWS][GRID_COLS];        // 공간 격자 (근처 객체 수집/셀 재배치용)
            std::vector<GameObject*> m_objects;             // 이 맵의 전체 객체 (전수 순회용)
            std::vector<GameObject*> m_movedObjects;        // 이번 tick 위치가 바뀐 객체 (broadcast 수집용)
            std::vector<OutboundPacket>   m_outboundPackets;   // spawn/despawn 등 송신 대기 버퍼
            std::vector<GameObject*>      m_targetCandidates;      // 몬스터 교전 대상 스캔 재사용 버퍼(ResolveTarget) - 채널 1스레드라 단일 점유, 매 호출 clear 후 채움
            std::vector<UINT64>           m_diedPlayers;           // 이번 tick 몬스터에게 죽은 플레이어 sid (Update가 채움, 다음 Update 시작 시 clear) - Channel이 거래 강제 종결에 소비
        };

    }
}
