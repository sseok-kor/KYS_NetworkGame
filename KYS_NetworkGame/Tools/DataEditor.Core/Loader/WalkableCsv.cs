using System.Text;
using DataEditor.Model;

namespace DataEditor.Loader;

/// <summary>
/// walkable_map*.csv('0'/'1' 문자 격자)의 round-trip 로더.
/// 주석/빈 줄은 원문 보존, 격자 데이터 행은 편집 시만 재직렬화(dirty 행) → 미편집 저장 바이트 동등.
/// 차원: rows = 데이터 행 수, cols = 첫 데이터 행 길이(맵 크기/50과 일치해야 함·검증은 ValidationEngine).
/// </summary>
public sealed class WalkableCsv
{
    private readonly byte[]? _bom;
    private readonly string _newline;
    private readonly List<(string raw, int gridRow)> _lines; // gridRow: 데이터 행 인덱스, 주석/빈 줄은 -1

    public WalkableGrid Grid { get; }

    private WalkableCsv(byte[]? bom, string newline, List<(string, int)> lines, WalkableGrid grid)
    {
        _bom = bom;
        _newline = newline;
        _lines = lines;
        Grid = grid;
    }

    public static WalkableCsv Load(string fileName, string path)
    {
        var (text, bom, newline) = TextFileIo.ReadText(path);
        var rawLines = text.Split(newline);

        // 1차: 데이터 행 수집(차원 파악)
        var dataLines = new List<string>();
        foreach (var line in rawLines)
        {
            var head = line.TrimStart();
            if (head.Length == 0 || head.StartsWith('#')) continue;
            dataLines.Add(line.Trim());
        }
        int rows = dataLines.Count;
        int cols = rows > 0 ? dataLines[0].Length : 0;

        var grid = new WalkableGrid(fileName, cols, rows);
        for (int r = 0; r < rows; r++) grid.LoadRow(r, dataLines[r]); // dirty 안 찍음

        // 2차: 줄 순서 + 데이터 행 인덱스 매핑(원문 보존용)
        var lines = new List<(string, int)>();
        int gr = 0;
        foreach (var line in rawLines)
        {
            var head = line.TrimStart();
            if (head.Length == 0 || head.StartsWith('#')) lines.Add((line, -1));
            else lines.Add((line, gr++));
        }

        return new WalkableCsv(bom, newline, lines, grid);
    }

    public byte[] SaveBytes()
    {
        var sb = new StringBuilder();
        for (int i = 0; i < _lines.Count; i++)
        {
            var (raw, gridRow) = _lines[i];
            sb.Append(gridRow >= 0 && Grid.IsRowDirty(gridRow) ? Grid.RowToString(gridRow) : raw);
            if (i < _lines.Count - 1) sb.Append(_newline);
        }
        return TextFileIo.ComposeBytes(sb.ToString(), _bom);
    }
}
