using CommunityToolkit.Mvvm.ComponentModel;

namespace DataEditor.Model;

/// <summary>
/// items.csv 한 행 (7컬럼). templateId는 monsters.yml drops의 FK 대상이라
/// 에디터는 유일성·stackMax 정합을 게임보다 엄격히 강제(유령 드랍 방지·설계 §6).
/// 게임 자체는 items 위반 행을 조용히 무시(관대).
/// </summary>
public partial class ItemDef : ObservableValidator
{
    [ObservableProperty] private int _templateId;   // [0] >0, 유일 (drops FK 대상)
    [ObservableProperty] private string _name = ""; // [1] <=15자, 콤마/개행 금지
    [ObservableProperty] private int _type;         // [2] EItemType 0..3
    [ObservableProperty] private int _atkPower;      // [3]
    [ObservableProperty] private int _defPower;      // [4]
    [ObservableProperty] private int _hpRestore;     // [5]
    [ObservableProperty] private int _stackMax;      // [6] 한 슬롯 최대(장비=1)
}
