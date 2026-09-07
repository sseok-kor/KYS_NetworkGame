namespace DataEditor.Edit;

/// <summary>do/undo 델리게이트로 즉석 편집 액션을 만드는 편의 구현.
/// 예: 속성 before/after 값을 클로저로 캡처해 되돌림.</summary>
public sealed class DelegateEditAction : IEditAction
{
    private readonly Action _execute;
    private readonly Action _unexecute;

    public DelegateEditAction(Action execute, Action unexecute)
    {
        _execute = execute;
        _unexecute = unexecute;
    }

    public void Execute() => _execute();
    public void Unexecute() => _unexecute();
}
