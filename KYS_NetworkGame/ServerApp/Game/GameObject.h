#pragma once
#include "../GameCommon/GameTypes.h"
#include "../GameCommon/CommonStructs.h"
#include "../GameCommon/Protocol/CPacket.h"
// UINT64는 Defines.h의 전역 타입

namespace KYS
{
    namespace SERVERAPP
    {
        // 모든 게임 객체(Player/Monster)의 공통 base.
        // 위치/맵 소속/그리드 셀/hp/이동 상태를 들고, tick 갱신과 패킷 직렬화는 파생이 채운다(순수가상).
        class GameObject
        {
        public:
            // 생성 / 소멸
            explicit GameObject(UINT64 id, EObjectType type = EObjectType::PLAYER);   // id(=sid)를 받아 m_id 즉시 확정
            virtual ~GameObject();

            GameObject(const GameObject&) = delete;
            GameObject& operator=(const GameObject&) = delete;
            GameObject(GameObject&&) = delete;
            GameObject& operator=(GameObject&&) = delete;

            // 다형 - 파생(Player/Monster)이 구현
            virtual void Update(float deltaTime) = 0;                                              // 기대 역할: 매 tick 자기 상태 갱신
            virtual bool SerializeBroadcast(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;        // 주변에 알릴 변경분(위치 등)을 패킷에 담음 - 기본 no-op(미이동 객체용), Player/Monster override
            virtual bool SerializeSpawn(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const = 0;        // 기대 역할: 시야에 처음 들어온 상대에게 보낼 전체 상태를 패킷에 담음
            virtual int TakeDamage(int);                                                          // 피해 적용 -> 실제 깎인 hp 반환 (base 기본 구현, Monster가 덮을 수 있음)

            // 접근자 - 식별 / 위치 / 변경 표시
            UINT64          GetId() const;          // 식별자(=sid)
            const Position& GetPos() const;
            void            SetPos(const Position& p);
            bool            IsDirty() const;
            void            SetDirty(bool d);

            // 접근자 - 맵 소속
            int  GetMapId() const;
            void SetMapId(int id);

            // 접근자 - 직전 그리드 셀 (이동 시 셀 재배치 비교용)
            int  GetCellRow() const;
            int  GetCellCol() const;
            void SetCell(int row, int col);

            EObjectType GetObjectType() const;        // 타입 판별 (가상함수가 아니라 vtable 안 거침)

            // 접근자 - 이동 상태
            void           SetMoveState(EMoveState s, EMoveDirection direction);
            EMoveState     GetMoveState() const;
            EMoveDirection GetDirection() const;

            // 접근자 - 전투(hp)
            int GetHp() const;
            void SetHp(int hp);
            int GetMaxHp() const;
            void SetMaxHp(int hp);
            bool IsDead() const;                       // hp <= 0 이면 사망

        private:
            UINT64   m_id;       // 식별자 (=sid), 조회 키로 쓰임
            Position m_pos;      // 현재 위치 {int x, int y}
            bool     m_dirty;    // 이 tick에 위치가 바뀜 표시 (broadcast 대상 마킹)
            int      m_mapId;    // 현재 속한 맵 (초기 0 = 시작 맵)
            int      m_cellRow;  // 직전에 있던 그리드 셀 row (초기 -1 = 아직 미배치)
            int      m_cellCol;  // 직전에 있던 그리드 셀 col (초기 -1)
            EObjectType m_objectType;  // 타입 판별자 (ctor에서 한 번 set 후 불변, 송신 수신자 필터/캐스팅용)

            // 이동 상태 (Player/Monster 공통)
            EMoveState     m_moveState; // 지금 움직임 상태 (STOP / 이동 중)
            EMoveDirection m_direction; // 바라보는 8방향 (위치 예측용 방향)

            // 전투 (hp)
            int m_hp;
            int m_maxHp;
        };
    }
}
