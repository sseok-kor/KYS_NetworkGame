namespace DataEditor.Core;

/// <summary>
/// 게임 C++ GameDefines.h의 상수를 그대로 이식.
/// 에디터 검증이 게임 부팅 검증과 1:1이려면 이 값들이 절대 어긋나면 안 된다
/// (설계 §5·§6·§13, 리뷰 #4·#5가 잡은 부류의 불일치를 원천 차단).
/// C++가 코드 리터럴로 쓰던 값(예: moveSpeed 하한 43)도 여기서 명시 상수화.
/// </summary>
public static class EditorConstants
{
    // --- 맵 개수/크기 ---
    public const int MaxMapCount = 16;            // MAX_MAP_COUNT
    public const int MinMapSizePx = 1000;         // MIN_MAP_SIZE_PX
    public const int MaxMapSizePx = 4000;         // MAX_MAP_SIZE_PX
    public const int MapSizeStepPx = 250;         // MAP_SIZE_STEP_PX (크기는 250 배수여야 부팅 통과)

    // --- 이름/파일명 길이 (C++는 '15자 + 널'을 최대로 봄 → 사용 가능 문자 수 = MAX-1) ---
    public const int MapNameMaxChars = 15;        // MAP_NAME_MAX(16) - 1
    public const int MapFileMaxChars = 31;        // MAP_FILE_MAX(32) - 1 (walkableFile)
    public const int ItemNameMaxChars = 15;       // ITEM_NAME_MAX(16) - 1

    // --- 통행(walkable) 격자 ---
    public const int WalkCellSize = 50;           // WALK_CELL_SIZE (벽 판정 셀 = 50px)
    public const int WalkCellsMax = 80;           // WALK_CELLS_MAX (4000/50)
    public const int WalkRowStrideBytes = 10;     // WALK_ROW_STRIDE_BYTES (.bin 행 stride, 80비트/행)

    // --- AOI 색인 격자 (편집 대상 아님·참고 오버레이용) ---
    public const int AoiCellSize = 250;           // CELL_SIZE (AOI) = WalkCellSize * 5
    public const int ViewRange = 400;             // VIEW_RANGE

    // --- 몬스터/스폰 ---
    public const int MinMoveSpeed = 43;           // 코드 리터럴 하한 (42 이하는 대각 이동량 0px 절단 → 안 멈추게)
    public const int MaxMoveSpeed = 200;          // MAX_MOVE_SPEED
    public const int MaxGroupCount = 32;          // MAX_GROUP_COUNT (스폰 그룹 1개 마릿수 상한)
    public const int MaxSpawnGroups = 256;        // MAX_SPAWN_GROUPS (spawns.csv 행 상한)
    public const int MonsterPoolCapacity = 10000; // DEFAULT_MONSTER_POOL_CAPACITY (전맵 count/cap 총합 상한)
    public const int MonsterTypeCount = 9;        // EMonsterType 전수(0..8 모두 정의 필수)

    // --- 아이템/드랍 ---
    public const int ItemTypeCount = 4;           // EItemType (0..3)
    public const int MaxItemDefs = 128;           // MAX_ITEM_DEFS (초과분 관대 절단 → 상한초과는 Warning, 리뷰 #4)
    public const int MaxDropRules = 256;          // MAX_DROP_RULES (초과 = Error·strict)
    public const int GroundItemMapCap = 100;      // GROUND_ITEM_MAP_CAP
    public const int ChancePermilMin = 1;         // 드랍 확률 유효 하한 (permil)
    public const int ChancePermilMax = 1000;      // 1000 = 100%

    // --- 포탈 ---
    public const int MaxPortals = 64;             // MAX_PORTALS (초과분 관대 절단 → Warning, 리뷰 #4)

    // --- maps.csv 열 개수 (문자열 필드 콤마 검증에 사용·리뷰 #9) ---
    public const int MapCsvFieldCount = 10;
}
