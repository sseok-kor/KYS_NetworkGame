using System.ComponentModel;

namespace DataEditor.Loader;

/// <summary>
/// CsvDocument(원문 보존) + 파싱된 엔티티 + 재직렬화 함수를 묶은 round-trip 테이블.
/// 엔티티 편집(PropertyChanged) 시 해당 줄만 dirty → 저장 시 그 줄만 재직렬화, 나머지는 원문 그대로.
/// 엔티티 추가/삭제(Attach/Detach)는 가역 프리미티브 — 삭제 후 재추가는 끝에 새 줄로(행 순서는 이 파일들에 무의미).
/// </summary>
public sealed class CsvTable<T> where T : INotifyPropertyChanged
{
    private readonly CsvDocument _doc;
    private readonly Dictionary<CsvDocument.Row, T> _rowToEntity; // Row는 참조 동일성 키
    private readonly Dictionary<T, CsvDocument.Row> _entityToRow; // 역참조(삭제/재직렬화 대상 줄 찾기)
    private readonly HashSet<T> _subscribed;                      // 엔티티당 PropertyChanged 구독 1회
    private readonly Func<T, string[]> _toFields;
    private readonly List<T> _entities;

    public IReadOnlyList<T> Entities => _entities;

    private CsvTable(CsvDocument doc, Func<T, string[]> toFields)
    {
        _doc = doc;
        _toFields = toFields;
        _rowToEntity = new Dictionary<CsvDocument.Row, T>();
        _entityToRow = new Dictionary<T, CsvDocument.Row>();
        _subscribed = new HashSet<T>();
        _entities = new List<T>();
    }

    /// <param name="parse">CSV 필드(trim) → 엔티티</param>
    /// <param name="toFields">엔티티 → CSV 필드(저장용, 구분자로 join)</param>
    public static CsvTable<T> Load(string path, Func<string[], T> parse, Func<T, string[]> toFields)
    {
        var doc = CsvDocument.Load(path);
        var table = new CsvTable<T>(doc, toFields);
        foreach (var row in doc.DataRows)
            table.Bind(parse(row.Fields), row);
        return table;
    }

    // 엔티티를 줄에 연결 + 구독(1회) + 목록 추가. 편집 시 그 시점의 줄을 dirty로(재연결돼도 현재 줄 추종).
    private void Bind(T entity, CsvDocument.Row row)
    {
        _rowToEntity[row] = entity;
        _entityToRow[entity] = row;
        if (_subscribed.Add(entity))
            entity.PropertyChanged += (_, __) => { if (_entityToRow.TryGetValue(entity, out var r)) r.Dirty = true; };
        _entities.Add(entity);
    }

    /// <summary>엔티티 추가: 끝에 새 dirty 줄을 만들어 연결(저장 시 엔티티에서 직렬화). Detach로 되돌림.</summary>
    public void Attach(T entity) => Bind(entity, _doc.AddDataRow());

    /// <summary>엔티티 삭제: 그 줄을 doc에서 제거 + 매핑/목록에서 제거. Attach로 되돌림.</summary>
    public void Detach(T entity)
    {
        if (_entityToRow.TryGetValue(entity, out var row))
        {
            _doc.RemoveRow(row);
            _rowToEntity.Remove(row);
            _entityToRow.Remove(entity);
        }
        _entities.Remove(entity);
    }

    public string SaveText() => _doc.ToText(row => string.Join(_doc.Separator, _toFields(_rowToEntity[row])));
    public byte[] SaveBytes() => _doc.ToBytes(row => string.Join(_doc.Separator, _toFields(_rowToEntity[row])));
}
