using System.Globalization;
using DataEditor.Model;

namespace DataEditor.Loader;

/// <summary>
/// Data/ 폴더의 정적 데이터를 Dataset으로 로드하고, 저장용 round-trip 테이블을 보유.
/// (대상: 플랫 CSV 4종 + monsters.yml + 각 맵이 참조하는 walkable 격자.)
/// </summary>
public sealed class DataLoader
{
    public Dataset Dataset { get; } = new();

    public CsvTable<MapData>? MapsTable { get; private set; }
    public CsvTable<SpawnGroup>? SpawnsTable { get; private set; }
    public CsvTable<Portal>? PortalsTable { get; private set; }
    public CsvTable<ItemDef>? ItemsTable { get; private set; }
    public MonstersYaml? MonstersFile { get; private set; }
    public Dictionary<string, WalkableCsv> WalkableFiles { get; } = new(StringComparer.Ordinal);

    /// <summary>전체 로드: 플랫 CSV 4종 + monsters.yml + 각 맵이 참조하는 walkable 격자.</summary>
    public void LoadAll(string dataDir)
    {
        LoadCsvTables(dataDir);

        MonstersFile = MonstersYaml.Load(Path.Combine(dataDir, "monsters.yml"));
        foreach (var m in MonstersFile.Monsters) Dataset.Monsters.Add(m);

        // walkable: maps.walkableFile("-" 아님)이 가리키는 파일을 로드(같은 파일 공유 가능·1회만).
        foreach (var map in Dataset.Maps)
        {
            if (!map.HasWalkableFile || WalkableFiles.ContainsKey(map.WalkableFile)) continue;
            var path = Path.Combine(dataDir, map.WalkableFile);
            if (!File.Exists(path)) continue; // 없으면 ValidationEngine이 Error로 잡음(리뷰 #6)
            var wc = WalkableCsv.Load(map.WalkableFile, path);
            WalkableFiles[map.WalkableFile] = wc;
            Dataset.Walkables[map.WalkableFile] = wc.Grid;
        }
    }

    public void LoadCsvTables(string dataDir)
    {
        MapsTable = CsvTable<MapData>.Load(Path.Combine(dataDir, "maps.csv"), ParseMap, MapFields);
        foreach (var m in MapsTable.Entities) Dataset.Maps.Add(m);

        SpawnsTable = CsvTable<SpawnGroup>.Load(Path.Combine(dataDir, "spawns.csv"), ParseSpawn, SpawnFields);
        foreach (var s in SpawnsTable.Entities) Dataset.Spawns.Add(s);

        PortalsTable = CsvTable<Portal>.Load(Path.Combine(dataDir, "portals.csv"), ParsePortal, PortalFields);
        foreach (var p in PortalsTable.Entities) Dataset.Portals.Add(p);

        ItemsTable = CsvTable<ItemDef>.Load(Path.Combine(dataDir, "items.csv"), ParseItem, ItemFields);
        foreach (var it in ItemsTable.Entities) Dataset.Items.Add(it);
    }

    // --- 파싱 헬퍼(관대: 파싱 실패는 0/"". 잘못된 값은 ValidationEngine이 잡음. 미편집 줄은 원문 보존이라 손상 없음) ---
    private static int I(string[] f, int i)
        => i < f.Length && int.TryParse(f[i], NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) ? v : 0;
    private static string S(string[] f, int i) => i < f.Length ? f[i] : "";
    private static string N(int v) => v.ToString(CultureInfo.InvariantCulture);

    // --- maps.csv (10 컬럼) ---
    private static MapData ParseMap(string[] f) => new()
    {
        MapId = I(f, 0), Name = S(f, 1), Type = I(f, 2), Width = I(f, 3), Height = I(f, 4),
        MonsterCap = I(f, 5), RespawnX = I(f, 6), RespawnY = I(f, 7), MusicId = I(f, 8), WalkableFile = S(f, 9),
    };
    private static string[] MapFields(MapData m) => new[]
    {
        N(m.MapId), m.Name, N(m.Type), N(m.Width), N(m.Height),
        N(m.MonsterCap), N(m.RespawnX), N(m.RespawnY), N(m.MusicId), m.WalkableFile,
    };

    // --- spawns.csv (8 컬럼) ---
    private static SpawnGroup ParseSpawn(string[] f) => new()
    {
        MapId = I(f, 0), Type = I(f, 1), X0 = I(f, 2), Y0 = I(f, 3), X1 = I(f, 4), Y1 = I(f, 5),
        Count = I(f, 6), RespawnDelayMs = I(f, 7),
    };
    private static string[] SpawnFields(SpawnGroup s) => new[]
    {
        N(s.MapId), N(s.Type), N(s.X0), N(s.Y0), N(s.X1), N(s.Y1), N(s.Count), N(s.RespawnDelayMs),
    };

    // --- portals.csv (7 컬럼) ---
    private static Portal ParsePortal(string[] f) => new()
    {
        SrcMapId = I(f, 0), TriggerX = I(f, 1), TriggerY = I(f, 2), TriggerRadius = I(f, 3),
        DstMapId = I(f, 4), DstX = I(f, 5), DstY = I(f, 6),
    };
    private static string[] PortalFields(Portal p) => new[]
    {
        N(p.SrcMapId), N(p.TriggerX), N(p.TriggerY), N(p.TriggerRadius), N(p.DstMapId), N(p.DstX), N(p.DstY),
    };

    // --- items.csv (7 컬럼) ---
    private static ItemDef ParseItem(string[] f) => new()
    {
        TemplateId = I(f, 0), Name = S(f, 1), Type = I(f, 2), AtkPower = I(f, 3),
        DefPower = I(f, 4), HpRestore = I(f, 5), StackMax = I(f, 6),
    };
    private static string[] ItemFields(ItemDef it) => new[]
    {
        N(it.TemplateId), it.Name, N(it.Type), N(it.AtkPower), N(it.DefPower), N(it.HpRestore), N(it.StackMax),
    };
}
