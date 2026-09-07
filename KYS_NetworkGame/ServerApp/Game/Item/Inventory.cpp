#include "pch_serverapp.h"
#include "Inventory.h"
#include "../../../GameCommon/ItemData/ItemTable.h"     // FindItemDef (스택 상한 / 장착 종류 판정)

namespace KYS
{
    namespace SERVERAPP
    {
        // 범위 밖 슬롯 조회가 돌려줄 빈 칸 (수정 금지 - 읽기 전용 반환 전용)
        static const ItemInstance kEmptySlot = { 0, 0, 0 };

        Inventory::Inventory()
        {
            ClearAll();
        }

        void Inventory::ClearAll()
        {
            for (int i = 0; i < MAX_INVENTORY_SLOTS; ++i) { m_bag[i].Clear(); }
            m_weapon.Clear();
            m_armor.Clear();
        }

        ItemInstance* Inventory::SlotRef(int slot)
        {
            if (slot >= 0 && slot < MAX_INVENTORY_SLOTS) { return &m_bag[slot]; }
            if (slot == EQUIP_SLOT_WEAPON) { return &m_weapon; }
            if (slot == EQUIP_SLOT_ARMOR) { return &m_armor; }
            return nullptr;
        }

        const ItemInstance* Inventory::SlotRef(int slot) const
        {
            if (slot >= 0 && slot < MAX_INVENTORY_SLOTS) { return &m_bag[slot]; }
            if (slot == EQUIP_SLOT_WEAPON) { return &m_weapon; }
            if (slot == EQUIP_SLOT_ARMOR) { return &m_armor; }
            return nullptr;
        }

        int Inventory::FindEmptyBagSlot() const
        {
            for (int i = 0; i < MAX_INVENTORY_SLOTS; ++i)
            {
                if (m_bag[i].IsEmpty()) { return i; }
            }
            return -1;
        }

        const ItemInstance& Inventory::At(int slot) const
        {
            const ItemInstance* ref = SlotRef(slot);
            return (ref != nullptr) ? *ref : kEmptySlot;
        }

        bool Inventory::HasRoomFor(int templateId, int quantity) const
        {
            const ItemDef* def = FindItemDef(templateId);
            if (def == nullptr || quantity <= 0) { return false; }

            // 같은 종류 스택들의 남은 여유부터 센다
            int remaining = quantity;
            for (int i = 0; i < MAX_INVENTORY_SLOTS && remaining > 0; ++i)
            {
                if (!m_bag[i].IsEmpty() && m_bag[i].templateId == templateId)
                {
                    const int spare = def->stackMax - m_bag[i].quantity;
                    if (spare > 0) { remaining -= spare; }
                }
            }
            if (remaining <= 0) { return true; }

            // 남은 양은 빈 칸 하나로 받는다 (획득/거래 단위가 스택 상한 이하라 새 칸은 최대 1개)
            if (remaining > def->stackMax) { return false; }
            return FindEmptyBagSlot() >= 0;
        }

        bool Inventory::Add(UINT64 uid, int templateId, int quantity)
        {
            if (!HasRoomFor(templateId, quantity)) { return false; }   // 사전검증 통과 못 하면 아무것도 안 바꿈
            const ItemDef* def = FindItemDef(templateId);
            if (def == nullptr) { return false; }

            // (1) 같은 종류 스택에 붓기
            int remaining = quantity;
            for (int i = 0; i < MAX_INVENTORY_SLOTS && remaining > 0; ++i)
            {
                if (!m_bag[i].IsEmpty() && m_bag[i].templateId == templateId)
                {
                    const int spare = def->stackMax - m_bag[i].quantity;
                    if (spare > 0)
                    {
                        const int pour = (remaining < spare) ? remaining : spare;
                        m_bag[i].quantity += pour;
                        remaining -= pour;
                    }
                }
            }
            if (remaining <= 0) { return true; }   // 전량 합류 - 넘겨받은 uid 는 안 씀

            // (2) 남은 양은 빈 칸에 새 실물로
            const int emptySlot = FindEmptyBagSlot();
            if (emptySlot < 0) { return false; }   // HasRoomFor 통과라 도달 불가 - 방어적 가드
            m_bag[emptySlot].uid = uid;
            m_bag[emptySlot].templateId = templateId;
            m_bag[emptySlot].quantity = remaining;
            return true;
        }

        bool Inventory::RemoveAt(int slot, int quantity)
        {
            ItemInstance* ref = SlotRef(slot);
            if (ref == nullptr || ref->IsEmpty() || quantity <= 0) { return false; }
            if (ref->quantity < quantity) { return false; }   // 있는 것보다 많이 못 뺌

            ref->quantity -= quantity;
            if (ref->quantity <= 0) { ref->Clear(); }
            return true;
        }

        bool Inventory::Move(int fromSlot, int toSlot)
        {
            // 가방 안 이동만 허용 (장착 칸은 Equip/Unequip 전용 경로)
            if (fromSlot < 0 || fromSlot >= MAX_INVENTORY_SLOTS) { return false; }
            if (toSlot < 0 || toSlot >= MAX_INVENTORY_SLOTS) { return false; }
            if (fromSlot == toSlot) { return false; }

            ItemInstance& from = m_bag[fromSlot];
            ItemInstance& to = m_bag[toSlot];
            if (from.IsEmpty()) { return false; }

            // 같은 종류끼리는 스택 합류 (상한까지 붓고 남으면 원래 칸에 남김)
            if (!to.IsEmpty() && to.templateId == from.templateId)
            {
                const ItemDef* def = FindItemDef(from.templateId);
                if (def == nullptr) { return false; }
                const int spare = def->stackMax - to.quantity;
                if (spare <= 0) { return false; }   // 받는 쪽이 이미 가득
                const int pour = (from.quantity < spare) ? from.quantity : spare;
                to.quantity += pour;
                from.quantity -= pour;
                if (from.quantity <= 0) { from.Clear(); }
                return true;
            }

            // 다른 종류(또는 빈 칸)와는 자리 맞바꿈
            const ItemInstance temp = to;
            to = from;
            from = temp;
            return true;
        }

        bool Inventory::Equip(int bagSlot)
        {
            if (bagSlot < 0 || bagSlot >= MAX_INVENTORY_SLOTS) { return false; }
            ItemInstance& bag = m_bag[bagSlot];
            if (bag.IsEmpty()) { return false; }

            const ItemDef* def = FindItemDef(bag.templateId);
            if (def == nullptr) { return false; }

            // 종류에 맞는 장착 칸 결정 (장비가 아니면 거부)
            ItemInstance* equipRef = nullptr;
            if (def->itemType == EItemType::EQUIP_WEAPON) { equipRef = &m_weapon; }
            else if (def->itemType == EItemType::EQUIP_ARMOR) { equipRef = &m_armor; }
            else { return false; }

            // 기존 장착품과 자리 맞바꿈 (비어 있으면 그냥 이동)
            const ItemInstance temp = *equipRef;
            *equipRef = bag;
            bag = temp;
            return true;
        }

        int Inventory::Unequip(int equipSlot)
        {
            ItemInstance* equipRef = nullptr;
            if (equipSlot == EQUIP_SLOT_WEAPON) { equipRef = &m_weapon; }
            else if (equipSlot == EQUIP_SLOT_ARMOR) { equipRef = &m_armor; }
            else { return -1; }
            if (equipRef->IsEmpty()) { return -1; }

            const int emptySlot = FindEmptyBagSlot();
            if (emptySlot < 0) { return -1; }   // 가방이 가득이면 해제 불가

            m_bag[emptySlot] = *equipRef;
            equipRef->Clear();
            return emptySlot;   // 클라 통지용 - 어느 가방 칸으로 들어갔는지
        }

        int Inventory::TotalAtkPower() const
        {
            if (m_weapon.IsEmpty()) { return 0; }
            const ItemDef* def = FindItemDef(m_weapon.templateId);
            return (def != nullptr) ? def->atkPower : 0;
        }

        int Inventory::TotalDefPower() const
        {
            if (m_armor.IsEmpty()) { return 0; }
            const ItemDef* def = FindItemDef(m_armor.templateId);
            return (def != nullptr) ? def->defPower : 0;
        }

        void Inventory::SetAt(int slot, UINT64 uid, int templateId, int quantity)
        {
            ItemInstance* ref = SlotRef(slot);
            if (ref == nullptr) { return; }   // 규약 밖 슬롯 번호는 무시 (DB 오염 행 방어)
            ref->uid = uid;
            ref->templateId = templateId;
            ref->quantity = quantity;
        }
    }
}
