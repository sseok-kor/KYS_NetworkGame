#pragma once
#include "../../../GameCommon/GameTypes.h"   // EMonsterType

namespace KYS
{
    namespace SERVERAPP
    {
        // 드랍 규칙 한 행 - "이 종류 몬스터는 이 아이템을 천분율 확률로 min~max 개 떨어뜨린다".
        //   한 몬스터 종류에 행 여러 개 허용 - 행마다 독립 추첨이라 동시에 여러 종류가 떨어질 수 있다 (rAthena 방식).
        //   출처 파일: Data/monsters.yml 의 몬스터 블록 안 drops 목록 (기존 별도 drops.csv 를 흡수 - 서버 전용, 클라 비배포).
        struct DropRule
        {
            EMonsterType monsterType;
            int          templateId;     // Data/items.csv 참조 (적재 시 FindItemDef 로 검증)
            int          chancePermil;   // 천분율 (1000 = 100%) - 부동소수점 없이 결정적 추첨
            int          minQty;
            int          maxQty;
        };

        // 드랍 규칙 저장소 초기화/채움 - monsters.yml 로더(LoadMonsterTable)가 부팅 시 호출한다.
        //   행 검증(참조 무결성/스택 한도/확률/수량)은 로더가 적재 시점에 끝내고, 여기는 검증된 행의 보관만 맡는다.
        void            ClearDropRules();
        bool            AddDropRule(const DropRule& rule);   // 반환 false = 저장소 가득 (호출자가 부팅 거부)

        // 적재된 규칙 수. index 는 0..count-1 (= 적재 순서). 호출자가 monsterType 으로 걸러 쓴다.
        int             DropTableCount();
        const DropRule& DropRuleAt(int index);
    }
}
