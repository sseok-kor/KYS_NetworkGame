#pragma once
#include "Monster.h"   // EMonsterType (그룹의 몬스터 종류)

namespace KYS
{
    namespace SERVERAPP
    {
        // 스폰 그룹 한 행 - "이 맵의 이 사각 구역에 이 종류를 count 마리 유지한다".
        //   부활 위치 = 구역 안 랜덤 (부활 위치 == 배회 기준점). 죽은 몹은 시신 회수 후 respawnDelayMs 뒤 부활.
        struct SpawnGroup
        {
            int          mapId;            // 어느 맵 (maps.csv 행 인덱스)
            EMonsterType monsterType;      // 종류 (스탯/보스 판정 키)
            int          x0, y0, x1, y1;   // 스폰 사각 구역 (x0<=x1, y0<=y1 - 로더가 검증)
            int          count;            // 이 그룹이 유지할 마릿수 (1..MAX_GROUP_COUNT)
            int          respawnDelayMs;   // 죽은 뒤 부활까지 지연 ms (1 이상 - 0은 구버전 점 스폰 파일로 보고 부팅 거부)
        };

        // 부팅 1회: Data/spawns.csv 적재 - 스폰 그룹 표 (서버 전용).
        //   반드시 LoadMapTable(맵 수/monsterCap/부활 좌표)과 LoadMonsterTable(isBoss/cap/aggroRange) 뒤에 호출
        //   (아래 교차검증들이 두 표를 참조).
        //
        // 반환 = 적재한 그룹 수. 실패 = -1, 서버는 fail-fast (조용한 행 무시 없이 전부 부팅 거부):
        //   파일 없음/0행/행 수 초과/필드 8개 아님/mapId/type 범위 밖/x0>x1 또는 y0>y1/구역이 맵 밖/
        //   count 가 1..MAX_GROUP_COUNT 밖/respawnDelayMs 1 미만(구버전 파일 판별 겸용)/
        //   맵별 count 합 > maps.csv monsterCap(cap 은 안전 천장 - 초과 데이터는 튜닝 실수)/
        //   전체 count 합 > 몬스터 풀 용량(이중 안전망)/
        //   종류별 cap(monsters.yml/cap>0 인 종류)의 같은 맵 count 합 초과/
        //   스폰 구역이 그 맵 부활 지점(respawn)과 어그로 거리(aggroRange) 안 - 스폰 위치 기준 보장
        //   (배회로 구역 밖까지는 나갈 수 있음 - 부활 무적 1초가 완충).
        int               LoadSpawnTable();

        // 적재된 그룹 수. index 는 0..count-1 (= 행 순서).
        int               SpawnGroupCount();

        // 스폰 그룹 연속 배열의 시작 - MapManager::InitSpawns 에 (배열, 개수) 통째로 넘긴다.
        const SpawnGroup* SpawnGroupData();
    }
}
