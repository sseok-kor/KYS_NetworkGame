namespace DataEditor.Edit;

/// <summary>
/// 되돌릴 수 있는 편집 조작 한 단위(GoF Command). ⚠️ WPF의 RelayCommand(UI 커맨드 바인딩)와 다른 개념(리뷰 #3).
/// Execute = 적용, Unexecute = 되돌림. 이동/리사이즈/속성변경/셀페인트 등 모든 편집을 이 인터페이스로 감싼다.
/// </summary>
public interface IEditAction
{
    void Execute();
    void Unexecute();
}
