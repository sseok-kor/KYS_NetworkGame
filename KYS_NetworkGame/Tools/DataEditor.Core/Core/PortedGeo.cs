using DataEditor.Model;

namespace DataEditor.Core;

/// <summary>
/// 게임 C++ WalkableTable의 공간 로직을 C#으로 포팅.
/// ValidationEngine이 이 위에서 포탈/스폰 검증을 수행하므로 게임과 결과가 100% 일치해야 한다
/// (설계 §5·§13, 적대 리뷰 VERDICT: 경계·클램프·BFS는 실코드와 정확 일치 확인).
///
/// 좌표 규약: 원점 좌상단, x+ 오른쪽, y+ 아래. 모든 범위 검사는 폐구간(&lt;=)
/// (반개구간으로 짜면 게임은 통과시키는 데이터를 에디터가 오거부·리뷰 강조).
///
/// grid == null = 맵의 walkableFile이 "-" (전-통행 맵) 또는 미적재 → 전 칸 통행.
/// </summary>
public static class PortedGeo
{
    private const int Cell = EditorConstants.WalkCellSize; // 50

    /// <summary>coord/50, 음수는 0, cellCount 이상은 cellCount-1로 클램프.
    /// 맵 끝 좌표(x==width, 폐구간)는 마지막 칸(예: 4000/50=80 → 79).</summary>
    public static int CellIndexOf(int coord, int cellCount)
    {
        if (cellCount <= 0) return 0;   // 퇴화 격자(0행/0열) 방어 - 호출자도 격자 차원을 걸러야 함
        int c = coord / Cell;      // C# 정수 나눗셈은 0 방향 절단
        if (c < 0) return 0;
        if (c >= cellCount) return cellCount - 1;
        return c;
    }

    /// <summary>IsWalkable: 폐구간 [0,width]x[0,height] 밖 → false. grid null(전-통행) → true.
    /// 아니면 셀 비트 조회. (mapId 범위밖은 호출자가 걸러 여기선 grid로만 판단.)</summary>
    public static bool IsWalkable(WalkableGrid? grid, int mapWidth, int mapHeight, int x, int y)
    {
        if (x < 0 || x > mapWidth || y < 0 || y > mapHeight) return false; // 폐구간
        if (grid == null) return true;                                     // "-" 맵 = 전 칸 통행
        if (grid.Cols <= 0 || grid.Rows <= 0) return true;                 // 퇴화 격자(0행 파일) - 크래시 대신 통행 취급(차원 Error는 검증이 별도 발화)
        int col = CellIndexOf(x, grid.Cols);
        int row = CellIndexOf(y, grid.Rows);
        return grid.GetCell(col, row);
    }

    /// <summary>IsReachable: 50px 셀 4방향 BFS flood-fill.
    /// grid null(전-통행/미적재) → true 폴백(격자가 없으면 격리 개념도 없다. IsWalkable의 grid null 분기도 통행 취급).
    /// from/to 셀이 벽이면 false. (from/to는 셀로 클램프 후 셀 비트로 판단 — 게임과 동일.)</summary>
    public static bool IsReachable(WalkableGrid? grid, int fromX, int fromY, int toX, int toY)
    {
        if (grid == null) return true; // 전-통행/미적재 폴백
        if (grid.Cols <= 0 || grid.Rows <= 0) return true; // 퇴화 격자 방어

        int fromCol = CellIndexOf(fromX, grid.Cols), fromRow = CellIndexOf(fromY, grid.Rows);
        int toCol = CellIndexOf(toX, grid.Cols), toRow = CellIndexOf(toY, grid.Rows);
        if (!grid.GetCell(fromCol, fromRow) || !grid.GetCell(toCol, toRow)) return false; // 출발/도착 벽
        if (fromCol == toCol && fromRow == toRow) return true;

        var visited = new bool[grid.Rows, grid.Cols];
        var queue = new Queue<(int col, int row)>();
        visited[fromRow, fromCol] = true;
        queue.Enqueue((fromCol, fromRow));
        // 4방향(대각 없음 — 4방 도달집합 == 8방 no-corner-cut 도달집합)
        int[] dc = { 0, 0, -1, 1 };
        int[] dr = { -1, 1, 0, 0 };
        while (queue.Count > 0)
        {
            var (c, r) = queue.Dequeue();
            for (int i = 0; i < 4; i++)
            {
                int nc = c + dc[i], nr = r + dr[i];
                if (nc < 0 || nc >= grid.Cols || nr < 0 || nr >= grid.Rows) continue;
                if (visited[nr, nc] || !grid.GetCell(nc, nr)) continue;
                if (nc == toCol && nr == toRow) return true;
                visited[nr, nc] = true;
                queue.Enqueue((nc, nr));
            }
        }
        return false;
    }

    /// <summary>사각형 [x0,y0]~[x1,y1]과 점 (px,py)의 최단거리 제곱.
    /// 점이 사각 안이면 0, 아니면 축별 clamp 차이 제곱합(long). 스폰 어그로거리 검증용.</summary>
    public static long RectPointDistSq(int x0, int y0, int x1, int y1, int px, int py)
    {
        int dx = px < x0 ? x0 - px : (px > x1 ? px - x1 : 0);
        int dy = py < y0 ? y0 - py : (py > y1 ? py - y1 : 0);
        return (long)dx * dx + (long)dy * dy;
    }

    /// <summary>스폰 구역과 겹치는 각 50px 셀의 "교집합 대표점"을 열거(게임 SpawnTable 샘플과 동일).
    /// sampleX = (c*50 &lt; x0) ? x0 : c*50 (셀 시작이 구역 밖이면 구역 경계로 당김).
    /// 구역 통행성/도달성 검증이 이 점들로 IsWalkable/IsReachable 판정.</summary>
    public static IEnumerable<(int x, int y)> ZoneSamplePoints(int x0, int y0, int x1, int y1, int gridCols, int gridRows)
    {
        if (gridCols <= 0 || gridRows <= 0) yield break; // 퇴화 격자 - 샘플 없음(Math.Clamp min>max 예외 방어)
        int c0 = Math.Clamp(x0 / Cell, 0, gridCols - 1);
        int c1 = Math.Clamp(x1 / Cell, 0, gridCols - 1);
        int r0 = Math.Clamp(y0 / Cell, 0, gridRows - 1);
        int r1 = Math.Clamp(y1 / Cell, 0, gridRows - 1);
        for (int r = r0; r <= r1; r++)
        {
            for (int c = c0; c <= c1; c++)
            {
                int sx = (c * Cell < x0) ? x0 : c * Cell;
                int sy = (r * Cell < y0) ? y0 : r * Cell;
                yield return (sx, sy);
            }
        }
    }
}
