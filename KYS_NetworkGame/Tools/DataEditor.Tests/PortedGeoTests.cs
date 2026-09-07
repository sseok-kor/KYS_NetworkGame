using DataEditor.Core;
using DataEditor.Model;
using Xunit;

namespace DataEditor.Tests;

/// <summary>
/// PortedGeo가 게임 C++ WalkableTable과 동일한 결과를 내는지 게이트.
/// 각 케이스는 조사에서 확인된 C++ 시맨틱스(폐구간 클램프·4방 BFS·폴백 비대칭)를 인코딩.
/// </summary>
public class PortedGeoTests
{
    // rows[0] = 맨 위 행(y=0..49). '1'=통행, '0'=벽. cells[row,col].
    private static WalkableGrid MakeGrid(params string[] rows)
    {
        int r = rows.Length, c = rows[0].Length;
        var g = new WalkableGrid("test", c, r);
        for (int row = 0; row < r; row++)
            for (int col = 0; col < c; col++)
                g.SetCell(col, row, rows[row][col] == '1');
        return g;
    }

    [Theory]
    [InlineData(0, 80, 0)]
    [InlineData(49, 80, 0)]
    [InlineData(50, 80, 1)]
    [InlineData(3999, 80, 79)]
    [InlineData(4000, 80, 79)]   // x==width(폐구간) → 마지막 칸으로 클램프
    [InlineData(-1, 80, 0)]      // 음수 클램프
    public void CellIndexOf_clamps_like_game(int coord, int cellCount, int expected)
        => Assert.Equal(expected, PortedGeo.CellIndexOf(coord, cellCount));

    [Fact]
    public void IsWalkable_nullGrid_is_all_walkable_within_closed_bounds()
    {
        // "-" 맵(grid null) = 전 칸 통행. 폐구간이라 x==width도 true, x>width만 false.
        Assert.True(PortedGeo.IsWalkable(null, 4000, 4000, 2000, 2000));
        Assert.True(PortedGeo.IsWalkable(null, 4000, 4000, 4000, 4000)); // 폐구간 경계
        Assert.False(PortedGeo.IsWalkable(null, 4000, 4000, 4001, 2000)); // OOB
        Assert.False(PortedGeo.IsWalkable(null, 4000, 4000, -1, 2000));
    }

    [Fact]
    public void IsWalkable_reads_cell_bit_with_closed_interval_last_cell()
    {
        var g = MakeGrid("110"); // col0,col1 통행 / col2 벽. 3칸 → mapWidth=150, mapHeight=50
        Assert.True(PortedGeo.IsWalkable(g, 150, 50, 25, 25));   // col0 통행
        Assert.False(PortedGeo.IsWalkable(g, 150, 50, 125, 25)); // col2 벽
        Assert.False(PortedGeo.IsWalkable(g, 150, 50, 150, 25)); // x==width → 마지막칸(col2, 벽)
    }

    [Fact]
    public void IsReachable_nullGrid_true_fallback()
        => Assert.True(PortedGeo.IsReachable(null, 0, 0, 4000, 4000)); // 전-통행/미적재 → true(비대칭)

    [Fact]
    public void IsReachable_connected_true()
    {
        var g = MakeGrid("111"); // 전부 통행 1행
        Assert.True(PortedGeo.IsReachable(g, 25, 25, 125, 25)); // col0 -> col2
    }

    [Fact]
    public void IsReachable_isolated_pocket_false()
    {
        var g = MakeGrid("10101"); // col0통행 | col1벽 | col2통행(격리) | col3벽 | col4통행
        // col0(x25) -> col2(x125): 사이 벽으로 4방 통과 불가 → 도달 불가
        Assert.False(PortedGeo.IsReachable(g, 25, 25, 125, 25));
    }

    [Fact]
    public void IsReachable_from_or_to_wall_false()
    {
        var g = MakeGrid("010"); // col1만 통행
        Assert.False(PortedGeo.IsReachable(g, 25, 25, 75, 25)); // from(col0)이 벽
        Assert.False(PortedGeo.IsReachable(g, 75, 25, 125, 25)); // to(col2)가 벽
    }

    [Fact]
    public void IsReachable_around_wall_true()
    {
        // 벽을 우회해 도달 가능한 경우(4방 우회로 존재)
        var g = MakeGrid(
            "111",
            "101",  // 중앙 벽
            "111");
        // (col0,row0) -> (col2,row2): 중앙 벽을 둘레로 우회 → 도달
        Assert.True(PortedGeo.IsReachable(g, 25, 25, 125, 125));
    }

    [Theory]
    [InlineData(100, 100, 200, 200, 150, 150, 0L)]     // 점이 사각 안 → 0
    [InlineData(100, 100, 200, 200, 50, 150, 2500L)]   // 왼쪽 50px → 50^2
    [InlineData(100, 100, 200, 200, 250, 250, 5000L)]  // 우하 대각 (50,50) → 50^2+50^2
    public void RectPointDistSq_matches(int x0, int y0, int x1, int y1, int px, int py, long expected)
        => Assert.Equal(expected, PortedGeo.RectPointDistSq(x0, y0, x1, y1, px, py));

    [Fact]
    public void ZoneSamplePoints_pulls_cell_start_to_zone_edge()
    {
        // 구역 [60,60]~[140,140]는 col1..2, row1..2와 겹침. 각 셀 대표점이 구역 경계로 당겨지는지.
        var pts = PortedGeo.ZoneSamplePoints(60, 60, 140, 140, 10, 10).ToList();
        Assert.Equal(4, pts.Count);            // 2x2 셀
        Assert.Contains((60, 60), pts);        // col1(50<60→60), row1(50<60→60)
        Assert.Contains((100, 100), pts);      // col2(100), row2(100)
    }
}
