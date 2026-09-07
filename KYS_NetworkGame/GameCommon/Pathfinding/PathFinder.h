#pragma once
#include "../GameServer/Types/Defines.h"   // 기본 타입
#include "CommonStructs.h"                  // Position
#include "GameDefines.h"                    // WALK_CELL_SIZE / MAX_MAP_SIZE_PX / WALK_CELLS_MAX

// 8방 A*/JPS 격자 길찾기. pathfinder 스레드가 인스턴스 1개를 소유(무락·쿼리마다 재사용).
//   벽은 WalkableTable 공유 표(IsCellWalkable)를 읽는다. 코너 규칙 = strict no-corner-cut
//   (AdvanceMove/IsPathWalkable 과 동일 규약 - 계산한 경로를 이동이 그대로 밟을 수 있다).
//   압축 = 공선 병합만(결정 9=A·postfilter 제거): 방향 전환점(코너)만 waypoint 로 남겨 각 leg 가
//   순수 직교/45° 대각 -> 8방 추종이 정확 재현. (임의 각 현 병합은 8방 이산 이동과 비정합이라 안 함.)

static const int PATH_CELL_STRIDE  = WALK_CELLS_MAX;              // 격자 고정 stride (80) - 인덱스 = row*stride + col
static const int PATH_MAX_CELLS    = WALK_CELLS_MAX * WALK_CELLS_MAX;   // 6400
static const int PATH_NODE_BUDGET  = 2048;   // 노드 확장(pop) 상한 - 초과 = NOPATH (도달불가/과대 탐색 컷)
static const int PATH_WAYPOINT_MAX = 32;     // waypoint 버퍼 상한 - 초과 = 앞 32점 부분 경로(재계산이 이어감)

static const int PATH_COST_ORTHO = 10;   // 직교 한 칸 비용 (정수 octile)
static const int PATH_COST_DIAG  = 14;   // 대각 한 칸 비용 (~10*sqrt2)

// 격자 통행 조회 추상 - 프로덕션은 WalkableTable 공유 표를, 테스트는 인라인 격자를 주입한다.
//   Data 실파일/전역 상태 결합 없이 순수 알고리즘만 채점(결정 8 학습 목적·A*<->JPS 대조 유닛테스트).
//   IJob/INetEventHandler 와 같은 주입 idiom - pathfinder 스레드 밖 계층이라 virtual 비용 무해.
class IGridSource
{
public:
    virtual ~IGridSource() {}
    virtual bool CellWalkable(int mapId, int col, int row) const = 0;   // 격자 밖/mapId 밖 = false
    virtual int  Cols(int mapId) const = 0;                             // 통행 격자 가로 칸 수
    virtual int  Rows(int mapId) const = 0;                             // 통행 격자 세로 칸 수
};

class PathFinder
{
public:
    // grid = nullptr 이면 WalkableTable 공유 표(프로덕션 기본). 테스트는 인라인 IGridSource 주입.
    explicit PathFinder(const IGridSource* grid = nullptr);

    // from -> to 경로. 성공 = waypoints[0..N) 에 "다음 목표점부터 목적지까지" 코너(셀 중심 Position)를
    //   채우고 N(>=1) 반환. 실패(도달불가·노드 예산 초과) = 0. 버퍼 초과 = 앞 maxWaypoints 점(부분 경로).
    //   from/to 는 픽셀 좌표(내부에서 칸으로 환산). 같은 칸이면 목적지 1점. 실제 탐색 = JPS(A* 는 검증기).
    int FindPath(int mapId, const Position& from, const Position& to, Position* waypoints, int maxWaypoints);

    // 순수 A* 최단 비용(경로 재구성 없이 비용만) - JPS 결과와 대조하는 유닛테스트 채점기. 도달불가/예산초과 = -1.
    long long AStarCost(int mapId, const Position& from, const Position& to);

    // JPS 최단 비용 - 유닛테스트가 AStarCost 와 대조(같아야 JPS 정확). 도달불가/예산초과 = -1.
    long long JpsCost(int mapId, const Position& from, const Position& to);

    PathFinder(const PathFinder&) = delete;
    PathFinder& operator=(const PathFinder&) = delete;

private:
    // 한 번의 A* 탐색: startIdx -> goalIdx. 성공 시 m_g/m_cameFrom 을 채우고 true.
    //   generation(m_gen) 스탬프로 방문/비용 배열을 쿼리마다 memset 없이 재사용.
    bool RunAStar(int mapId, int startIdx, int goalIdx);

    // 한 번의 JPS 탐색: A* 와 같은 배열/힙을 쓰되 이웃 8 확장 대신 "점프포인트"만 successor 로 연다.
    //   m_cameFrom 이 점프포인트(코너)끼리를 잇는다 - 재구성이 곧 코너열이라 별도 압축 최소.
    bool RunJPS(int mapId, int startIdx, int goalIdx);

    // (x,y) 에서 (dx,dy) 방향으로 점프 - 목적지/강제이웃(코너)까지 직진하고 그 셀 인덱스 반환(없으면 -1).
    //   no-corner-cut 변형(대각은 양 직교 셀이 열려야 진행) - 고전 코너컷형과 강제이웃 공식이 다르다.
    //   cols/rows = 격자 경계 - 밖으로 나가면 -1(벽 취급). 불량/미적재 격자서 무한 전진 + 배열 OOB 봉인.
    int Jump(int mapId, int x, int y, int dx, int dy, int gc, int gr, int cols, int rows) const;

    // cur 셀에서 점프를 시작할 (dx,dy) 방향 목록을 부모 기준 가지치기로 고른다(start = 전방향).
    //   반환 = 방향 수, dirs[i] = {dx,dy}. no-corner-cut findNeighbors 규약(대각은 코너 열림 확인).
    int PrunedDirections(int mapId, int cellIdx, int dirs[8][2]) const;

    void   HeapPush(long long f, int idx);
    int    HeapPop();        // 최소 f 의 cellIdx 반환 (빈 힙 = -1)
    bool   HeapEmpty() const { return m_heapSize == 0; }

    // 격자 조회 forwarder - 알고리즘 코드는 이 3개만 부르고, 실제 소스(파일/테스트)는 m_grid 가 정한다.
    bool CellWalkable(int mapId, int col, int row) const { return m_grid->CellWalkable(mapId, col, row); }
    int  GridCols(int mapId) const { return m_grid->Cols(mapId); }
    int  GridRows(int mapId) const { return m_grid->Rows(mapId); }

    const IGridSource* m_grid;   // 통행 조회 소스 (기본 = WalkableTable 공유 표)

    // 격자 상태 (generation 스탬프 - m_seenGen[idx]==m_gen 이면 이번 쿼리서 유효)
    int       m_gen;
    int       m_seenGen[PATH_MAX_CELLS];     // 이 셀의 m_g/m_cameFrom 이 이번 쿼리 것인지
    int       m_closedGen[PATH_MAX_CELLS];   // 이번 쿼리서 확정(closed)됐는지
    long long m_g[PATH_MAX_CELLS];           // start 로부터의 비용
    int       m_cameFrom[PATH_MAX_CELLS];    // 부모 셀 인덱스
    int       m_pathScratch[PATH_MAX_CELLS]; // 경로 역추적 임시 버퍼(goal->start 셀 열) - 공선 병합 전 원본

    // 이진 최소 힙 (고정 배열·f 기준). 지연 삭제(pop 시 closed 면 skip)라 같은 셀이 여러 번 들어갈 수 있다.
    //   상한 = 확장 예산(PATH_NODE_BUDGET) x 이웃 8 = 최대 push 수. cells*3 로 그 상한을 덮는다(HeapPush 오버플로 가드 병행).
    static const int PATH_HEAP_CAP = PATH_MAX_CELLS * 3;
    struct HeapNode { long long f; int idx; };
    HeapNode m_heap[PATH_HEAP_CAP];
    int      m_heapSize;
};
