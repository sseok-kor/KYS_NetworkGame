#include "pch_serverapp.h"
#include "GameObject.h"


namespace KYS
{
    namespace SERVERAPP
    {
        // 빈 게임 객체 초기화 - id와 타입만 받고 나머지는 기본값.
        //   id   : 이 객체의 식별자 (=sid)
        //   type : 객체 종류 (Player/Monster), 이후 불변
        GameObject::GameObject(UINT64 id, EObjectType type)
            : m_id(id)
            , m_pos{ 0, 0 }
            , m_dirty(false)
            , m_mapId(0)
            , m_cellRow(-1)
            , m_cellCol(-1)
            , m_objectType(type)               // 한 번만 set (이후 불변)
            , m_moveState(EMoveState::STOP)     // 진입 시 정지
            , m_direction(EMoveDirection::UP)   // 초기 방향 (STOP이라 위치 예측 안 함)
            , m_hp(0)
            , m_maxHp(0)
        {
        }

        // 가상 소멸자 - base 포인터로 delete 해도 파생 소멸자까지 호출되도록.
        GameObject::~GameObject()
        {
        }

        // 받은 피해를 hp에서 깎고, 실제로 깎인 양을 돌려준다.
        //   damage : 들어온 피해량 (음수면 0으로 막음)
        int GameObject::TakeDamage(int damage)
        {
            if (m_hp <= 0)        // 이미 죽었으면 추가타 무시 (시신이 잠깐 남는 동안 중복 적용 차단)
                return 0;
            if (damage < 0)       // 음수 가드 (회복은 별도 API)
                damage = 0;

            int applied = (damage < m_hp) ? damage : m_hp;   // 남은 hp보다 큰 피해는 남은 hp만큼만 (초과분 버림)
            m_hp -= applied;                                 // applied 만큼만 깎음 -> 0에서 멈춤 (음수 안 됨)
            // dirty 표시 안 함: hp 변경은 위치 변경이 아니라 이동 broadcast 사유가 아님.
            //   hp 값은 피격 이벤트 패킷과 시야 진입 시 spawn 스냅샷으로 따로 보냄.
            return applied;                                  // 실제 깎인 양 -> 드랍 기여 랭킹용 (표시용 데미지는 호출자가 원시 판정치를 따로 보냄)
        }

        // 주변에 알릴 변경분을 패킷에 담는다 - 기본은 no-op(바닥 아이템처럼 이동/변경이 없어 broadcast 대상이 아닌 객체용).
        //   Player/Monster 만 override 해 실제 이동 상태를 기록한다. 미이동 객체는 바뀜 표시(SetDirty)를 안 켜서 실제 호출되지 않는다.
        bool GameObject::SerializeBroadcast(KYS::GAMECOMMON::PROTOCOL::CPacket& /*pkt*/) const
        {   return true;   }   // 쓴 것이 없으니 실패도 없다 (파생이 Begin/End 를 하면 그 End 결과를 돌려준다)

        UINT64 GameObject::GetId() const
        {
            return m_id;
        }

        const Position& GameObject::GetPos() const
        {
            return m_pos;
        }

        void GameObject::SetPos(const Position& p)
        {
            m_pos = p;
        }

        bool GameObject::IsDirty() const
        {
            return m_dirty;
        }

        void GameObject::SetDirty(bool d)
        {
            m_dirty = d;
        }

        int  GameObject::GetMapId() const
        {
            return m_mapId;
        }

        void GameObject::SetMapId(int id)
        {
            m_mapId = id;
        }

        int  GameObject::GetCellRow() const
        {
            return m_cellRow;
        }

        int  GameObject::GetCellCol() const
        {
            return m_cellCol;
        }

        void GameObject::SetCell(int row, int col)
        {
            m_cellRow = row; m_cellCol = col;
        }

        EObjectType GameObject::GetObjectType() const
        {
            return m_objectType;
        }

        void GameObject::SetMoveState(EMoveState s, EMoveDirection direction)
        {
            m_moveState = s;
            m_direction = direction;
        }

        EMoveState GameObject::GetMoveState() const
        {
            return m_moveState;
        }

        EMoveDirection GameObject::GetDirection() const
        {
            return m_direction;
        }

        int GameObject::GetHp() const
        {
            return m_hp;
        }

        void GameObject::SetHp(int hp)
        {
            m_hp = hp;
        }

        int GameObject::GetMaxHp() const
        {
            return m_maxHp;
        }

        void GameObject::SetMaxHp(int maxHp)
        {
            m_maxHp = maxHp;
        }

        bool GameObject::IsDead() const
        {
            return m_hp <= 0;   // hp 0 이하면 사망 (시신이 잠깐 남는 처리는 전투 로직 쪽)
        }
    }
}
