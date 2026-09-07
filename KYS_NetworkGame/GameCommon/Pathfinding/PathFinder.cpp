#include "pch_gamecommon.h"
#include "PathFinder.h"
#include "MapData/WalkableTable.h"   // IsCellWalkable / WalkCellCols / WalkCellRows (벽/격자 경계 조회)
#include "GameDefines.h"     // WALK_CELL_SIZE (칸<->픽셀 환산)

#include <cstring>           // memset (generation 배열 최초 0화)

// 기본 격자 소스: WalkableTable 공유 표로 위임(프로덕션). 테스트는 자기 IGridSource 를 주입한다.
namespace
{
    class WalkableTableGrid : public IGridSource
    {
    public:
        bool CellWalkable(int mapId, int col, int row) const override { return IsCellWalkable(mapId, col, row); }
        int  Cols(int mapId) const override { return WalkCellCols(mapId); }
        int  Rows(int mapId) const override { return WalkCellRows(mapId); }
    };
    WalkableTableGrid g_defaultGrid;
}

// 셀 인덱스 <-> (col,row) 환산. stride 는 격자 고정폭(PATH_CELL_STRIDE=80) - 실제 격자가 더 좁아도
//   인덱스 규약을 한 값으로 고정해야 배열 크기(PATH_MAX_CELLS)와 어긋나지 않는다.
static inline int ColOf(int idx) { return idx % PATH_CELL_STRIDE; }
static inline int RowOf(int idx) { return idx / PATH_CELL_STRIDE; }

// 정수 octile 휴리스틱: h = ORTHO*(dx+dy) - (2*ORTHO-DIAG)*min(dx,dy).
//   대각으로 갈 수 있는 만큼(min) 은 DIAG(14) 로, 남는 축차(|dx-dy|) 는 ORTHO(10) 로 센다 -
//   실제 8방 최단과 정확히 일치하는 admissible+consistent 휴리스틱(과대평가 없음 = 최단 보장).
static inline long long OctileH(int c0, int r0, int c1, int r1)
{
    int dx = c1 - c0; if (dx < 0) { dx = -dx; }
    int dy = r1 - r0; if (dy < 0) { dy = -dy; }
    const int mn = (dx < dy) ? dx : dy;
    const long long straightPart = static_cast<long long>(PATH_COST_ORTHO) * (dx + dy);
    const long long diagSaving = static_cast<long long>(2 * PATH_COST_ORTHO - PATH_COST_DIAG) * mn;
    return straightPart - diagSaving;   // 2*10-14 = 6 절감/대각칸
}

// 두 점프포인트 사이 leg 의 정확 비용. JPS leg 는 순수 직교/45도 대각이라
//   대각 성분(min)은 DIAG(14), 남는 직진 성분(max-min)은 ORTHO(10) 로 센다.
static inline long long OctileDistanceCost(int c0, int r0, int c1, int r1)
{
    int adx = c1 - c0; if (adx < 0) { adx = -adx; }
    int ady = r1 - r0; if (ady < 0) { ady = -ady; }
    const int mn = (adx < ady) ? adx : ady;
    const int mx = (adx < ady) ? ady : adx;
    return static_cast<long long>(PATH_COST_DIAG) * mn + static_cast<long long>(PATH_COST_ORTHO) * (mx - mn);
}

// 칸 (col,row) 의 중심 픽셀. 경로점은 칸 경계가 아니라 중심으로 내보내야
//   이동(AdvanceMove)이 그 점을 향해 걸을 때 벽 옆을 아슬하게 긁지 않는다.
static inline Position CellCenter(int col, int row)
{
    Position p;
    p.x = col * WALK_CELL_SIZE + WALK_CELL_SIZE / 2;
    p.y = row * WALK_CELL_SIZE + WALK_CELL_SIZE / 2;
    return p;
}

// 부호(-1/0/+1) - 두 셀 사이 진행 방향 성분. 공선 판정에 쓴다.
static inline int Sign(int v) { return (v > 0) - (v < 0); }

PathFinder::PathFinder(const IGridSource* grid)
    : m_grid(grid ? grid : &g_defaultGrid)
    , m_gen(0)
    , m_heapSize(0)
{
    // generation 스탬프 방식: 배열을 쿼리마다 밀지 않고 m_gen 을 올려 "이번 쿼리 것"을 구분한다.
    //   최초 1회만 0 으로 채워 첫 쿼리(gen=1)와 확실히 다르게 한다(0 != 1).
    memset(m_seenGen, 0, sizeof(m_seenGen));
    memset(m_closedGen, 0, sizeof(m_closedGen));
}

void PathFinder::HeapPush(long long f, int idx)
{
    if (m_heapSize >= PATH_HEAP_CAP) { return; }   // 오버플로 가드 - 노드 예산이 먼저 걸려 실제로는 도달 안 함
    int i = m_heapSize++;
    m_heap[i].f = f;
    m_heap[i].idx = idx;
    while (i > 0)
    {
        const int parent = (i - 1) / 2;
        if (m_heap[parent].f <= m_heap[i].f) { break; }
        const HeapNode tmp = m_heap[parent];
        m_heap[parent] = m_heap[i];
        m_heap[i] = tmp;
        i = parent;
    }
}

int PathFinder::HeapPop()
{
    if (m_heapSize == 0) { return -1; }
    const int result = m_heap[0].idx;
    m_heap[0] = m_heap[--m_heapSize];
    int i = 0;
    for (;;)
    {
        const int left = 2 * i + 1;
        const int right = 2 * i + 2;
        int smallest = i;
        if (left < m_heapSize && m_heap[left].f < m_heap[smallest].f) { smallest = left; }
        if (right < m_heapSize && m_heap[right].f < m_heap[smallest].f) { smallest = right; }
        if (smallest == i) { break; }
        const HeapNode tmp = m_heap[smallest];
        m_heap[smallest] = m_heap[i];
        m_heap[i] = tmp;
        i = smallest;
    }
    return result;
}

bool PathFinder::RunAStar(int mapId, int startIdx, int goalIdx)
{
    // generation 스탬프가 int 상한이면 배열을 0화하고 순환(2^31 쿼리마다 1회 - 스탬프 충돌/부호 오버플로 방지).
    if (m_gen >= 0x7FFFFFFF) { memset(m_seenGen, 0, sizeof(m_seenGen)); memset(m_closedGen, 0, sizeof(m_closedGen)); m_gen = 0; }
    ++m_gen;
    m_heapSize = 0;

    const int cols = GridCols(mapId);
    const int rows = GridRows(mapId);
    if (cols <= 0 || rows <= 0) { return false; }
    if (cols > PATH_CELL_STRIDE || rows > PATH_CELL_STRIDE) { return false; }   // 격자>stride = 인덱스 규약 붕괴 방어(로드가 MAX_MAP_SIZE_PX 로 막지만)

    const int gc = ColOf(goalIdx);
    const int gr = RowOf(goalIdx);

    m_g[startIdx] = 0;
    m_cameFrom[startIdx] = -1;
    m_seenGen[startIdx] = m_gen;
    HeapPush(OctileH(ColOf(startIdx), RowOf(startIdx), gc, gr), startIdx);

    // 이웃 8방 델타(col,row): 0..3 직교(상/하/좌/우), 4..7 대각. 대각은 코너 컷 검사를 건다.
    static const int NEIGHBOR_DC[8] = { 0,  0, -1,  1, -1,  1, -1,  1 };
    static const int NEIGHBOR_DR[8] = { -1,  1,  0,  0, -1, -1,  1,  1 };

    int expansions = 0;
    while (!HeapEmpty())
    {
        const int cur = HeapPop();
        if (m_closedGen[cur] == m_gen) { continue; }   // 지연 삭제 - 이미 확정된 셀의 낡은 힙 엔트리
        m_closedGen[cur] = m_gen;

        if (cur == goalIdx) { return true; }

        if (++expansions > PATH_NODE_BUDGET) { return false; }   // 예산 초과 = NOPATH(도달불가/과대 탐색 컷)

        const int cc = ColOf(cur);
        const int cr = RowOf(cur);
        for (int k = 0; k < 8; ++k)
        {
            const int nc = cc + NEIGHBOR_DC[k];
            const int nr = cr + NEIGHBOR_DR[k];
            if (nc < 0 || nc >= cols || nr < 0 || nr >= rows) { continue; }
            if (!CellWalkable(mapId, nc, nr)) { continue; }

            const bool diagonal = (k >= 4);
            if (diagonal)
            {
                // strict no-corner-cut: 대각으로 스치는 두 직교 이웃이 둘 다 통행이어야 대각 이동 허용.
                //   (AdvanceMove/IsPathWalkable 의 코너 규칙과 동일 - 계산한 경로를 이동이 그대로 밟는다.)
                if (!CellWalkable(mapId, cc + NEIGHBOR_DC[k], cr)) { continue; }
                if (!CellWalkable(mapId, cc, cr + NEIGHBOR_DR[k])) { continue; }
            }

            const int nIdx = nr * PATH_CELL_STRIDE + nc;
            if (m_closedGen[nIdx] == m_gen) { continue; }

            const long long step = diagonal ? PATH_COST_DIAG : PATH_COST_ORTHO;
            const long long ng = m_g[cur] + step;
            if (m_seenGen[nIdx] != m_gen || ng < m_g[nIdx])
            {
                m_seenGen[nIdx] = m_gen;
                m_g[nIdx] = ng;
                m_cameFrom[nIdx] = cur;
                HeapPush(ng + OctileH(nc, nr, gc, gr), nIdx);
            }
        }
    }
    return false;   // 힙 소진 = 도달 불가
}

int PathFinder::Jump(int mapId, int x, int y, int dx, int dy, int gc, int gr, int cols, int rows) const
{
    // (x,y) = 부모에서 (dx,dy) 방향으로 한 칸 밟아 들어온 셀. 점프포인트면 그 인덱스, 없으면 -1.
    //   대각 진입의 코너 규칙은 PrunedDirections 가 가지치기 단계에서 이미 걸러 여기 도착 = 진입 합법.
    for (;;)
    {
        if (x < 0 || x >= cols || y < 0 || y >= rows) { return -1; }   // 격자 밖 = 벽(경계 클램프 소관) - 무한 전진 + 배열 OOB 봉인
        if (!CellWalkable(mapId, x, y)) { return -1; }
        if (x == gc && y == gr) { return y * PATH_CELL_STRIDE + x; }

        if (dx != 0 && dy != 0)
        {
            // 대각: 두 직교 성분 중 하나라도 점프포인트를 찾으면 (x,y)가 코너(점프포인트).
            //   직교 Jump 는 직진 루프뿐이라 재귀 깊이 <=2(대각 -> 직교) 로 유계.
            if (Jump(mapId, x + dx, y, dx, 0, gc, gr, cols, rows) >= 0) { return y * PATH_CELL_STRIDE + x; }
            if (Jump(mapId, x, y + dy, 0, dy, gc, gr, cols, rows) >= 0) { return y * PATH_CELL_STRIDE + x; }
            // 대각 계속 = 양 직교 셀이 열려야(no-corner-cut). 막히면 이 방향 점프포인트 없음.
            if (CellWalkable(mapId, x + dx, y) && CellWalkable(mapId, x, y + dy))
            {
                x += dx;
                y += dy;
                continue;
            }
            return -1;
        }

        // 직진: 강제이웃(코너) 검출 - no-corner-cut 변형.
        //   수평 이동 중, 옆 칸(위/아래)은 열렸는데 그 칸의 "직전(부모)측"이 막혔으면
        //   그 옆 칸은 오직 (x,y) 를 거쳐야 도달 = 강제이웃 -> (x,y)가 코너.
        if (dx != 0)   // 수평
        {
            if ((CellWalkable(mapId, x, y - 1) && !CellWalkable(mapId, x - dx, y - 1)) ||
                (CellWalkable(mapId, x, y + 1) && !CellWalkable(mapId, x - dx, y + 1)))
            {
                return y * PATH_CELL_STRIDE + x;
            }
        }
        else           // 수직
        {
            if ((CellWalkable(mapId, x - 1, y) && !CellWalkable(mapId, x - 1, y - dy)) ||
                (CellWalkable(mapId, x + 1, y) && !CellWalkable(mapId, x + 1, y - dy)))
            {
                return y * PATH_CELL_STRIDE + x;
            }
        }
        x += dx;
        y += dy;
    }
}

int PathFinder::PrunedDirections(int mapId, int cellIdx, int dirs[8][2]) const
{
    const int x = ColOf(cellIdx);
    const int y = RowOf(cellIdx);
    int n = 0;

    const int parent = m_cameFrom[cellIdx];
    if (parent < 0)
    {
        // start: no-corner-cut 로 밟을 수 있는 전방향. 직교는 대상 통행, 대각은 대상+양 직교 통행.
        static const int DC[8] = { 0,  0, -1,  1, -1,  1, -1,  1 };
        static const int DR[8] = { -1,  1,  0,  0, -1, -1,  1,  1 };
        for (int k = 0; k < 8; ++k)
        {
            const int dx = DC[k];
            const int dy = DR[k];
            if (!CellWalkable(mapId, x + dx, y + dy)) { continue; }
            if (dx != 0 && dy != 0)
            {
                if (!CellWalkable(mapId, x + dx, y)) { continue; }
                if (!CellWalkable(mapId, x, y + dy)) { continue; }
            }
            dirs[n][0] = dx;
            dirs[n][1] = dy;
            ++n;
        }
        return n;
    }

    // 부모->cur 정규화 방향(-1/0/+1).
    const int px = ColOf(parent);
    const int py = RowOf(parent);
    const int dx = (x > px) - (x < px);
    const int dy = (y > py) - (y < py);

    if (dx != 0 && dy != 0)
    {
        // 대각 이동 중: 자연 이웃 3(직교 dy·직교 dx·대각) - no-corner-cut findNeighbors.
        const bool sideDy = CellWalkable(mapId, x, y + dy);
        const bool sideDx = CellWalkable(mapId, x + dx, y);
        if (sideDy) { dirs[n][0] = 0;  dirs[n][1] = dy; ++n; }
        if (sideDx) { dirs[n][0] = dx; dirs[n][1] = 0;  ++n; }
        if (sideDy && sideDx && CellWalkable(mapId, x + dx, y + dy))
        {
            dirs[n][0] = dx; dirs[n][1] = dy; ++n;
        }
    }
    else if (dx != 0)
    {
        // 수평 이동 중.
        const bool nextOpen = CellWalkable(mapId, x + dx, y);
        const bool topOpen = CellWalkable(mapId, x, y + 1);
        const bool botOpen = CellWalkable(mapId, x, y - 1);
        if (nextOpen)
        {
            dirs[n][0] = dx; dirs[n][1] = 0; ++n;
            if (topOpen) { dirs[n][0] = dx; dirs[n][1] = 1;  ++n; }   // 양 직교(dx·위) 열림 = 코너 진입 합법
            if (botOpen) { dirs[n][0] = dx; dirs[n][1] = -1; ++n; }
        }
        if (topOpen) { dirs[n][0] = 0; dirs[n][1] = 1;  ++n; }
        if (botOpen) { dirs[n][0] = 0; dirs[n][1] = -1; ++n; }
    }
    else
    {
        // 수직 이동 중.
        const bool nextOpen = CellWalkable(mapId, x, y + dy);
        const bool rightOpen = CellWalkable(mapId, x + 1, y);
        const bool leftOpen = CellWalkable(mapId, x - 1, y);
        if (nextOpen)
        {
            dirs[n][0] = 0; dirs[n][1] = dy; ++n;
            if (rightOpen) { dirs[n][0] = 1;  dirs[n][1] = dy; ++n; }
            if (leftOpen) { dirs[n][0] = -1; dirs[n][1] = dy; ++n; }
        }
        if (rightOpen) { dirs[n][0] = 1;  dirs[n][1] = 0; ++n; }
        if (leftOpen) { dirs[n][0] = -1; dirs[n][1] = 0; ++n; }
    }
    return n;
}

bool PathFinder::RunJPS(int mapId, int startIdx, int goalIdx)
{
    // generation 스탬프가 int 상한이면 배열을 0화하고 순환(2^31 쿼리마다 1회 - 스탬프 충돌/부호 오버플로 방지).
    if (m_gen >= 0x7FFFFFFF) { memset(m_seenGen, 0, sizeof(m_seenGen)); memset(m_closedGen, 0, sizeof(m_closedGen)); m_gen = 0; }
    ++m_gen;
    m_heapSize = 0;

    const int cols = GridCols(mapId);
    const int rows = GridRows(mapId);
    if (cols <= 0 || rows <= 0) { return false; }
    if (cols > PATH_CELL_STRIDE || rows > PATH_CELL_STRIDE) { return false; }   // 격자>stride = 인덱스 규약 붕괴 방어(로드가 MAX_MAP_SIZE_PX 로 막지만)

    const int gc = ColOf(goalIdx);
    const int gr = RowOf(goalIdx);

    m_g[startIdx] = 0;
    m_cameFrom[startIdx] = -1;
    m_seenGen[startIdx] = m_gen;
    HeapPush(OctileH(ColOf(startIdx), RowOf(startIdx), gc, gr), startIdx);

    int expansions = 0;
    while (!HeapEmpty())
    {
        const int cur = HeapPop();
        if (m_closedGen[cur] == m_gen) { continue; }   // 지연 삭제
        m_closedGen[cur] = m_gen;

        if (cur == goalIdx) { return true; }
        if (++expansions > PATH_NODE_BUDGET) { return false; }

        int dirs[8][2];
        const int ndirs = PrunedDirections(mapId, cur, dirs);
        const int cc = ColOf(cur);
        const int cr = RowOf(cur);
        for (int i = 0; i < ndirs; ++i)
        {
            const int jp = Jump(mapId, cc + dirs[i][0], cr + dirs[i][1], dirs[i][0], dirs[i][1], gc, gr, cols, rows);
            if (jp < 0) { continue; }
            if (m_closedGen[jp] == m_gen) { continue; }

            // cur -> jp 는 순수 직진/대각 한 leg - 비용은 두 셀 사이 octile 거리.
            const long long ng = m_g[cur] + OctileDistanceCost(cc, cr, ColOf(jp), RowOf(jp));
            if (m_seenGen[jp] != m_gen || ng < m_g[jp])
            {
                m_seenGen[jp] = m_gen;
                m_g[jp] = ng;
                m_cameFrom[jp] = cur;
                HeapPush(ng + OctileH(ColOf(jp), RowOf(jp), gc, gr), jp);
            }
        }
    }
    return false;
}

int PathFinder::FindPath(int mapId, const Position& from, const Position& to, Position* waypoints, int maxWaypoints)
{
    if (waypoints == nullptr || maxWaypoints <= 0) { return 0; }

    const int cols = GridCols(mapId);
    const int rows = GridRows(mapId);
    if (cols <= 0 || rows <= 0) { return 0; }

    // 픽셀 -> 칸 환산 후 격자 범위로 클램프(맵 끝 좌표가 격자 밖 인덱스가 되지 않게).
    int sc = from.x / WALK_CELL_SIZE; if (sc < 0) { sc = 0; } else if (sc >= cols) { sc = cols - 1; }
    int sr = from.y / WALK_CELL_SIZE; if (sr < 0) { sr = 0; } else if (sr >= rows) { sr = rows - 1; }
    int gc = to.x / WALK_CELL_SIZE;   if (gc < 0) { gc = 0; } else if (gc >= cols) { gc = cols - 1; }
    int gr = to.y / WALK_CELL_SIZE;   if (gr < 0) { gr = 0; } else if (gr >= rows) { gr = rows - 1; }

    const int startIdx = sr * PATH_CELL_STRIDE + sc;
    const int goalIdx = gr * PATH_CELL_STRIDE + gc;

    if (startIdx == goalIdx)
    {
        waypoints[0] = CellCenter(gc, gr);   // 같은 칸 - 목적지 1점만
        return 1;
    }
    if (!CellWalkable(mapId, gc, gr)) { return 0; }        // 목적지가 벽 = NOPATH
    if (!RunJPS(mapId, startIdx, goalIdx)) { return 0; }     // 도달불가/예산초과 = NOPATH (실제 탐색 = JPS)

    // goal -> start 부모 체인 역추적 (m_pathScratch[0]=goal .. [len-1]=start).
    //   JPS 는 m_cameFrom 이 점프포인트(코너)끼리를 잇지만, 두 점프포인트가 공선일 수 있어
    //   아래 공선 병합을 A* 와 동일하게 한 번 더 통과시킨다(코너 간 leg 방향 부호로 판정 - 다칸 leg 도 정상).
    int len = 0;
    for (int cur = goalIdx; cur != -1; cur = m_cameFrom[cur])
    {
        m_pathScratch[len++] = cur;
        if (len >= PATH_MAX_CELLS) { break; }   // 사이클 불가하나 배열 상한 방어
    }

    // 공선 병합: start 방향으로 훑으며 "직전 진행 방향과 달라지는 칸(코너)" + 목적지만 waypoint 로 남긴다.
    //   각 leg 가 순수 직교/45도 대각이라 8방 추종이 정확 재현(임의각 현으로 뭉개지 않음 = 결정 9=A).
    //   i 는 start(0)에서 goal(len-1)로 가는 순번 -> m_pathScratch 는 역순이라 [len-1-i] 로 읽는다.
    int count = 0;
    for (int i = 1; i < len && count < maxWaypoints; ++i)
    {
        bool keep = (i == len - 1);   // 목적지는 항상
        if (!keep)
        {
            const int prevCell = m_pathScratch[len - 1 - (i - 1)];
            const int curCell = m_pathScratch[len - 1 - i];
            const int nextCell = m_pathScratch[len - 1 - (i + 1)];
            const int pdx = Sign(ColOf(curCell) - ColOf(prevCell));
            const int pdy = Sign(RowOf(curCell) - RowOf(prevCell));
            const int ndx = Sign(ColOf(nextCell) - ColOf(curCell));
            const int ndy = Sign(RowOf(nextCell) - RowOf(curCell));
            keep = (pdx != ndx) || (pdy != ndy);   // 방향 전환점 = 코너
        }
        if (keep)
        {
            const int wc = ColOf(m_pathScratch[len - 1 - i]);
            const int wr = RowOf(m_pathScratch[len - 1 - i]);
            waypoints[count++] = CellCenter(wc, wr);
        }
    }
    return count;
}

long long PathFinder::AStarCost(int mapId, const Position& from, const Position& to)
{
    const int cols = GridCols(mapId);
    const int rows = GridRows(mapId);
    if (cols <= 0 || rows <= 0) { return -1; }

    int sc = from.x / WALK_CELL_SIZE; if (sc < 0) { sc = 0; } else if (sc >= cols) { sc = cols - 1; }
    int sr = from.y / WALK_CELL_SIZE; if (sr < 0) { sr = 0; } else if (sr >= rows) { sr = rows - 1; }
    int gc = to.x / WALK_CELL_SIZE;   if (gc < 0) { gc = 0; } else if (gc >= cols) { gc = cols - 1; }
    int gr = to.y / WALK_CELL_SIZE;   if (gr < 0) { gr = 0; } else if (gr >= rows) { gr = rows - 1; }

    const int startIdx = sr * PATH_CELL_STRIDE + sc;
    const int goalIdx = gr * PATH_CELL_STRIDE + gc;

    if (startIdx == goalIdx) { return 0; }
    if (!CellWalkable(mapId, gc, gr)) { return -1; }
    if (!RunAStar(mapId, startIdx, goalIdx)) { return -1; }
    return m_g[goalIdx];
}

long long PathFinder::JpsCost(int mapId, const Position& from, const Position& to)
{
    const int cols = GridCols(mapId);
    const int rows = GridRows(mapId);
    if (cols <= 0 || rows <= 0) { return -1; }

    int sc = from.x / WALK_CELL_SIZE; if (sc < 0) { sc = 0; } else if (sc >= cols) { sc = cols - 1; }
    int sr = from.y / WALK_CELL_SIZE; if (sr < 0) { sr = 0; } else if (sr >= rows) { sr = rows - 1; }
    int gc = to.x / WALK_CELL_SIZE;   if (gc < 0) { gc = 0; } else if (gc >= cols) { gc = cols - 1; }
    int gr = to.y / WALK_CELL_SIZE;   if (gr < 0) { gr = 0; } else if (gr >= rows) { gr = rows - 1; }

    const int startIdx = sr * PATH_CELL_STRIDE + sc;
    const int goalIdx = gr * PATH_CELL_STRIDE + gc;

    if (startIdx == goalIdx) { return 0; }
    if (!CellWalkable(mapId, gc, gr)) { return -1; }
    if (!RunJPS(mapId, startIdx, goalIdx)) { return -1; }
    return m_g[goalIdx];
}
