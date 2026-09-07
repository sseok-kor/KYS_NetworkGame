#include "pch_serverapp.h"
#include "DropTable.h"

namespace KYS
{
    namespace SERVERAPP
    {
        // 드랍 표는 부팅에 한 번 채운 뒤 읽기 전용 - 고정 배열.
        //   파일 파싱/검증은 monsters.yml 로더(MonsterTable.cpp)가 맡고, 여기는 검증된 행의 보관소만 남는다
        //   (기존 drops.csv 전용 로더는 monsters.yml 흡수로 폐기).
        static const int MAX_DROP_RULES = 256;
        static DropRule g_dropRules[MAX_DROP_RULES];
        static int      g_dropRuleCount = 0;

        void ClearDropRules()
        {
            g_dropRuleCount = 0;
        }

        bool AddDropRule(const DropRule& rule)
        {
            if (g_dropRuleCount >= MAX_DROP_RULES) { return false; }   // 저장소 가득 - 호출자가 부팅 거부
            g_dropRules[g_dropRuleCount] = rule;
            ++g_dropRuleCount;
            return true;
        }

        int DropTableCount()
        {
            return g_dropRuleCount;
        }

        const DropRule& DropRuleAt(int index)
        {
            return g_dropRules[index];
        }
    }
}
