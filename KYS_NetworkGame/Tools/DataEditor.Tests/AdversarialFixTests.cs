using System.Text;
using DataEditor.Loader;
using DataEditor.Model;
using DataEditor.Validation;
using Xunit;

namespace DataEditor.Tests;

/// <summary>
/// fix-diff 적대 리뷰(2026-07-27)에서 CONFIRMED 된 검증-등가/로더 결함의 회귀 테스트.
/// 각 테스트는 "게임 부팅이 거부/수용하는 데이터를 에디터도 동일하게 판정" 하는지 게이트.
/// </summary>
public class AdversarialFixTests
{
    private static List<Finding> Errors(Dataset ds)
        => new ValidationEngine().Validate(ds).Where(f => f.Severity == FindingSeverity.Error).ToList();

    // F4: 빈 walkableFile 은 "-" 가 아니므로 게임이 파일로 열려다 실패 -> 에디터도 Error.
    [Fact]
    public void Empty_walkable_file_is_error()
    {
        var ds = TestPaths.LoadAll().Dataset;
        ds.Maps[1].WalkableFile = ""; // "-" 였던 것을 빈 문자열로
        Assert.Contains(Errors(ds), e => e.Category == "walkable" && e.Message.Contains("비어 있음"));
    }

    // F3: 드랍이 전부 무효(chancePermil 0)면 게임 dropRuleCount=0 -> 부팅 거부. 에디터도 "유효 0행" Error.
    [Fact]
    public void All_invalid_drops_yields_zero_valid_error()
    {
        var ds = TestPaths.LoadAll().Dataset;
        foreach (var m in ds.Monsters)
            foreach (var d in m.Drops)
                d.ChancePermil = 0; // 전부 확률 무효화(게임은 이 행들을 무시 -> 유효 0)
        Assert.Contains(Errors(ds), e => e.Category == "monsters" && e.Message.Contains("유효 드랍 규칙이 0행"));
    }

    // F7: 무효 드랍은 유효 카운트에 안 들어가므로, 무효 드랍을 잔뜩 더해도 ">256" 오거부가 없어야 함(게임은 수용).
    [Fact]
    public void Invalid_drops_do_not_count_toward_cap()
    {
        var ds = TestPaths.LoadAll().Dataset;
        var m = ds.Monsters[0];
        int validItem = ds.Items[0].TemplateId;
        for (int i = 0; i < 300; i++)
            m.Drops.Add(new DropRule { Item = validItem, ChancePermil = 0, Min = 1, Max = 1 }); // 무효(확률 0)
        Assert.DoesNotContain(Errors(ds), e => e.Message.Contains("유효 드랍 규칙") && e.Message.Contains(">"));
    }

    // F6: 아이템 128개 절단 뒤로 밀린 아이템을 참조하는 드랍은 게임 FK 실패 -> 에디터도 유령 드랍 Error.
    [Fact]
    public void Drop_referencing_truncated_item_is_ghost()
    {
        var ds = TestPaths.LoadAll().Dataset;
        const int baseId = 500000;
        for (int i = 0; i < 130; i++)
            ds.Items.Add(new ItemDef { TemplateId = baseId + i, Type = 0, StackMax = 10, Name = "t" });
        int truncatedId = baseId + 129; // 첫 128 유효분을 넘어선 아이템
        ds.Monsters[0].Drops.Add(new DropRule { Item = truncatedId, ChancePermil = 500, Min = 1, Max = 1 });
        Assert.Contains(Errors(ds), e => e.Category == "monsters" && e.Message.Contains("유령 드랍") && e.Message.Contains(truncatedId.ToString()));
    }

    // F8: 필수 스탯 필드가 파일에 없으면 게임 부팅 거부. present-key 로 누락을 잡는다.
    [Fact]
    public void Missing_required_monster_field_is_error()
    {
        var ds = TestPaths.LoadAll().Dataset;
        ds.Monsters[0].LoadedKeys.Remove("aggroRange"); // 파일에 aggroRange 줄이 없던 것으로 시뮬레이션
        Assert.Contains(Errors(ds), e => e.Category == "monsters" && e.Message.Contains("aggroRange") && e.Message.Contains("누락"));
    }

    // F1/F5: 어떤 행이 폭/50 와 길이가 다르면 게임 ParseWalkCsv 가 거부 -> 에디터도 Error.
    [Fact]
    public void Walkable_row_length_mismatch_is_error()
    {
        var ds = TestPaths.LoadAll().Dataset;
        var m = ds.Maps[0]; // map0 = walkable 있음
        int cols = m.Width / 50, rows = m.Height / 50;
        var bad = new WalkableGrid(m.WalkableFile, cols, rows);
        for (int r = 0; r < rows; r++)
            bad.LoadRow(r, new string('1', r == 5 ? cols - 1 : cols)); // 5행만 한 칸 짧게
        ds.Walkables[m.WalkableFile] = bad;
        Assert.Contains(Errors(ds), e => e.Category == "walkable" && e.Message.Contains("5행 길이"));
    }

    // F1/F5: '0'/'1' 아닌 문자가 있으면 게임 거부 -> 에디터도 Error.
    [Fact]
    public void Walkable_bad_char_is_error()
    {
        var ds = TestPaths.LoadAll().Dataset;
        var m = ds.Maps[0];
        int cols = m.Width / 50, rows = m.Height / 50;
        var bad = new WalkableGrid(m.WalkableFile, cols, rows);
        for (int r = 0; r < rows; r++)
        {
            string line = r == 3 ? new string('1', cols - 1) + "x" : new string('1', cols); // 3행 끝에 'x'
            bad.LoadRow(r, line);
        }
        ds.Walkables[m.WalkableFile] = bad;
        Assert.Contains(Errors(ds), e => e.Category == "walkable" && e.Message.Contains("'0'/'1'"));
    }

    // F2: 0행 walkable 격자는 게임이 거부하는 상태 — 검증이 크래시하지 않고 Error 를 낸다.
    [Fact]
    public void Zero_row_walkable_grid_does_not_crash_and_errors()
    {
        var ds = TestPaths.LoadAll().Dataset;
        var m = ds.Maps[0];
        ds.Walkables[m.WalkableFile] = new WalkableGrid(m.WalkableFile, 0, 0); // 0행 파일 시뮬
        var errors = Errors(ds); // 크래시하면 여기서 예외로 실패
        Assert.Contains(errors, e => e.Category == "walkable" && e.Message.Contains("행 수"));
    }
}
