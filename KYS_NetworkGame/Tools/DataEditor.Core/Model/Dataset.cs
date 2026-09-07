using System.Collections.ObjectModel;

namespace DataEditor.Model;

/// <summary>
/// 편집 세션 전체 데이터. ValidationEngine이 교차검증할 때 이 스냅샷을 훑는다.
/// Walkables는 파일명(maps.walkableFile) → 격자 맵(여러 맵이 같은 파일을 공유할 수 있으므로 파일명 키).
/// round-trip 원본 보존(주석 블록·구분자 스타일·개행·BOM)은 Loader/Writer 계층의 파일별 메타가 담당(모델은 순수 도메인).
/// </summary>
public sealed class Dataset
{
    public ObservableCollection<MapData> Maps { get; } = new();
    public ObservableCollection<SpawnGroup> Spawns { get; } = new();
    public ObservableCollection<Portal> Portals { get; } = new();
    public ObservableCollection<MonsterTemplate> Monsters { get; } = new();
    public ObservableCollection<ItemDef> Items { get; } = new();

    /// <summary>파일명 → 통행 격자.</summary>
    public Dictionary<string, WalkableGrid> Walkables { get; } = new(StringComparer.Ordinal);

    public MapData? FindMap(int mapId) => mapId >= 0 && mapId < Maps.Count ? Maps[mapId] : null;
    public ItemDef? FindItem(int templateId)
    {
        foreach (var it in Items)
            if (it.TemplateId == templateId) return it;
        return null;
    }
    public MonsterTemplate? FindMonster(int type)
    {
        foreach (var m in Monsters)
            if (m.Type == type) return m;
        return null;
    }
}
