using System.Text;

namespace DataEditor.Model;

/// <summary>
/// walkable_map*.csv 한 장의 통행 격자. 50px 셀.
/// 좌표 규약(게임 정합): row = y칸(위→아래 증가), col = x칸(왼→오른쪽 증가). cells[row, col].
/// 값 true = 통행('1'), false = 벽('0').
/// CSV가 정본. .bin은 캐시라 에디터가 쓰지 않음(게임이 mtime 불일치로 재생성·설계 §13 #11).
/// 클램프/도달성(IsReachable BFS)은 PortedGeo가 이 격자 위에서 수행.
///
/// round-trip: 셀 편집 시 그 행을 dirty로 표시 → Writer가 dirty 행만 재직렬화(나머지는 원문).
/// </summary>
public sealed class WalkableGrid
{
    public string FileName { get; }
    public int Cols { get; }          // = mapWidth / 50
    public int Rows { get; }          // = mapHeight / 50
    private readonly bool[,] _cells;  // [row, col]
    private readonly bool[] _dirtyRows;
    private readonly int[] _rowLengths;   // 로드 시 각 데이터 행의 원본 문자 수(게임 행별 길이 검증 미러용)
    private readonly bool[] _rowBadChar;  // 로드 시 그 행에 '0'/'1' 아닌 문자가 있었는가

    public WalkableGrid(string fileName, int cols, int rows)
    {
        FileName = fileName;
        Cols = cols;
        Rows = rows;
        _cells = new bool[rows, cols];
        _dirtyRows = new bool[rows];
        _rowLengths = new int[rows];
        _rowBadChar = new bool[rows];
    }

    /// <summary>격자 인덱스(범위 안 전제) 직접 조회. 범위 클램프/폐구간 처리는 PortedGeo.</summary>
    public bool GetCell(int col, int row) => _cells[row, col];

    /// <summary>셀 설정. 값이 실제로 바뀔 때만 그 행을 dirty로 표시(불필요 재직렬화 방지).</summary>
    public void SetCell(int col, int row, bool walkable)
    {
        if (_cells[row, col] == walkable) return;
        _cells[row, col] = walkable;
        _dirtyRows[row] = true;
    }

    /// <summary>로드용 벌크 행 채우기 — dirty를 찍지 않는다(원본 로드는 편집이 아니므로).
    /// 원본 길이/비-'0'/'1' 문자 유무를 기록해 ValidationEngine이 게임 ParseWalkCsv 의 행별 규칙을 미러링한다.
    /// (셀 채움은 Cols 까지만 — 초과분은 검증이 길이 불일치로 잡는다.)</summary>
    public void LoadRow(int row, string bits)
    {
        _rowLengths[row] = bits.Length;
        bool bad = false;
        for (int c = 0; c < bits.Length; c++)
        {
            char ch = bits[c];
            if (ch != '0' && ch != '1') bad = true;
            if (c < Cols) _cells[row, c] = ch == '1';
        }
        _rowBadChar[row] = bad;
    }

    /// <summary>로드 시 그 데이터 행의 원본 문자 수(길이 검증용).</summary>
    public int RowLength(int row) => _rowLengths[row];

    /// <summary>로드 시 그 행에 '0'/'1' 이 아닌 문자가 있었는가(문자셋 검증용).</summary>
    public bool RowHasBadChar(int row) => _rowBadChar[row];

    public bool IsRowDirty(int row) => _dirtyRows[row];

    /// <summary>한 행을 '0'/'1' 문자열로 직렬화(dirty 행 저장용).</summary>
    public string RowToString(int row)
    {
        var sb = new StringBuilder(Cols);
        for (int c = 0; c < Cols; c++) sb.Append(_cells[row, c] ? '1' : '0');
        return sb.ToString();
    }
}
