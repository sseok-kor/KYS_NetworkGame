using System.Text;
using DataEditor.Model;
using Xunit;

namespace DataEditor.Tests;

/// <summary>
/// 엔티티 추가/삭제(CsvTable Attach/Detach) 회귀 — undo/redo 의 기반 프리미티브.
/// 핵심: Attach 는 끝에 새 dirty 줄을 만들어 엔티티에서 직렬화하고, Detach 로 되돌리면 원본 바이트 동등(기존 줄 불변).
/// </summary>
public class EntityAddRemoveTests
{
    [Fact]
    public void Attach_then_detach_round_trips_to_original_bytes()
    {
        var loader = TestPaths.LoadAll();
        var table = loader.SpawnsTable!;
        var original = table.SaveBytes();

        var e = new SpawnGroup { MapId = 0, Type = 0, X0 = 1, Y0 = 2, X1 = 3, Y1 = 4, Count = 1, RespawnDelayMs = 5000 };
        table.Attach(e);
        var withAdd = table.SaveBytes();

        Assert.True(withAdd.Length > original.Length);                    // 줄 하나 늘었음
        Assert.Contains("0, 0, 1, 2, 3, 4, 1, 5000", Encoding.UTF8.GetString(withAdd)); // 새 행 직렬화(구분자 ", ")

        table.Detach(e);
        Assert.Equal(original, table.SaveBytes());                        // 삭제 후 원본 바이트 동등(기존 줄 불변)
    }

    [Fact]
    public void Attach_and_detach_update_entity_count()
    {
        var loader = TestPaths.LoadAll();
        var table = loader.ItemsTable!;
        int baseCount = table.Entities.Count;

        var e = new ItemDef { TemplateId = 999001, Name = "새아이템", Type = 0, StackMax = 1 };
        table.Attach(e);
        Assert.Equal(baseCount + 1, table.Entities.Count);
        Assert.Contains(e, table.Entities);

        table.Detach(e);
        Assert.Equal(baseCount, table.Entities.Count);
        Assert.DoesNotContain(e, table.Entities);
    }

    // 편집한 값이 추가 행에 반영되는가(Attach 뒤 프로퍼티 변경 → 그 줄 재직렬화).
    [Fact]
    public void Edit_after_attach_serializes_new_value()
    {
        var loader = TestPaths.LoadAll();
        var table = loader.SpawnsTable!;

        var e = new SpawnGroup { MapId = 0, Type = 0, X0 = 0, Y0 = 0, X1 = 0, Y1 = 0, Count = 1, RespawnDelayMs = 5000 };
        table.Attach(e);
        e.X1 = 777; // 추가 후 편집
        Assert.Contains("0, 0, 0, 0, 777, 0, 1, 5000", Encoding.UTF8.GetString(table.SaveBytes()));
    }
}
