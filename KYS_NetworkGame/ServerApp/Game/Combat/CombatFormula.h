#pragma once
#include "../GameObject.h"   // GameObject* caster/target (전방 선언으로도 충분 - 포인터 인자. 실제 include 사용)

namespace KYS
{
    namespace SERVERAPP
    {
        // 데미지 수식 거점 - 순수 static 유틸(인스턴스 0).
        //   player(Channel::OnSkill), monster(Monster::Update ATTACK) 두 호출처가 공통 base 인자로 1벌 공유.
        //   난수(치명타/명중 판정) 상태는 .cpp 파일 범위 thread_local (1 Channel=1 Thread, 락 0). 서버 권위(GameCommon 공유 0).
        class CombatFormula
        {
        public:
            // 공격자/대상의 base 스탯(현재 hp/maxHp 등) + 난수로 실데미지 산출. 적용(차감)은 GameObject::TakeDamage 책임.
            static int Compute(GameObject* caster, GameObject* target);

            // 균등 정수 [lo, hi] (양끝 포함). 내부 xorshift32 thread_local 재사용(동시 접근 충돌 없음) - std::rand 금지.
            //   드랍 추첨/수량/좌표 분산, 몬스터 배회 좌표, 그룹 스폰 좌표가 공유.
            static int RandomRange(int lo, int hi);

        private:
            CombatFormula() = delete;   // 인스턴스화 금지 (static 전용 유틸 - namespace 대안 대비 응집)
        };
    }
}
