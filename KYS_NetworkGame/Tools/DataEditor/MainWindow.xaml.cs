using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using DataEditor.Edit;
using DataEditor.ViewModels;

namespace DataEditor;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        SpatialCanvas.EditCommitted += OnCanvasEdit;
    }

    /// <summary>우클릭한 행을 먼저 선택 — "캔버스에서 보기" 컨텍스트 메뉴가 그 행을 대상으로 동작하게.
    /// (WPF DataGrid는 우클릭으로 선택이 안 바뀌므로 직접 선택.)</summary>
    private void Grid_PreviewMouseRightButtonDown(object sender, MouseButtonEventArgs e)
    {
        var dep = e.OriginalSource as System.Windows.DependencyObject;
        while (dep != null && dep is not DataGridRow) dep = System.Windows.Media.VisualTreeHelper.GetParent(dep);
        if (dep is DataGridRow row) row.IsSelected = true;
    }

    /// <summary>검증 결과 행 더블클릭 → 위반 엔티티의 맵 캔버스로 이동(공간 엔티티면 하이라이트, 아니면 no-op).</summary>
    private void Finding_MouseDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (sender is DataGrid g && g.SelectedItem is Validation.Finding f && DataContext is MainViewModel vm)
            vm.NavigateTo(f.Entity);
    }

    /// <summary>Maps 그리드 자동 열 생성 시 walkableFile 열을 읽기전용으로 —
    /// 이름 변경은 저장 시 격자를 옛 이름으로 쓰는 desync(maps.csv가 없는 파일 참조)를 낳으므로 v1에선 편집 금지(F15).</summary>
    private void MapsGrid_AutoGeneratingColumn(object sender, DataGridAutoGeneratingColumnEventArgs e)
    {
        if (e.PropertyName == nameof(Model.MapData.WalkableFile))
            e.Column.IsReadOnly = true;
    }

    /// <summary>캔버스 편집(통행 페인트 스트로크 · 스폰/포탈 드래그 이동) 커밋 → undo 스택 등록 + 재검증.</summary>
    private void OnCanvasEdit(IEditAction action)
    {
        if (DataContext is MainViewModel vm)
        {
            vm.History.Push(action);
            vm.ValidateCommand.Execute(null);
        }
    }

    // ---- 표(DataGrid) 셀 편집 undo 통합(결정 A·CellEditEnding 인터셉트) ----
    //   View 이벤트에서 편집 전/후 값을 리플렉션으로 잡아 IEditAction 으로 히스토리에 등록 -> 표·캔버스 단일 undo 스택.
    //   모델은 undo 를 전혀 모른다(계층 분리). 캔버스가 같은 프로퍼티를 직접 세팅하는 경로와 겹치지 않는다.
    private object? _cellOldValue;
    private System.Reflection.PropertyInfo? _cellProp;
    private object? _cellTarget;

    private void Grid_BeginningEdit(object sender, DataGridBeginningEditEventArgs e)
    {
        _cellProp = null; _cellTarget = null; _cellOldValue = null;
        var path = ((e.Column as DataGridBoundColumn)?.Binding as System.Windows.Data.Binding)?.Path?.Path;
        var item = e.Row?.Item;
        if (string.IsNullOrEmpty(path) || item == null) return;
        var prop = item.GetType().GetProperty(path);
        if (prop == null || !prop.CanWrite) return;
        _cellTarget = item; _cellProp = prop; _cellOldValue = prop.GetValue(item); // 편집 전 값 스냅샷
    }

    private void Grid_CellEditEnding(object sender, DataGridCellEditEndingEventArgs e)
    {
        var prop = _cellProp; var target = _cellTarget; var oldVal = _cellOldValue;
        _cellProp = null; _cellTarget = null; _cellOldValue = null;
        if (e.EditAction != DataGridEditAction.Commit || prop == null || target == null || DataContext is not MainViewModel vm) return;

        // 새 값은 이 이벤트 "직후" 바인딩이 모델에 써넣으므로 한 틱 미뤄 읽는다(WPF 타이밍 함정).
        Dispatcher.BeginInvoke(System.Windows.Threading.DispatcherPriority.Background, new Action(() =>
        {
            var newVal = prop.GetValue(target);
            if (Equals(oldVal, newVal)) return; // 실제 변경 없으면 기록 안 함
            vm.History.Push(new DelegateEditAction(
                () => prop.SetValue(target, newVal),   // redo
                () => prop.SetValue(target, oldVal))); // undo
            vm.ValidateCommand.Execute(null);
        }));
    }
}
