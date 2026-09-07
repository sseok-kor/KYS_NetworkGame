#include "pch_serverapp.h"
#include "GroundItem.h"
#include "../../../GameCommon/Protocol/GamePackets.h"   // SC_GROUND_ITEM_SPAWN
#include "../../../GameCommon/Protocol/PacketType.h"
#include "../../../GameCommon/GameDefines.h"     // GROUND_ITEM_* 상수

namespace KYS
{
    namespace SERVERAPP
    {
        GroundItem::GroundItem(UINT64 id)
            : GameObject(id, EObjectType::GROUND_ITEM)
            , m_templateId(0)
            , m_quantity(0)
            , m_spawnTimeMs(0)
        {
            m_ownerCharIds[0] = 0;
            m_ownerCharIds[1] = 0;
            m_ownerCharIds[2] = 0;
        }

        GroundItem::~GroundItem()
        {
            // 다형 정리는 GameObject::~GameObject 가 담당. GroundItem 고유 자원 없음.
        }

        // 바닥 아이템은 스스로 하는 일이 없다 - 만료 판정은 MapManager 의 스윕이 벽시계로 한다
        //   (per-object 타이머 대신 몬스터 리스폰과 같은 timestamp 비교 방식).
        void GroundItem::Update(float)
        {
        }

        void GroundItem::InitDrop(int templateId, int quantity, UINT64 spawnTimeMs, const UINT32 ownerCharIds[3])
        {
            m_templateId = templateId;
            m_quantity = quantity;
            m_spawnTimeMs = spawnTimeMs;
            for (int i = 0; i < 3; ++i)
            {
                m_ownerCharIds[i] = (ownerCharIds != nullptr) ? ownerCharIds[i] : 0;
            }
        }

        bool GroundItem::CanBePickedBy(UINT32 charId, UINT64 nowMs) const
        {
            if (m_ownerCharIds[0] == 0) { return true; }   // 기여자 없음 - 처음부터 자유 루팅

            const UINT64 elapsed = nowMs - m_spawnTimeMs;
            if (elapsed >= static_cast<UINT64>(GROUND_ITEM_OWNER3_MS)) { return true; }   // 보호 종료 - 자유

            // 경과 시간에 따라 허용 순위가 계단식으로 넓어진다 (3초: 1순위 / 5초: 2순위 / 7초: 3순위).
            int allowedRanks = 1;
            if (elapsed >= static_cast<UINT64>(GROUND_ITEM_OWNER1_MS)) { allowedRanks = 2; }
            if (elapsed >= static_cast<UINT64>(GROUND_ITEM_OWNER2_MS)) { allowedRanks = 3; }

            for (int i = 0; i < allowedRanks; ++i)
            {
                if (m_ownerCharIds[i] != 0 && m_ownerCharIds[i] == charId) { return true; }
            }
            return false;
        }

        bool GroundItem::IsExpired(UINT64 nowMs) const
        {
            return (nowMs - m_spawnTimeMs) >= static_cast<UINT64>(GROUND_ITEM_EXPIRE_MS);
        }

        // 시야 진입자에게 보낼 등장 패킷. payload 20B + 헤더 4B = 24B (맵 스테이징 64B 캡 안).
        bool GroundItem::SerializeSpawn(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
        {
            const Position pos = GetPos();
            SC_GROUND_ITEM_SPAWN body;
            body.objectId = static_cast<UINT32>(GetId());
            body.templateId = m_templateId;
            body.x = pos.x;
            body.y = pos.y;
            body.quantity = m_quantity;
            pkt.Begin(static_cast<USHORT>(PacketType::SC_GROUND_ITEM_SPAWN));
            body.Serialize(pkt);
            return pkt.End();
        }
    }
}
