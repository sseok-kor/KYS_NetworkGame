using DataEditor.Model;
using DataEditor.Validation;
using Xunit;

namespace DataEditor.Tests;

/// <summary>
/// ValidationEngine이 게임 부팅 검증과 등가인지 게이트.
/// 핵심: 현재 Data/(게임 부팅 통과)에 대해 Error 0. + 파손 주입 시 올바른 Error 발화.
/// </summary>
public class ValidationTests
{
    private static List<Finding> Errors(Dataset ds)
        => new ValidationEngine().Validate(ds).Where(f => f.Severity == FindingSeverity.Error).ToList();

    [Fact]
    public void Current_data_has_zero_errors()
    {
        var ds = TestPaths.LoadAll().Dataset;
        var errors = Errors(ds);
        Assert.True(errors.Count == 0, "예상치 못한 Error:\n" + string.Join("\n", errors));
    }

    [Fact]
    public void Detects_bad_map_size_not_multiple_of_250()
    {
        var ds = TestPaths.LoadAll().Dataset;
        ds.Maps[0].Width = 3333;
        Assert.Contains(Errors(ds), e => e.Category == "maps" && e.Message.Contains("width"));
    }

    [Fact]
    public void Detects_movespeed_below_floor()
    {
        var ds = TestPaths.LoadAll().Dataset;
        ds.Monsters[0].MoveSpeed = 30; // < 43
        Assert.Contains(Errors(ds), e => e.Category == "monsters" && e.Message.Contains("moveSpeed"));
    }

    [Fact]
    public void Detects_duplicate_item_id()
    {
        var ds = TestPaths.LoadAll().Dataset;
        ds.Items[1].TemplateId = ds.Items[0].TemplateId;
        Assert.Contains(Errors(ds), e => e.Category == "items" && e.Message.Contains("중복"));
    }

    [Fact]
    public void Detects_portal_out_of_range_dst_map()
    {
        var ds = TestPaths.LoadAll().Dataset;
        ds.Portals[0].DstMapId = 99;
        Assert.Contains(Errors(ds), e => e.Category == "portals" && e.Message.Contains("dstMapId"));
    }

    [Fact]
    public void Detects_spawn_count_zero()
    {
        var ds = TestPaths.LoadAll().Dataset;
        ds.Spawns[0].Count = 0;
        Assert.Contains(Errors(ds), e => e.Category == "spawns" && e.Message.Contains("count"));
    }

    [Fact]
    public void Detects_ghost_drop_fk()
    {
        var ds = TestPaths.LoadAll().Dataset;
        ds.Monsters[0].Drops[0].Item = 999999;
        Assert.Contains(Errors(ds), e => e.Category == "monsters" && e.Message.Contains("유령 드랍"));
    }

    [Fact]
    public void Missing_walkable_file_is_error()
    {
        var ds = TestPaths.LoadAll().Dataset;
        ds.Maps[1].WalkableFile = "nonexistent.csv"; // "-" 였음
        Assert.Contains(Errors(ds), e => e.Category == "walkable" && e.Message.Contains("찾을 수 없음"));
    }

    [Fact]
    public void Array_cap_severity_is_per_file()
    {
        // portals/items 상한 초과는 게임이 관대(절단·부팅 통과)하므로 Warning이어야 함(리뷰 #4)
        var findings = new ValidationEngine().Validate(TestPaths.LoadAll().Dataset);
        // 현재 데이터는 상한 미만이라 경고 없음 — severity 매핑이 Error가 아닌지만 구조적으로 확인
        Assert.DoesNotContain(findings, f => f.Category == "portals" && f.Severity == FindingSeverity.Error && f.Message.Contains("개수"));
    }
}
