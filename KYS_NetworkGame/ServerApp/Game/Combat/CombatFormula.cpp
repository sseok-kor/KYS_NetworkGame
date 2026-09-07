#include "pch_serverapp.h"
#include "CombatFormula.h"
#include "../GameObject.h"          // GameObject::GetObjectType / EObjectType (CombatFormula.cpp=Game/Combat/ -> GameObject.h=Game/ 이므로 ../GameObject.h)
#include "../Monster/Monster.h"     // Monster::GetMonsterType / MonsterTemplateFor (monster 공격력=종류별 템플릿)
#include "../Player/Player.h"       // Player::GetAttackPower / GetDefPower (장착 반영 - 매 타격마다 파생 계산)

namespace KYS
{
    namespace SERVERAPP
    {
        // 산식 시작값 (서버 전용 파일 범위. GameDefines 아님 = 클라 공유 0. 부하 측정 후 조정)
        //   player 공격력은 상수가 아니라 Player::GetAttackPower() - 기본(PLAYER_BASE_ATTACK_POWER) + 장착 무기 파생.
        static const int DAMAGE_VARIANCE = 10;   // +- 변동폭 (50 -> 40..60)
        static const int CRIT_CHANCE_PERCENT = 20;   // 치명타 확률(%)
        static const int CRIT_MULTIPLIER = 2;    // 치명타 배수
        static const int MISS_CHANCE_PERCENT = 5;    // 빗나감 확률(%)
        static const int MIN_DAMAGE = 1;    // 최소 보장 데미지 (명중 시 0 방지)

        // 채널 스레드별 독립 난수 상태 (글로벌 thread_local)
        // 1 Channel = 1 Thread 이므로 스레드마다 독립 -> lock 없이 동시 접근 충돌 없음.
        // std::rand는 전역 상태 공유라 멀티채널 동시 접근 충돌 + 하위비트 분포 저질 -> 직접 구현.
        static thread_local unsigned int t_combatRngState = 0;

        static unsigned int NextRandom()
        {
            if (t_combatRngState == 0)
                t_combatRngState = GetCurrentThreadId() | 1u;   // 스레드별 seed (0 회피: xorshift는 0에서 멈춤)
            // xorshift32 - 빠른 정수 난수 (게임 판정용 충분)
            t_combatRngState ^= t_combatRngState << 13;
            t_combatRngState ^= t_combatRngState >> 17;
            t_combatRngState ^= t_combatRngState << 5;
            return t_combatRngState;
        }

        static int RollPercent()            // [0, 100)
        {
            return static_cast<int>(NextRandom() % 100u);
        }
        static int RollSpread(int spread)   // [0, spread)
        {
            return static_cast<int>(NextRandom() % static_cast<unsigned int>(spread));
        }

        int CombatFormula::Compute(GameObject* caster, GameObject* target)
        {
            // 1) 빗나감
            if (RollPercent() < MISS_CHANCE_PERCENT)
                return 0;

            // 2) 기본 공격력 (caster 타입별). monster는 종류별 스탯 테이블,
            //    player는 파생 계산(기본 + 장착 무기) - 저장된 수치가 아니라 타격 순간 인벤에서 읽는다.
            int basePower;
            if (caster->GetObjectType() == EObjectType::MONSTER)
            {
                basePower = MonsterTemplateFor(static_cast<Monster*>(caster)->GetMonsterType()).atkPower;
            }
            else
            {
                basePower = static_cast<Player*>(caster)->GetAttackPower();
            }

            // 3) 변동폭: base - V .. base + V
            int damage = basePower - DAMAGE_VARIANCE + RollSpread(DAMAGE_VARIANCE * 2 + 1);

            // 4) 치명타
            if (RollPercent() < CRIT_CHANCE_PERCENT)
                damage *= CRIT_MULTIPLIER;

            // 5) 대상 방어력 감산 (player 만 - 장착 방어구 파생. monster 방어력은 스코프 밖)
            if (target->GetObjectType() == EObjectType::PLAYER)
            {
                damage -= static_cast<Player*>(target)->GetDefPower();
            }

            // 6) 최소 데미지 보장 (방어력이 아무리 높아도 명중은 1 이상)
            if (damage < MIN_DAMAGE)
                damage = MIN_DAMAGE;

            return damage;
        }

        // 균등 정수 [lo, hi] (양끝 포함). 파일 범위 xorshift32(thread_local, 채널별 동시 접근 충돌 없음)를
        //   public 표면으로 위임 - 몬스터 그룹 스폰/배회 좌표가 std::rand(전역 상태 멀티채널 충돌) 대신 재사용.
        int CombatFormula::RandomRange(int lo, int hi)
        {
            if (hi <= lo)
            {
                return lo;
            }
            const unsigned int span = static_cast<unsigned int>(hi - lo + 1);
            return lo + static_cast<int>(NextRandom() % span);
        }
    }
}
