using System.Text;
using DataEditor.Loader;
using Xunit;

namespace DataEditor.Tests;

/// <summary>
/// round-trip 바이트 동등 게이트: 실제 Data/ CSV를 로드→무편집 저장하면 원본과 바이트 동등해야 한다
/// (주석 블록·파일별 구분자·개행·BOM 보존·리뷰 #8/#18). 편집 시 그 줄만 바뀌는지도 확인.
/// </summary>
public class RoundTripTests
{
    private static string FindDataDir()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            var probe = Path.Combine(dir.FullName, "Data", "maps.csv");
            if (File.Exists(probe)) return Path.Combine(dir.FullName, "Data");
            dir = dir.Parent;
        }
        throw new DirectoryNotFoundException("리포 Data/ 폴더를 찾지 못함");
    }

    private static DataLoader LoadAll()
    {
        var loader = new DataLoader();
        loader.LoadCsvTables(FindDataDir());
        return loader;
    }

    [Fact]
    public void All_flat_csv_round_trip_is_byte_equal()
    {
        var dir = FindDataDir();
        var loader = LoadAll();
        AssertByteEqual(dir, "maps.csv", loader.MapsTable!.SaveBytes());
        AssertByteEqual(dir, "spawns.csv", loader.SpawnsTable!.SaveBytes());
        AssertByteEqual(dir, "portals.csv", loader.PortalsTable!.SaveBytes());
        AssertByteEqual(dir, "items.csv", loader.ItemsTable!.SaveBytes());
    }

    private static void AssertByteEqual(string dir, string file, byte[] saved)
    {
        var original = File.ReadAllBytes(Path.Combine(dir, file));
        Assert.True(original.AsSpan().SequenceEqual(saved),
            $"{file}: round-trip 바이트 불일치 (원본 {original.Length}B, 저장 {saved.Length}B)");
    }

    [Fact]
    public void Loads_expected_counts_and_values()
    {
        var loader = LoadAll();
        Assert.Equal(9, loader.Dataset.Maps.Count);
        Assert.Equal("초보 사냥터", loader.Dataset.Maps[0].Name);
        Assert.Equal(4000, loader.Dataset.Maps[0].Width);
        Assert.Equal(30, loader.Dataset.Maps[0].MonsterCap);
        Assert.Equal("walkable_map0.csv", loader.Dataset.Maps[0].WalkableFile);
        Assert.Equal("-", loader.Dataset.Maps[1].WalkableFile);
        Assert.Equal(26, loader.Dataset.Spawns.Count);
        Assert.Equal(16, loader.Dataset.Portals.Count);
        Assert.Equal(11, loader.Dataset.Items.Count);
        Assert.Equal(1001, loader.Dataset.Items[0].TemplateId);
    }

    [Fact]
    public void Edit_reserializes_only_changed_line_preserving_style()
    {
        var loader = LoadAll();
        loader.Dataset.Maps[0].MonsterCap = 50; // 30 -> 50
        var saved = Encoding.UTF8.GetString(loader.MapsTable!.SaveBytes());
        // 편집된 줄은 ", " 스타일 그대로 값만 바뀜
        Assert.Contains("0, 초보 사냥터, 1, 4000, 4000, 50, 700, 700, 0, walkable_map0.csv", saved);
        // 편집 안 한 다른 줄은 원문 유지
        Assert.Contains("1, 중급 사냥터, 1, 4000, 4000, 24, 3300, 700, 0, -", saved);
        // 주석 헤더도 유지
        Assert.Contains("# 맵 메타 표", saved);
    }

    [Fact]
    public void Walkable_round_trip_is_byte_equal()
    {
        var dir = FindDataDir();
        var path = Path.Combine(dir, "walkable_map0.csv");
        var wc = WalkableCsv.Load("walkable_map0.csv", path);
        AssertByteEqual(dir, "walkable_map0.csv", wc.SaveBytes());
        Assert.Equal(80, wc.Grid.Cols);
        Assert.Equal(80, wc.Grid.Rows);
    }

    [Fact]
    public void Monsters_round_trip_is_byte_equal_and_parses()
    {
        var dir = FindDataDir();
        var my = MonstersYaml.Load(Path.Combine(dir, "monsters.yml"));
        AssertByteEqual(dir, "monsters.yml", my.SaveBytes());
        Assert.Equal(9, my.Monsters.Count);
        var goblin = my.Monsters.First(m => m.Type == 0);
        Assert.Equal(50, goblin.Hp);
        Assert.Equal(200, goblin.MoveSpeed);
        Assert.Equal(1.0f, goblin.AttackCooldown);
        Assert.Equal(2, goblin.Drops.Count);
        Assert.Equal(1001, goblin.Drops[0].Item);
        Assert.Equal(300, goblin.Drops[0].ChancePermil);
    }

    [Fact]
    public void LoadAll_populates_full_dataset()
    {
        var loader = new DataLoader();
        loader.LoadAll(FindDataDir());
        Assert.Equal(9, loader.Dataset.Maps.Count);
        Assert.Equal(9, loader.Dataset.Monsters.Count);
        Assert.Equal(11, loader.Dataset.Items.Count);
        // map0만 walkable 파일 참조(나머지 "-")
        Assert.Single(loader.Dataset.Walkables);
        Assert.True(loader.Dataset.Walkables.ContainsKey("walkable_map0.csv"));
    }

    [Fact]
    public void Monster_field_edit_patches_only_that_line()
    {
        var dir = FindDataDir();
        var my = MonstersYaml.Load(Path.Combine(dir, "monsters.yml"));
        var goblin = my.Monsters.First(m => m.Type == 0);
        goblin.Hp = 55; // 50 -> 55
        var saved = Encoding.UTF8.GetString(my.SaveBytes());

        Assert.Contains("    hp: 55", saved);
        Assert.Contains("    attackCooldown: 1.0", saved); // float 포맷 "1.0" 보존
        Assert.Contains("# 몬스터 사전", saved);            // 주석 헤더 보존
        // 정확히 한 줄만 바뀜
        Assert.Equal(1, CountDiffLines(File.ReadAllText(Path.Combine(dir, "monsters.yml")), saved));
    }

    [Fact]
    public void Drop_edit_patches_only_that_drop_line()
    {
        var dir = FindDataDir();
        var my = MonstersYaml.Load(Path.Combine(dir, "monsters.yml"));
        var goblin = my.Monsters.First(m => m.Type == 0);
        goblin.Drops[0].ChancePermil = 999; // 300 -> 999
        var saved = Encoding.UTF8.GetString(my.SaveBytes());

        Assert.Contains("{ item: 1001, chancePermil: 999, min: 1, max: 2 }", saved);
        Assert.Equal(1, CountDiffLines(File.ReadAllText(Path.Combine(dir, "monsters.yml")), saved));
    }

    private static int CountDiffLines(string original, string saved)
    {
        var a = original.Split('\n');
        var b = saved.Split('\n');
        int n = Math.Max(a.Length, b.Length), diff = 0;
        for (int i = 0; i < n; i++)
        {
            var la = i < a.Length ? a[i] : null;
            var lb = i < b.Length ? b[i] : null;
            if (la != lb) diff++;
        }
        return diff;
    }
}
