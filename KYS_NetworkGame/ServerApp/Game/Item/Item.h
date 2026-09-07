#pragma once
#include "../../../GameServer/Types/Defines.h"   // UINT64

namespace KYS
{
    namespace SERVERAPP
    {
        // 인벤토리 한 칸에 실제로 들어있는 아이템 실물.
        //   종류 정의(이름/능력치/스택 상한)는 GameCommon ItemTable 의 ItemDef 가 담당하고,
        //   여기는 "누가 어떤 실물을 몇 개 갖고 있나"만 담는다 (템플릿/인스턴스 분리).
        struct ItemInstance
        {
            UINT64 uid;          // 영속 고유 번호 (0 = 빈 칸). DB item_instance PK 와 동일 값
            int    templateId;   // 아이템 종류 번호 (Data/items.csv 참조)
            int    quantity;     // 수량 (장비 = 1)

            bool IsEmpty() const { return uid == 0; }
            void Clear() { uid = 0; templateId = 0; quantity = 0; }
        };

        // 아이템 고유 번호 발급기 - 채널 스레드 여럿이 동시에 획득을 처리해도 안전(Interlocked).
        //   부팅 시 DB 의 MAX(item_uid) 로 시드한 뒤 사용한다 (DBThread 접속 직후 1회).
        //   DB AUTO_INCREMENT 를 안 쓰는 이유: 비동기 DBThread 의 INSERT 완료를 기다리지 않고
        //   획득 순간 uid 를 확정해서 값 스냅샷으로 넘겨야 하기 때문 (인메모리 권위 유지).
        void   SeedItemUid(UINT64 maxUidInDb);   // 부팅 1회 - 이후 발급은 maxUidInDb+1 부터
        UINT64 IssueItemUid();                   // 새 아이템 실물 생성 시 호출 (스레드 안전)
    }
}
