namespace DataEditor.Edit;

/// <summary>
/// 여러 편집을 하나의 원자 undo 단위로 묶는다(리뷰 #3·#12).
/// Execute는 정방향, Unexecute는 역순 — cascade(맵 삭제→참조 재매핑, 리사이즈→좌표 클램프, 브러시 스트로크)를
/// 한 번의 Ctrl+Z로 온전히 되돌리기 위함. 부분 undo로 데이터가 어긋나는 것을 막는다.
/// </summary>
public sealed class CompositeEditAction : IEditAction
{
    private readonly IReadOnlyList<IEditAction> _children;

    public CompositeEditAction(IReadOnlyList<IEditAction> children) => _children = children;

    public void Execute()
    {
        foreach (var c in _children) c.Execute();
    }

    public void Unexecute()
    {
        for (int i = _children.Count - 1; i >= 0; i--) _children[i].Unexecute(); // 역순 복원
    }
}
