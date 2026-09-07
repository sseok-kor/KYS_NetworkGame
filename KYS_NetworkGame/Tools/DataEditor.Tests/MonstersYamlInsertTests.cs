using System.Text;
using DataEditor.Loader;
using Xunit;

namespace DataEditor.Tests;

/// <summary>
/// F9 회귀: 파일에 물리적 줄이 없던 옵션 필드를 편집하면 저장 시 새 줄로 삽입돼야 한다(무음 소실 금지).
/// 동시에 미편집 저장은 여전히 바이트 동등이어야 한다(삽입 자리표시가 원문을 오염시키지 않음).
/// </summary>
public class MonstersYamlInsertTests
{
    // cap 줄이 없는(옵션 필드 생략) 최소 monsters.yml
    private const string Yml =
        "monsters:\n" +
        "  - type: 0\n" +
        "    hp: 50\n" +
        "    atkPower: 5\n" +
        "    moveSpeed: 50\n" +
        "    aggroRange: 100\n" +
        "    attackRange: 40\n" +
        "    attackCooldown: 1.0\n" +
        "    drops:\n" +
        "      - { item: 1001, chancePermil: 300, min: 1, max: 2 }\n";

    private static string Scratch(out string path)
    {
        var dir = Path.Combine(Path.GetTempPath(), "DE_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        path = Path.Combine(dir, "monsters.yml");
        File.WriteAllText(path, Yml, new UTF8Encoding(false));
        return dir;
    }

    [Fact]
    public void No_edit_save_is_byte_equal_despite_absent_field_placeholders()
    {
        var dir = Scratch(out var path);
        try
        {
            var doc = MonstersYaml.Load(path);
            Assert.Equal(File.ReadAllBytes(path), doc.SaveBytes());
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void Editing_absent_optional_field_inserts_line()
    {
        var dir = Scratch(out var path);
        try
        {
            var doc = MonstersYaml.Load(path);
            doc.Monsters[0].Cap = 5; // 파일에 cap 줄이 없었음 -> 삽입돼야 함
            var outText = Encoding.UTF8.GetString(doc.SaveBytes());
            Assert.Contains("cap: 5", outText);
            // 편집 안 한 다른 옵션 필드(name/isBoss)는 삽입되지 않아야 함
            Assert.DoesNotContain("isBoss:", outText);
        }
        finally { Directory.Delete(dir, true); }
    }

    // 드랍 추가 → 새 드랍 줄이 직렬화되고, 삭제하면 원본 바이트로 되돌아온다(기존 드랍 줄 불변).
    [Fact]
    public void AttachDrop_then_detach_round_trips_and_serializes()
    {
        var dir = Scratch(out var path);
        try
        {
            var doc = MonstersYaml.Load(path);
            var original = doc.SaveBytes();

            var drop = new DataEditor.Model.DropRule { Item = 1002, ChancePermil = 500, Min = 2, Max = 3 };
            doc.Monsters[0].Drops.Add(drop);
            doc.AttachDrop(0, drop);
            var withAdd = Encoding.UTF8.GetString(doc.SaveBytes());
            Assert.Contains("{ item: 1002, chancePermil: 500, min: 2, max: 3 }", withAdd);

            doc.Monsters[0].Drops.Remove(drop);
            doc.DetachDrop(drop);
            Assert.Equal(original, doc.SaveBytes()); // 삭제 후 원본 바이트 동등
        }
        finally { Directory.Delete(dir, true); }
    }
}
