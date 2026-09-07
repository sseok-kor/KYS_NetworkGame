#pragma once
#include "Item.h"
#include "../../../GameCommon/GameDefines.h"   // MAX_INVENTORY_SLOTS / EQUIP_SLOT_WEAPON / EQUIP_SLOT_ARMOR

namespace KYS
{
    namespace SERVERAPP
    {
        // 플레이어 한 명의 소지품 - 가방 고정 24칸 + 장착 2칸(무기/방어구).
        //   슬롯 번호 규약: 0..MAX_INVENTORY_SLOTS-1 = 가방, EQUIP_SLOT_WEAPON(100) = 무기, EQUIP_SLOT_ARMOR(101) = 방어구.
        //   채널 스레드 전용이라 락이 없다 (Zone Owner - 한 플레이어는 한 스레드만 만짐).
        //   DB 저장은 전량 스냅샷 교체 방식이라 여기엔 저장 코드가 없다 (Channel 이 스냅샷을 떠서 커맨드로 넘김).
        class Inventory
        {
        public:
            Inventory();

            // 획득 - 같은 종류 스택에 합류 우선, 남으면 빈 칸 하나 사용.
            //   전부 못 들어가면 아무것도 안 넣고 false (부분 획득 없음 - 검증과 적용을 가르는 사전검증 원칙).
            //   uid 는 새 칸이 필요할 때만 쓰인다 (전량 스택 합류면 미사용 - 발급 uid 하나 버려져도 무해).
            bool Add(UINT64 uid, int templateId, int quantity);

            // 슬롯 내용물 감소 - quantity 만큼 줄이고 0 이 되면 칸을 비움. 실패 = false (빈 칸/수량 부족/범위 밖)
            bool RemoveAt(int slot, int quantity);

            // 가방 칸 이동 - 같은 종류면 스택 합류(상한까지 붓기), 아니면 두 칸 스왑
            bool Move(int fromSlot, int toSlot);

            // 장착 - 가방 칸의 장비를 종류에 맞는 장착 칸으로 (기존 장착품이 있으면 자리 맞바꿈)
            bool Equip(int bagSlot);

            // 장착 해제 - 장착 칸의 장비를 가방 빈 칸으로. 반환 = 받은 가방 칸 번호 (-1 = 실패: 가방 꽉 참/빈 장착 칸)
            int Unequip(int equipSlot);

            // 조회
            const ItemInstance& At(int slot) const;               // 규약 슬롯 번호로 조회 (범위 밖 = 빈 칸 참조 반환)
            bool HasRoomFor(int templateId, int quantity) const;  // 획득/거래 수령 사전검증 (스택 합류 여유 감안)
            int  TotalAtkPower() const;                           // 장착 무기 공격력 (전투 계산이 읽음)
            int  TotalDefPower() const;                           // 장착 방어구 방어력 (받는 피해 감산)

            // 직접 세팅 - DB 로드 / 거래 커밋 전용. 게임 규칙 검증 없이 칸을 그대로 쓴다 (호출측이 검증 완료 전제).
            void SetAt(int slot, UINT64 uid, int templateId, int quantity);
            void ClearAll();

        private:
            ItemInstance*       SlotRef(int slot);         // 규약 슬롯 번호 -> 실제 칸 (범위 밖 = nullptr)
            const ItemInstance* SlotRef(int slot) const;
            int FindEmptyBagSlot() const;                  // 빈 가방 칸 번호 (-1 = 없음)

            ItemInstance m_bag[MAX_INVENTORY_SLOTS];   // 가방 (슬롯 0..23)
            ItemInstance m_weapon;                     // 장착 무기 (슬롯 100)
            ItemInstance m_armor;                      // 장착 방어구 (슬롯 101)
        };
    }
}
