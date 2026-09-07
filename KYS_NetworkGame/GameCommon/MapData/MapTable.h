#pragma once
#include "../GameServer/Types/Defines.h"   // BYTE/UINT32 (GameDefines 가 쓰는 기본 타입 - ItemTable.h 와 같은 선행 include)
#include "CommonStructs.h"                 // Position (부활 좌표)
#include "GameDefines.h"                   // MAP_NAME_MAX / MAP_FILE_MAX / MAX_MAP_COUNT

// 공유 맵 메타 표 - 서버(스폰 목표/부활 좌표)와 DummyClient(봇 목표 맵 범위)가
//   같은 파일(Data/maps.csv)을 부팅 시 적재하는 단일 진실원. 맵 개수 = 이 파일의 행 수.
//   하드코딩 맵 상수 표(kMapMonsterTarget/kRespawnPos)를 대체 - 맵 조정은 재컴파일 대신 파일 한 줄.
struct MapDef
{
    int      mapId;                        // 맵 번호 (반드시 행 순서 0..N-1과 일치 - 배열 인덱스 겸용)
    wchar_t  name[MAP_NAME_MAX];           // 표시 이름 (한글 가능, CSV는 UTF-8 저장. 지금은 예약 - 코드 미사용)
    int      mapType;                      // 0=마을(town) 1=필드(field) 2=던전(dungeon/보스맵) (예약 필드 - 코드 미사용)
    int      width;                        // 맵 가로 px (맵마다 다를 수 있음 - 좌표 유효 범위 [0, width]. 로더가 하한/상한/단위 검증)
    int      height;                       // 맵 세로 px (위와 동일 - [0, height])
    int      monsterCap;                   // 맵 몬스터 수 안전 천장 (스폰 그룹 count 합 <= 이 값 - 로더 검증. 실제 인구는 그룹이 정한다)
    Position respawn;                      // 플레이어 부활 좌표 (몬스터 스폰 중앙부에서 먼 구석)
    int      musicId;                      // 예약 필드 (코드 미사용)
    wchar_t  walkableFile[MAP_FILE_MAX];   // 통행 격자 파일명 ("-" = 벽 없는 맵[전 칸 통행] / 파일명 = Data/ 의 통행 CSV 강제 적재)
};

// 부팅 1회: Data/maps.csv 적재. 반환 = 적재한 맵 수 (실패 = -1, 서버/봇 모두 fail-fast).
//   실패 조건: 파일 없음 / 0행 / 행 수 > MAX_MAP_COUNT / mapId가 행 순서와 불일치 /
//   width/height가 [MIN_MAP_SIZE_PX, MAX_MAP_SIZE_PX] 밖이거나 MAP_SIZE_STEP_PX(250) 배수가 아님 /
//   monsterCap 음수 / 부활 좌표 맵 밖 / 맵 전체 monsterCap 합이 몬스터 풀 용량 초과.
//   (조용한 절단/무시 없이 전부 부팅 거부)
int           LoadMapTable();

// 적재된 맵 수 (= 런타임 실제 맵 수. 배열 상한 MAX_MAP_COUNT와 분리된 "쓰는 칸 수").
int           MapTableCount();

// mapId(= 행 인덱스)가 가리키는 맵 메타. 호출자가 0..count-1 범위를 보장한다.
const MapDef& MapTableAt(int mapId);

// 맵 크기 안전 조회 - 표 미적재(GameClient 로드 실패 관대 폴백)나 범위 밖 mapId면 MAX_MAP_SIZE_PX(4000)를 준다.
//   이동/렌더/좌표 검사의 경계 클램프는 반드시 이 함수로 읽는다 (MapTableAt 직접 인덱싱은 미적재 시
//   zero-init 값 0을 돌려줘 전원이 (0,0)에 고정되는 사고 - 가드 접근자가 그 실패 모드를 구조로 봉인).
int           MapWidthFor(int mapId);
int           MapHeightFor(int mapId);
