namespace DataEditor.Edit;

/// <summary>
/// undo/redo 2스택. 모든 편집은 여기를 거친다(설계 §8).
/// Do(a) = 실행 + undo 스택에 push + redo 클리어. Push(a) = 이미 실행된 액션 등록(드래그 트랜잭션 등).
/// </summary>
public sealed class CommandHistory
{
    private readonly Stack<IEditAction> _undo = new();
    private readonly Stack<IEditAction> _redo = new();

    /// <summary>스택이 바뀔 때마다 발생(Do/Push/Undo/Redo/Clear). 현재 UI 구독자는 없고, 툴바 Undo/Redo 버튼은 CanExecute 없이 항상 활성이다.</summary>
    public event Action? Changed;

    public bool CanUndo => _undo.Count > 0;
    public bool CanRedo => _redo.Count > 0;

    /// <summary>액션을 실행하고 등록.</summary>
    public void Do(IEditAction action)
    {
        action.Execute();
        _undo.Push(action);
        _redo.Clear();
        Changed?.Invoke();
    }

    /// <summary>이미 실행된 액션을 등록만(드래그 mouse down~up을 1 트랜잭션으로 묶어 넘길 때).</summary>
    public void Push(IEditAction action)
    {
        _undo.Push(action);
        _redo.Clear();
        Changed?.Invoke();
    }

    public void Undo()
    {
        if (_undo.Count == 0) return;
        var a = _undo.Pop();
        a.Unexecute();
        _redo.Push(a);
        Changed?.Invoke();
    }

    public void Redo()
    {
        if (_redo.Count == 0) return;
        var a = _redo.Pop();
        a.Execute();
        _undo.Push(a);
        Changed?.Invoke();
    }

    public void Clear()
    {
        _undo.Clear();
        _redo.Clear();
        Changed?.Invoke();
    }
}
