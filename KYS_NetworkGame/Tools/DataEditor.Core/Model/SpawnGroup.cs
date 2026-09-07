using CommunityToolkit.Mvvm.ComponentModel;

namespace DataEditor.Model;

/// <summary>
/// spawns.csv 한 행 (8컬럼·정수 전용). 사각 구역 [x0,y0]~[x1,y1] 안에 count 마리 유지.
/// 구역·count·cap·어그로거리·통행성·도달성 교차검증은 ValidationEngine.
/// </summary>
public partial class SpawnGroup : ObservableValidator
{
    [ObservableProperty] private int _mapId;            // [0] maps 행 인덱스
    [ObservableProperty] private int _type;             // [1] EMonsterType 0..8
    [ObservableProperty] private int _x0;               // [2] 구역 좌상 (x0<=x1)
    [ObservableProperty] private int _y0;               // [3]
    [ObservableProperty] private int _x1;               // [4] 구역 우하
    [ObservableProperty] private int _y1;               // [5]
    [ObservableProperty] private int _count;            // [6] 유지 마릿수 1..32
    [ObservableProperty] private int _respawnDelayMs;   // [7] 부활 지연 ms (>=1, 0=구버전 파일 판별)
}
