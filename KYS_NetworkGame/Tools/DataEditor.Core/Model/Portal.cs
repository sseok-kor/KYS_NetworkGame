using CommunityToolkit.Mvvm.ComponentModel;

namespace DataEditor.Model;

/// <summary>
/// portals.csv 한 행 (7컬럼·정수 전용). portalId = 행 순서.
/// 게임은 위반 행을 조용히 무시(관대)하나, main.cpp의 FK/도달성 교차검증은 부팅 거부(Error).
/// </summary>
public partial class Portal : ObservableValidator
{
    [ObservableProperty] private int _srcMapId;         // [0]
    [ObservableProperty] private int _triggerX;         // [1] 트리거 중심
    [ObservableProperty] private int _triggerY;         // [2]
    [ObservableProperty] private int _triggerRadius;    // [3] 근접 발동 반경(px, 원)
    [ObservableProperty] private int _dstMapId;         // [4]
    [ObservableProperty] private int _dstX;             // [5] 도착 좌표(서버 권위)
    [ObservableProperty] private int _dstY;             // [6]
}
