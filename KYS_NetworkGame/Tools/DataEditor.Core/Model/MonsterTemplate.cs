using System.Collections.Generic;
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;

namespace DataEditor.Model;

/// <summary>monsters.yml 드랍 규칙 한 항목(인라인 flow 맵).</summary>
public partial class DropRule : ObservableObject
{
    [ObservableProperty] private int _item;          // items.csv templateId FK (없으면 부팅 거부)
    [ObservableProperty] private int _chancePermil;  // 1..1000 (범위 밖은 게임 행-무시=Warning)
    [ObservableProperty] private int _min;           // >0
    [ObservableProperty] private int _max;           // >=min, <=해당 아이템 stackMax(초과=Error)
}

/// <summary>
/// monsters.yml 한 몬스터. type 0..8 전 9종을 반드시 정의해야 부팅(종류 증감 불가·enum이 코드).
/// name은 예약(미사용)이나 있으면 15자 검증.
/// </summary>
public partial class MonsterTemplate : ObservableValidator
{
    [ObservableProperty] private int _type;              // 0..8 유일
    [ObservableProperty] private int _hp;
    [ObservableProperty] private int _atkPower;
    [ObservableProperty] private int _moveSpeed;         // 43..200 (하한 43: 대각 0px 절단 방지)
    [ObservableProperty] private int _aggroRange;        // 스폰 구역-respawn 어그로거리 검증에 사용
    [ObservableProperty] private int _attackRange;
    [ObservableProperty] private float _attackCooldown;  // float (round-trip 시 "1.0" 포맷 보존 주의·리뷰 #1)
    [ObservableProperty] private string _name = "";      // 예약, <=15
    [ObservableProperty] private bool _isBoss;           // 기본 false
    [ObservableProperty] private int _cap;               // 0=무제한, isBoss=true면 >=1

    public ObservableCollection<DropRule> Drops { get; } = new();

    /// <summary>로드 시 이 몬스터 블록에 물리적으로 존재한 yaml 키 집합(camelCase).
    /// 게임은 필수 스탯 필드가 하나라도 없으면 부팅 거부하나 YamlDotNet은 없으면 0으로 채워
    /// 무음 통과하므로, 로더가 present-key 를 기록해 ValidationEngine 이 누락을 잡는다(리뷰 F8).</summary>
    public HashSet<string> LoadedKeys { get; } = new();
}
