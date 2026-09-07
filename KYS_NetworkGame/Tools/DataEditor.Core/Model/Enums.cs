namespace DataEditor.Model;

/// <summary>
/// 몬스터 종류. 게임 C++ EMonsterType과 값·순서가 정확히 일치해야 한다
/// (spawns.type / monsters.type가 이 값을 참조). 종류 증감은 게임 코드(enum) 변경 필요 →
/// 에디터는 9종 스탯/드랍만 편집, 종류 추가/삭제는 불가(리뷰 대상 openQuestion 8).
/// monsters.yml은 0..8 전 9종을 반드시 정의해야 부팅 통과.
/// </summary>
public enum MonsterType
{
    Goblin = 0,
    Orc = 1,
    Ogre = 2,
    Slime = 3,
    Fenrir = 4,   // 보스
    Wolf = 5,
    Bear = 6,
    Troll = 7,
    Golem = 8,    // 보스급
}

/// <summary>
/// 아이템 종류. 게임 C++ EItemType과 일치.
/// </summary>
public enum ItemType
{
    Weapon = 0,    // 무기 (atkPower 가산)
    Armor = 1,     // 방어구 (defPower 가산)
    Consume = 2,   // 소모품 (hpRestore)
    Etc = 3,       // 기타
}

/// <summary>
/// 검증 결과 심각도. 게임 부팅 fail-fast 기준과 1:1 매핑(설계 §6·§13).
/// Error = 게임이 부팅 거부(FATAL)하는 데이터 → 저장은 되나 "서버가 안 뜸" 경고.
/// Warning = 게임이 행을 조용히 무시하고 부팅은 통과하는 데이터(items/portals 관대 정책, 드랍 행-무시).
/// </summary>
public enum FindingSeverity
{
    Warning = 0,
    Error = 1,
}
