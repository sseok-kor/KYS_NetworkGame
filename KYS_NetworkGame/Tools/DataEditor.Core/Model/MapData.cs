using CommunityToolkit.Mvvm.ComponentModel;

namespace DataEditor.Model;

/// <summary>
/// maps.csv 한 행 (10컬럼). mapId는 반드시 행 순서(0부터 연속)와 일치해야 게임 부팅 통과.
/// type/musicId는 예약 필드(게임 미사용)이나 값은 보존·편집 허용.
/// ObservableValidator 기반: 속성 인라인 검증(INotifyDataErrorInfo)을 얹기 위함(리뷰 #20).
/// 파일 간 교차검증(walkableFile 존재성 등)은 ValidationEngine이 담당.
/// </summary>
public partial class MapData : ObservableValidator
{
    [ObservableProperty] private int _mapId;              // [0] 행 인덱스와 일치(0연속)
    [ObservableProperty] private string _name = "";       // [1] <=15자, 콤마/개행 금지(리뷰 #9)
    [ObservableProperty] private int _type;               // [2] 0=마을/1=필드/2=던전 (예약)
    [ObservableProperty] private int _width;              // [3] px, [1000,4000] 250배수
    [ObservableProperty] private int _height;             // [4] px
    [ObservableProperty] private int _monsterCap;         // [5] 맵 몬스터 안전 천장
    [ObservableProperty] private int _respawnX;           // [6] 부활 좌표(폐구간 [0,width])
    [ObservableProperty] private int _respawnY;           // [7]
    [ObservableProperty] private int _musicId;            // [8] (예약)
    [ObservableProperty] private string _walkableFile = "-"; // [9] "-"=전통행 / 파일명(<=31자)

    /// <summary>walkableFile이 "-"가 아니면 통행 CSV를 강제 적재하는 맵.</summary>
    public bool HasWalkableFile => WalkableFile != "-" && !string.IsNullOrEmpty(WalkableFile);
}
