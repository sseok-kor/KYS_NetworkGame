#pragma once
#include "../GameServer/Types/Defines.h"   // BYTE (기본 타입 - MapTable.h 와 같은 선행 include)
#include "CommonStructs.h"                 // Position
#include "GameTypes.h"                     // EMoveDirection (AdvanceMove 시그니처)
#include "GameDefines.h"                   // WALK_* 상수 / MAX_MAP_COUNT

// 통행 격자 (walkable) - 맵 내부의 벽/장애물을 50px 칸 단위 비트로 든다 ('1'=통행 / '0'=벽).
//   서버(이동 검증/스폰/포탈/부활)와 GameClient(이동 예측)·DummyClient(봇 이동)가
//   같은 파일을 같은 로더로 적재하는 공유 표 - 예측이 서버와 같은 곳에서 멈춰야 되당김(보정)이 없다.
//   AOI 격자(CELL_SIZE 250)와는 별개 시스템. 부팅 1회 적재 후 읽기 전용이라 채널 스레드 무락 조회.

// 부팅 1회: 맵마다 통행 격자 적재. 반드시 LoadMapTable 성공 뒤 호출(파일명/크기를 maps.csv 에서 읽음).
//   maps.csv 의 walkableFile 열이 "-" 면 벽 없는 맵(전 칸 통행 채움), 파일명이면 그 CSV 를 강제 적재
//   (파일 없음 = 실패 - 오타 파일명이 조용히 벽 없는 맵이 되는 사고 차단).
//   CSV 가 정본이고 .bin 은 캐시: .bin 헤더에 원본 CSV 수정시각을 기록해 두고, 다르거나 손상이면
//   CSV 를 다시 파싱해 .bin 을 재생성한다 (임시 파일에 쓴 뒤 원자 교체 - 여러 앱이 동시에 떠도 안전).
//   부활 좌표가 통행 칸 위인지도 여기서 검증한다.
// 반환 = 적재한 맵 수 / 실패 = -1. 서버/DummyClient 는 실패 시 부팅 거부,
//   GameClient 는 미적재로 두고 진행(전-통행 폴백 - 벽을 모르는 옛 동작으로 강등).
int LoadWalkableTable();

// 통행 격자가 적재됐는가 (GameClient 가 강등 모드 경고를 띄울 때 사용)
bool IsWalkableTableLoaded();

// (x,y)가 밟을 수 있는 칸인가. O(1) 비트 조회.
//   맵 밖 좌표 = false / mapId 가 범위 밖 = false (손상 데이터 방어) / 표 미적재 = true (전-통행 폴백).
//   x==width 같은 맵 끝 좌표(폐구간)는 마지막 칸으로 취급한다.
bool IsWalkable(int mapId, int x, int y);

// from->to 직선 구간이 전부 통행인가 - 두 점 사이의 모든 칸을 검사한다.
//   끝점만 검사하면 이동 허용 오차(64px)가 벽 두께(50px)보다 커서 얇은 벽 너머 좌표 보고가
//   통과하는 구멍이 생긴다 - 이동 보고 수용 게이트가 이 함수를 쓴다.
bool IsPathWalkable(int mapId, const Position& from, const Position& to);

// 한 tick 의 이동 한 걸음: 전진(int 절단) -> 맵 경계 클램프 -> 목적 칸이 벽이면 제자리(from 그대로).
//   서버(Player/Monster)·클라(자기 예측/원격 외삽)·봇이 전부 이 함수 하나를 쓴다 -
//   위치 예측은 서버와 클라가 "같은 비트 결과"를 내야 desync 가 없어서, 수식을 한 곳에 모아
//   구조로 보장한다 (예전엔 같은 수식이 4곳에 복사돼 있어 어긋나면 상시 보정이 나는 구조였다).
Position AdvanceMove(int mapId, const Position& from, EMoveDirection dir, int speedPxPerSec, float dt);

// from->to 사이에 벽이 없는가(시야 - supercover 만, 코너 컷 무관). 전투 LoS·몬스터 repath 가 쓴다.
//   이동(IsPathWalkable)과 달리 벽 모서리 스침을 허용한다 - 시선/투사체는 몸을 밀어 넣지 않기 때문.
//   표 미적재 = true(벽 모름 = 시야 통과·전-통행 폴백).
bool HasLineOfSight(int mapId, const Position& from, const Position& to);

// 칸 (col,row) 이 통행 칸인가(격자 밖/mapId 밖 = false·미적재 = true). A* 이웃 확장이 쓰는 O(1) 조회.
bool IsCellWalkable(int mapId, int col, int row);

// 맵의 통행 격자 칸 수 (폭/높이 / 50). A* 가 격자 경계를 아는 데 쓴다.
int WalkCellCols(int mapId);
int WalkCellRows(int mapId);

// from 에서 to 까지 통행 칸만 밟아 이어지는가 (4방 flood-fill·부팅 연결성 검사 전용).
//   격리된 통행 영역(도달 불가 포탈/스폰 = 데이터 실수)을 부팅에서 fail-fast 로 잡는다.
//   4방 도달집합 == A*/JPS(8방 no-corner-cut) 도달집합 - 코너 컷 금지라 대각 연결은 두 직교
//   스텝으로 분해 가능해서다(그래서 8방 대신 4방으로 검사해도 실제 이동 가능성과 정확히 일치).
//   (단 A*/JPS 는 노드 예산 초과 시 도달 가능해도 NOPATH - 병리적 초장거리 미로 한정 차이이고,
//    이 검사가 대상하는 '플레이어 걸음 이동'엔 예산 개념이 없어 무관하다.)
//   표 미적재/mapId 밖 = true(전-통행 폴백 - 벽을 모르면 격리 개념도 없다).
bool IsReachable(int mapId, const Position& from, const Position& to);
