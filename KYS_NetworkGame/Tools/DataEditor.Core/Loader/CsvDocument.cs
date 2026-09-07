using System.Text;

namespace DataEditor.Loader;

/// <summary>
/// 플랫 CSV의 round-trip 표현. 주석('#')/빈 줄은 원문 그대로 보존, 데이터 줄은 파싱된 필드(trim) 보유.
/// 저장 시 편집 안 된 줄은 원문 바이트 그대로, 편집된 줄만 재직렬화(파일별 구분자 재현).
/// → 미편집 저장이 바이트 동등(리뷰 #8: 구분자 하드코딩 대신 로드 시 감지·재현).
/// </summary>
public sealed class CsvDocument
{
    public byte[]? Bom { get; private set; }
    public string Newline { get; private set; } = "\n";
    /// <summary>감지한 필드 구분자. maps/spawns/items = ", "(콤마+공백), portals = ","(공백 없음).</summary>
    public string Separator { get; private set; } = ",";

    public sealed class Row
    {
        public string Raw = "";                         // 원본 줄(개행 제외)
        public bool IsData;                             // 주석/빈 줄이 아닌 데이터 줄
        public string[] Fields = Array.Empty<string>(); // 데이터 줄의 trim된 필드
        public bool Dirty;                              // 편집됨 → 저장 시 재직렬화
    }

    private readonly List<Row> _rows = new();
    public IReadOnlyList<Row> Rows => _rows;
    public IEnumerable<Row> DataRows => _rows.Where(r => r.IsData);

    public static CsvDocument Load(string path)
    {
        var (text, bom, newline) = TextFileIo.ReadText(path);
        var doc = new CsvDocument { Bom = bom, Newline = newline };
        var lines = text.Split(newline); // 마지막 원소는 트레일링 개행 산물(빈 줄)
        bool sepDetected = false;
        foreach (var line in lines)
        {
            var row = new Row { Raw = line };
            var head = line.TrimStart();
            if (head.Length == 0 || head.StartsWith('#'))
            {
                row.IsData = false; // 주석/빈 줄
            }
            else
            {
                row.IsData = true;
                if (!sepDetected)
                {
                    // 첫 데이터 줄에 ", "(콤마+공백)가 있으면 그 스타일, 아니면 ",".
                    doc.Separator = line.Contains(", ") ? ", " : ",";
                    sepDetected = true;
                }
                // 게임 로더처럼 콤마 split + 필드 trim(값 내부 공백은 유지·이름의 공백 보존).
                row.Fields = line.Split(',').Select(f => f.Trim()).ToArray();
            }
            doc._rows.Add(row);
        }
        return doc;
    }

    /// <summary>저장 텍스트 생성. dirty 데이터 줄만 serializeDataRow로 재직렬화, 나머지는 원문.</summary>
    public string ToText(Func<Row, string> serializeDataRow)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < _rows.Count; i++)
        {
            var r = _rows[i];
            sb.Append(r.IsData && r.Dirty ? serializeDataRow(r) : r.Raw);
            if (i < _rows.Count - 1) sb.Append(Newline);
        }
        return sb.ToString();
    }

    public byte[] ToBytes(Func<Row, string> serializeDataRow)
        => TextFileIo.ComposeBytes(ToText(serializeDataRow), Bom);

    /// <summary>새 데이터 줄을 트레일링 빈 줄 앞(마지막 데이터 줄 뒤)에 삽입하고 반환(엔티티 추가용).
    /// dirty=true 라 저장 시 엔티티에서 재직렬화된다. 미편집 줄은 그대로 → 기존 줄 바이트 불변.</summary>
    public Row AddDataRow()
    {
        var row = new Row { IsData = true, Dirty = true, Fields = Array.Empty<string>() };
        int idx = _rows.Count;
        while (idx > 0 && !_rows[idx - 1].IsData && _rows[idx - 1].Raw.Length == 0) idx--; // 끝의 빈 줄 앞에
        _rows.Insert(idx, row);
        return row;
    }

    public void RemoveRow(Row row) => _rows.Remove(row);
}
