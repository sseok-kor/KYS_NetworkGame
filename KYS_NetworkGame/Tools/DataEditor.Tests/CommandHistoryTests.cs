using DataEditor.Edit;
using Xunit;

namespace DataEditor.Tests;

public class CommandHistoryTests
{
    [Fact]
    public void Do_undo_redo_roundtrip()
    {
        int x = 0;
        var h = new CommandHistory();
        h.Do(new DelegateEditAction(() => x = 5, () => x = 0));
        Assert.Equal(5, x);
        Assert.True(h.CanUndo);
        Assert.False(h.CanRedo);

        h.Undo();
        Assert.Equal(0, x);
        Assert.True(h.CanRedo);

        h.Redo();
        Assert.Equal(5, x);
    }

    [Fact]
    public void New_action_clears_redo()
    {
        int x = 0;
        var h = new CommandHistory();
        h.Do(new DelegateEditAction(() => x = 1, () => x = 0));
        h.Undo();
        Assert.True(h.CanRedo);
        h.Do(new DelegateEditAction(() => x = 2, () => x = 1));
        Assert.False(h.CanRedo);
        Assert.Equal(2, x);
    }

    [Fact]
    public void Composite_executes_forward_undoes_reverse()
    {
        var log = new List<string>();
        var a1 = new DelegateEditAction(() => log.Add("do1"), () => log.Add("undo1"));
        var a2 = new DelegateEditAction(() => log.Add("do2"), () => log.Add("undo2"));
        var comp = new CompositeEditAction(new[] { a1, a2 });

        var h = new CommandHistory();
        h.Do(comp);
        Assert.Equal(new[] { "do1", "do2" }, log);

        h.Undo();
        Assert.Equal(new[] { "do1", "do2", "undo2", "undo1" }, log); // 자식 역순 복원
    }

    [Fact]
    public void Changed_event_fires_on_each_op()
    {
        int fired = 0;
        var h = new CommandHistory();
        h.Changed += () => fired++;
        h.Do(new DelegateEditAction(() => { }, () => { }));
        h.Undo();
        h.Redo();
        Assert.Equal(3, fired);
    }
}
