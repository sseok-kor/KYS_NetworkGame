#include "pch_tests.h"

#include "Pathfinding/PathFinder.h"        // PathFinder / IGridSource / PATH_* 상수

#include <string>
#include <vector>
#include <random>              // mt19937 (시드 고정 - Tests 는 C++17 유틸 허용)

// ==========================================================================
// 순수 알고리즘 유닛테스트 (결정 8=A·최초 순수 알고리즘 테스트).
//   IGridSource 인라인 주입으로 Data 실파일/전역 WalkableTable 상태 없이 A*/JPS 만 채점한다.
//   핵심 = A* <-> JPS 최단 비용 동일성(⑥): 서로 다른 코드 경로가 같은 최단을 내야 JPS 정확.
// ==========================================================================

namespace
{
    // 문자 격자 격자소스: '1'=통행 / '0'=벽. mapId 는 무시(단일 테스트 맵).
    class TestGrid : public IGridSource
    {
    public:
        explicit TestGrid(const std::vector<std::string>& rows) : m_rows(rows) {}

        bool CellWalkable(int, int col, int row) const override
        {
            if (row < 0 || row >= static_cast<int>(m_rows.size())) { return false; }
            if (col < 0 || col >= static_cast<int>(m_rows[row].size())) { return false; }
            return m_rows[row][col] == '1';
        }
        int Cols(int) const override { return m_rows.empty() ? 0 : static_cast<int>(m_rows[0].size()); }
        int Rows(int) const override { return static_cast<int>(m_rows.size()); }

    private:
        std::vector<std::string> m_rows;
    };

    // 칸 (col,row) -> 그 칸 중심 픽셀(PathFinder 가 픽셀을 칸으로 환산하므로 중심을 준다).
    Position CellPos(int col, int row)
    {
        Position p;
        p.x = col * WALK_CELL_SIZE + WALK_CELL_SIZE / 2;
        p.y = row * WALK_CELL_SIZE + WALK_CELL_SIZE / 2;
        return p;
    }

    // waypoint(셀 중심 픽셀) -> 칸 좌표
    int WpCol(const Position& p) { return (p.x - WALK_CELL_SIZE / 2) / WALK_CELL_SIZE; }
    int WpRow(const Position& p) { return (p.y - WALK_CELL_SIZE / 2) / WALK_CELL_SIZE; }

    int SignI(int v) { return (v > 0) - (v < 0); }

    // 뱀 모양(serpentine) 회랑 - 1칸 폭 외길. 노드 예산/버퍼 초과 테스트용.
    //   짝수 행 = 전 칸 통행, 홀수 행 = 한쪽 끝에만 연결 칸(좌/우 교대)로 위아래 행을 잇는다.
    std::vector<std::string> MakeSerpentine(int cols, int rows)
    {
        std::vector<std::string> g(rows, std::string(cols, '0'));
        for (int r = 0; r < rows; ++r)
        {
            if (r % 2 == 0)
            {
                for (int c = 0; c < cols; ++c) { g[r][c] = '1'; }
            }
            else
            {
                if ((r / 2) % 2 == 0) { g[r][cols - 1] = '1'; }   // 우측 연결
                else                  { g[r][0] = '1'; }          // 좌측 연결
            }
        }
        return g;
    }
}

// -------------------------------------------------------------------------
// (1) 고정 격자 최단 비용 일치 - 벽 없는 격자의 알려진 octile 최단.
// -------------------------------------------------------------------------
TEST(PathFinderTest, OpenGridShortestCostMatchesKnownValue)
{
    TestGrid grid(std::vector<std::string>(8, std::string(8, '1')));   // 8x8 전 통행
    PathFinder pf(&grid);

    // (0,0) -> (5,3): 대각 3 + 직교 2 = 3*14 + 2*10 = 62
    const long long expected = 3 * PATH_COST_DIAG + 2 * PATH_COST_ORTHO;
    EXPECT_EQ(pf.AStarCost(0, CellPos(0, 0), CellPos(5, 3)), expected);
    EXPECT_EQ(pf.JpsCost(0, CellPos(0, 0), CellPos(5, 3)), expected);
}

// -------------------------------------------------------------------------
// (2) 벽 차단 / NOPATH - 목적지가 벽 / 목적지 완전 봉쇄.
// -------------------------------------------------------------------------
TEST(PathFinderTest, BlockedGoalIsNoPath)
{
    // 가운데 목적지가 벽
    std::vector<std::string> rows = {
        "11111",
        "11111",
        "11011",
        "11111",
        "11111",
    };
    TestGrid grid(rows);
    PathFinder pf(&grid);

    EXPECT_EQ(pf.AStarCost(0, CellPos(0, 0), CellPos(2, 2)), -1);
    EXPECT_EQ(pf.JpsCost(0, CellPos(0, 0), CellPos(2, 2)), -1);

    Position wp[PATH_WAYPOINT_MAX];
    EXPECT_EQ(pf.FindPath(0, CellPos(0, 0), CellPos(2, 2), wp, PATH_WAYPOINT_MAX), 0);
}

TEST(PathFinderTest, WalledOffGoalIsNoPath)
{
    // 목적지(4,4)를 벽으로 둘러 도달 불가
    std::vector<std::string> rows = {
        "11111",
        "11111",
        "11111",
        "11100",
        "11101",
    };
    TestGrid grid(rows);
    PathFinder pf(&grid);
    EXPECT_EQ(pf.AStarCost(0, CellPos(0, 0), CellPos(4, 4)), -1);
    EXPECT_EQ(pf.JpsCost(0, CellPos(0, 0), CellPos(4, 4)), -1);
}

// -------------------------------------------------------------------------
// (3) 대각 코너 규칙 (strict no-corner-cut) - D-M7 결정적 픽스처.
// -------------------------------------------------------------------------
TEST(PathFinderTest, DiagonalCornerCutIsForbidden)
{
    // 3x3 가운데 벽: 모든 대각이 중앙 벽 코너를 스치므로 대각 이동 전무 -> 순수 직교 우회.
    //   (0,0)->(2,2) = 직교 4칸 = 40 이 유일한 최단.
    //   코너컷을 허용하는 변형이라면 (0,0)->(1,0) 직교 10 + (1,0)->(2,1) 대각 14 + (2,1)->(2,2) 직교 10 = 34 로 싸진다.
    //   즉 비용이 정확히 40 인 것이 no-corner-cut 증거 - 코너컷이 열리면 34 가 나와 아래 EXPECT_EQ 가 깨진다.
    //   [주의] 대각 2회 = 28 은 어떤 변형에서도 도달 불가 - 그 경로는 벽인 (1,1)을 밟아야 하는데
    //   대상 칸 통행 검사는 대각 여부와 무관하게 항상 하기 때문이다. 코너컷 허용 = "양옆 직교 칸 검사 생략"일 뿐이다.
    std::vector<std::string> rows = {
        "111",
        "101",
        "111",
    };
    TestGrid grid(rows);
    PathFinder pf(&grid);

    const long long expected = 4 * PATH_COST_ORTHO;   // 40
    EXPECT_EQ(pf.AStarCost(0, CellPos(0, 0), CellPos(2, 2)), expected);
    EXPECT_EQ(pf.JpsCost(0, CellPos(0, 0), CellPos(2, 2)), expected);
}

TEST(PathFinderTest, DiagonalPinchIsNoPath)
{
    // 2x2 대각 핀치: (0,0)과 (1,1)만 통행, 두 직교 칸이 벽 -> 대각 진입 불가 = NOPATH.
    std::vector<std::string> rows = {
        "10",
        "01",
    };
    TestGrid grid(rows);
    PathFinder pf(&grid);
    EXPECT_EQ(pf.AStarCost(0, CellPos(0, 0), CellPos(1, 1)), -1);
    EXPECT_EQ(pf.JpsCost(0, CellPos(0, 0), CellPos(1, 1)), -1);
}

// -------------------------------------------------------------------------
// (4) 노드 예산 초과 = NOPATH - 예산(2048)보다 긴 외길 회랑.
//   A* 는 셀당 확장이라 회랑 길이(~3200 > 2048)에서 예산 컷 -> -1.
//   (JPS 는 코너에서만 확장해 예산에 안 걸리지만, ⑥ 동일성이 JPS<=A* 확장 불변으로 예산 거동을 가둔다.)
// -------------------------------------------------------------------------
TEST(PathFinderTest, NodeBudgetExceededIsNoPath)
{
    TestGrid grid(MakeSerpentine(80, 80));   // 회랑 길이 ~3240 > PATH_NODE_BUDGET(2048)
    PathFinder pf(&grid);
    // (0,0) 시작, (0,78) 목적지 - 뱀 회랑 반대 끝(유일 경로가 예산 초과 길이).
    EXPECT_EQ(pf.AStarCost(0, CellPos(0, 0), CellPos(0, 78)), -1);
}

// -------------------------------------------------------------------------
// (5) 버퍼 초과 = 부분 경로 - 코너 여럿인 경로에 작은 maxWaypoints.
// -------------------------------------------------------------------------
TEST(PathFinderTest, WaypointBufferOverflowReturnsPartial)
{
    TestGrid grid(MakeSerpentine(6, 6));   // 코너 여럿(각 회랑 전환점)
    PathFinder pf(&grid);

    Position full[PATH_WAYPOINT_MAX];
    const int fullCount = pf.FindPath(0, CellPos(0, 0), CellPos(5, 4), full, PATH_WAYPOINT_MAX);
    ASSERT_GT(fullCount, 2);   // 코너 3개 이상인 경로여야 부분 경로 테스트가 의미 있음

    Position partial[2];
    const int partialCount = pf.FindPath(0, CellPos(0, 0), CellPos(5, 4), partial, 2);
    EXPECT_EQ(partialCount, 2);   // 버퍼 상한만큼만(앞 2점)
    // 부분 경로 앞 2점 = 전체 경로 앞 2점과 동일해야(같은 탐색·앞에서 자름)
    EXPECT_EQ(WpCol(partial[0]), WpCol(full[0]));
    EXPECT_EQ(WpRow(partial[0]), WpRow(full[0]));
    EXPECT_EQ(WpCol(partial[1]), WpCol(full[1]));
    EXPECT_EQ(WpRow(partial[1]), WpRow(full[1]));
}

// -------------------------------------------------------------------------
// (6) A* <-> JPS 경로 비용 동일성 - 시드 고정 무작위 격자 N개(JPS 정확성의 수학 채점).
// -------------------------------------------------------------------------
TEST(PathFinderTest, JpsCostEqualsAStarCostOnRandomGrids)
{
    std::mt19937 rng(12345u);   // 시드 고정 = 재현 가능
    const int GRID = 16;
    const int TRIALS = 300;

    for (int t = 0; t < TRIALS; ++t)
    {
        // ~28% 벽 무작위 격자
        std::vector<std::string> rows(GRID, std::string(GRID, '1'));
        for (int r = 0; r < GRID; ++r)
        {
            for (int c = 0; c < GRID; ++c)
            {
                if ((rng() % 100) < 28) { rows[r][c] = '0'; }
            }
        }
        TestGrid grid(rows);
        PathFinder pf(&grid);

        // 통행 칸 중 무작위 start/goal (몇 번 시도해 못 찾으면 이 격자 skip)
        auto pickWalkable = [&](int& col, int& row) -> bool {
            for (int tries = 0; tries < 40; ++tries)
            {
                const int c = rng() % GRID;
                const int r = rng() % GRID;
                if (rows[r][c] == '1') { col = c; row = r; return true; }
            }
            return false;
        };
        int sc, sr, gc, gr;
        if (!pickWalkable(sc, sr) || !pickWalkable(gc, gr)) { continue; }

        const long long aCost = pf.AStarCost(0, CellPos(sc, sr), CellPos(gc, gr));
        const long long jCost = pf.JpsCost(0, CellPos(sc, sr), CellPos(gc, gr));
        EXPECT_EQ(aCost, jCost) << "trial=" << t << " start=(" << sc << "," << sr
                                << ") goal=(" << gc << "," << gr << ")";
    }
}

// -------------------------------------------------------------------------
// (7) JPS leg 순수 방향성 불변식 - 반환 경로의 인접 waypoint 쌍이 순수 직교/45도 대각(8방 추종 가능)
//     + 출발->첫 waypoint 도 동일 + 마지막 waypoint = 목적지 칸(끝점 보존).
// -------------------------------------------------------------------------
TEST(PathFinderTest, WaypointLegsArePureEightWayDirections)
{
    std::mt19937 rng(6789u);
    const int GRID = 16;
    const int TRIALS = 300;
    int pathsChecked = 0;

    for (int t = 0; t < TRIALS; ++t)
    {
        std::vector<std::string> rows(GRID, std::string(GRID, '1'));
        for (int r = 0; r < GRID; ++r)
        {
            for (int c = 0; c < GRID; ++c)
            {
                if ((rng() % 100) < 25) { rows[r][c] = '0'; }
            }
        }
        rows[0][0] = '1';
        rows[GRID - 1][GRID - 1] = '1';
        TestGrid grid(rows);
        PathFinder pf(&grid);

        Position wp[PATH_WAYPOINT_MAX];
        const int n = pf.FindPath(0, CellPos(0, 0), CellPos(GRID - 1, GRID - 1), wp, PATH_WAYPOINT_MAX);
        if (n == 0) { continue; }   // 이 격자는 도달 불가 - skip
        ++pathsChecked;

        // start(0,0) -> 첫 waypoint, 그리고 waypoint 간 각 leg 방향성 검사
        int prevC = 0, prevR = 0;
        for (int i = 0; i < n; ++i)
        {
            const int wc = WpCol(wp[i]);
            const int wr = WpRow(wp[i]);
            const int dc = wc - prevC;
            const int dr = wr - prevR;
            const bool pureLeg = (dc == 0) || (dr == 0) ||
                                 (SignI(dc) * dc == SignI(dr) * dr);   // |dc| == |dr| (45도 대각)
            EXPECT_TRUE(pureLeg) << "trial=" << t << " leg " << i
                                 << " dc=" << dc << " dr=" << dr;
            prevC = wc;
            prevR = wr;
        }
        // 끝점 보존: 마지막 waypoint = 목적지 칸
        EXPECT_EQ(WpCol(wp[n - 1]), GRID - 1);
        EXPECT_EQ(WpRow(wp[n - 1]), GRID - 1);
    }
    EXPECT_GT(pathsChecked, 0);   // 최소 몇 개는 도달 가능해야 테스트가 의미 있음
}
