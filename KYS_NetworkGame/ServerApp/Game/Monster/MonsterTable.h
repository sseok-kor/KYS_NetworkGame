#pragma once
#include "Monster.h"   // MonsterTemplate / EMonsterType (적재 대상 스탯 구조)

namespace KYS
{
    namespace SERVERAPP
    {
        // 부팅 1회: Data/monsters.yml 적재 - 몬스터 종류별 스탯 표 + 드랍 규칙(몬스터 블록 안 중첩)을 한 파일에서 읽는다.
        //   스탯은 MonsterTemplateFor 가 조회하는 내부 표에, 드랍은 DropTable 저장소(AddDropRule)에 채운다.
        //   서버 전용 - 드랍률이 들어 있어 클라에 배포/노출하지 않는다 (기존 drops.csv 소관을 흡수).
        //   반드시 LoadItemTable 뒤에 호출 (드랍 행의 templateId 를 아이템 사전과 대조하는 참조 무결성 검증 때문).
        //
        // 반환 = 적재한 몬스터 종류 수 (= EMonsterType::COUNT). 실패 = -1, 서버는 fail-fast.
        //   실패 조건 (하드코딩 표 시절의 컴파일 검증을 부팅 검증으로 대체 - 조용한 무시 없이 전부 부팅 거부):
        //   파일 없음/빈 파일/버퍼 상한 초과/YAML 파싱 오류/필수 필드 누락(스탯/드랍 행)/종류(type) 누락/중복/범위 밖/
        //   moveSpeed 범위 밖(43..MAX_MOVE_SPEED - 하한 43 = 8방 대각 성분이 30Hz 절단으로 1px·상한은 클라 예측 신뢰 경계)/
        //   드랍 아이템이 items.csv 에 없음/드랍 maxQty 가 stackMax 초과/드랍 저장소 가득/드랍 규칙 0행/
        //   isBoss 인데 cap 이 1 미만 (보스는 맵당 상한이 반드시 있어야 함).
        int LoadMonsterTable();

        // 종류별 보스 여부/맵당 상한 조회 (monsters.yml 적재 값 - 스폰 그룹 검증과 리스폰 우선순위가 사용).
        //   cap 0 = 상한 없음 (잡몹 기본값 - "0마리 허용"이 아님). 범위 밖 type = 보스 아님/상한 없음 취급.
        bool MonsterIsBoss(EMonsterType type);
        int  MonsterCapFor(EMonsterType type);
    }
}
