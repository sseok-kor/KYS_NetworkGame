#include "pch_gamecommon.h"
#include "MapTable.h"
#include <cstdio>      // _wfopen_s / fgets / fclose / swprintf_s / wprintf
#include <cstdlib>     // strtol
#include <cstring>     // strchr / strlen
#include <cwchar>      // wcsrchr
#include <Windows.h>   // GetModuleFileNameW / MultiByteToWideChar / MAX_PATH

// 맵 메타 표는 부팅에 한 번 적재 후 읽기 전용 - 고정 배열(게임 서버 예측 가능 메모리).
//   배열 차원 = MAX_MAP_COUNT(상한), 실제 쓰는 칸 = g_mapCount(파일 행 수).
static MapDef g_maps[MAX_MAP_COUNT];
static int    g_mapCount = 0;

// maps.csv 를 열어 FILE* 반환 (실패 = nullptr). PortalTable/ItemTable 과 같은 탐색 -
//   exe 디렉터리 기준 0..5 단계 위를 훑고, 실패하면 working dir 폴백.
static FILE* OpenMapFile()
{
    // (1) exe 디렉터리 기준 (결정적 - cwd 무관)
    wchar_t exeDir[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
    {
        wchar_t* slash = wcsrchr(exeDir, L'\\');
        if (slash != nullptr) { *slash = L'\0'; }   // 파일명 제거 -> exe 디렉터리만 남김

        static const wchar_t* const relFromExe[] =
        {
            L"\\Data\\maps.csv",
            L"\\..\\Data\\maps.csv",
            L"\\..\\..\\Data\\maps.csv",
            L"\\..\\..\\..\\Data\\maps.csv",          // Build\x64\<Config>\ -> repo 루트 (정상 경로)
            L"\\..\\..\\..\\..\\Data\\maps.csv",
            L"\\..\\..\\..\\..\\..\\Data\\maps.csv",
        };
        for (int i = 0; i < 6; ++i)
        {
            wchar_t path[MAX_PATH];
            if (swprintf_s(path, L"%s%s", exeDir, relFromExe[i]) > 0)
            {
                FILE* fp = nullptr;
                if (_wfopen_s(&fp, path, L"rt") == 0 && fp != nullptr) { return fp; }
            }
        }
    }

    // (2) 폴백: working dir 기준
    static const wchar_t* const cwdCandidates[] = { L"Data\\maps.csv", L"..\\Data\\maps.csv" };
    for (int i = 0; i < 2; ++i)
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, cwdCandidates[i], L"rt") == 0 && fp != nullptr) { return fp; }
    }
    return nullptr;
}

// 부팅 실패 사유를 콘솔에 남긴다 - 이 표는 서버와 DummyClient 가 공유하므로 특정 앱 로거에 묶지 않고
//   양쪽 다 보이는 wprintf 를 쓴다 (부팅 시점이라 콘솔 가시성 충분. 서버 fatal 파일 로그는 main 이 별도).
template <typename... Args>
static void PrintLoadError(const wchar_t* fmt, Args... args)
{
    wprintf(L"[maps.csv] ");
    wprintf(fmt, args...);
    wprintf(L"\n");
}

// cursor 에서 다음 콤마 전까지를 한 필드로 잘라 앞뒤 공백을 벗겨 outField 에 준다.
//   반환 = 다음 필드 시작 위치 (마지막 필드였으면 nullptr). ItemTable 과 동일한 수동 분리
//   (name/walkableFile 문자열 컬럼이 있어 sscanf_s 정수 전용 파싱을 못 쓴다).
static char* SplitField(char* cursor, char** outField)
{
    char* comma = strchr(cursor, ',');
    if (comma != nullptr) { *comma = '\0'; }

    while (*cursor == ' ' || *cursor == '\t') { ++cursor; }               // 앞 공백 제거
    char* end = cursor + strlen(cursor);
    while (end > cursor &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
    {
        *--end = '\0';                                                     // 뒤 공백/개행 제거
    }
    *outField = cursor;
    return (comma != nullptr) ? (comma + 1) : nullptr;
}

// 숫자 필드를 엄격 파싱한다 - "abc"나 "12abc" 같은 오타가 무음으로 0/12 가 되지 않게
//   (strtol 은 비수치를 조용히 0 으로 돌려 데이터 실수가 숨는다 - 잔여 문자가 있으면 실패로 취급).
static bool ParseIntStrict(const char* field, int& out)
{
    if (field == nullptr || field[0] == '\0') { return false; }
    char* end = nullptr;
    const long value = strtol(field, &end, 10);
    if (end == field || *end != '\0') { return false; }   // 숫자 0개 또는 숫자 뒤 잔여 문자 = 오타
    out = static_cast<int>(value);
    return true;
}

int LoadMapTable()
{
    FILE* fp = OpenMapFile();
    if (fp == nullptr)
    {
        PrintLoadError(L"파일을 찾지 못함 (Data/maps.csv 확인)");
        return -1;
    }

    g_mapCount = 0;
    bool valid = true;
    char line[512];
    while (valid && fgets(line, sizeof(line), fp) != nullptr)
    {
        // 파일 선두 UTF-8 BOM(EF BB BF)은 건너뛴다 - 에디터가 붙였을 때 첫 행이 형식 오류로 오진되는 것 방지.
        char* text = line;
        if (static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) { text += 3; }
        if (text[0] == '#' || text[0] == '\n' || text[0] == '\r' || text[0] == '\0') { continue; }   // 주석/빈 줄

        // 하드코딩 표와 달리 파일은 오타가 가능하다 - 상한 초과는 조용한 절단 대신 부팅 거부
        //   (절단하면 뒤쪽 맵이 무음 소실되어 스폰/포탈 참조가 깨진 채 돌게 된다).
        if (g_mapCount >= MAX_MAP_COUNT)
        {
            PrintLoadError(L"맵 상한(%d) 초과", MAX_MAP_COUNT);
            valid = false; break;
        }

        // 컬럼 10개: mapId, name, type, width, height, monsterCap, respawnX, respawnY, musicId, walkableFile
        char* fields[10] = {};
        char* cursor = text;
        int fieldCount = 0;
        while (fieldCount < 10 && cursor != nullptr)
        {
            cursor = SplitField(cursor, &fields[fieldCount]);
            ++fieldCount;
        }
        if (fieldCount < 10)
        {
            PrintLoadError(L"%d행 - 필드 10개가 아닌 줄 (형식 오류)", g_mapCount);
            valid = false; break;
        }

        MapDef& d = g_maps[g_mapCount];

        // 숫자 필드 8개 엄격 파싱 (비수치 오타 = 부팅 거부. 문자열 필드 = name[1]/walkableFile[9]).
        if (!ParseIntStrict(fields[0], d.mapId) ||
            !ParseIntStrict(fields[2], d.mapType) ||
            !ParseIntStrict(fields[3], d.width) ||
            !ParseIntStrict(fields[4], d.height) ||
            !ParseIntStrict(fields[5], d.monsterCap) ||
            !ParseIntStrict(fields[6], d.respawn.x) ||
            !ParseIntStrict(fields[7], d.respawn.y) ||
            !ParseIntStrict(fields[8], d.musicId))
        {
            PrintLoadError(L"%d행 - 숫자 필드에 비수치 문자 (오타 확인)", g_mapCount);
            valid = false; break;
        }

        // mapId = 배열 인덱스 규약 - 행 순서와 다르면(구멍/뒤섞임) 스폰/포탈의 mapId 참조가 어긋난다.
        if (d.mapId != g_mapCount)
        {
            PrintLoadError(L"%d행 - mapId %d 가 행 순서와 불일치 (0부터 연속이어야 함)", g_mapCount, d.mapId);
            valid = false; break;
        }

        // 이름은 UTF-8 -> wchar 변환 (파일은 UTF-8, 게임 문자열 규약은 wchar_t). 초과/손상 = 무음 '?' 대신 부팅 거부.
        const int converted = ::MultiByteToWideChar(CP_UTF8, 0, fields[1], -1, d.name, MAP_NAME_MAX);
        if (converted == 0)
        {
            PrintLoadError(L"%d행 - name 이 %d자 초과이거나 손상", g_mapCount, MAP_NAME_MAX - 1);
            valid = false; break;
        }
        d.name[MAP_NAME_MAX - 1] = L'\0';

        const int fileConverted = ::MultiByteToWideChar(CP_UTF8, 0, fields[9], -1, d.walkableFile, MAP_FILE_MAX);
        if (fileConverted == 0)
        {
            PrintLoadError(L"%d행 - walkableFile 이 %d자 초과이거나 손상", g_mapCount, MAP_FILE_MAX - 1);
            valid = false; break;
        }
        d.walkableFile[MAP_FILE_MAX - 1] = L'\0';

        // 맵 크기는 맵마다 다를 수 있다 - 단 하한/상한과 250 배수만 허용.
        //   250 배수 = AOI 셀(250)과 통행 셀(50)을 모두 정수로 나누는 단위 (반 칸짜리 격자가 안 생김).
        if (d.width  < MIN_MAP_SIZE_PX || d.width  > MAX_MAP_SIZE_PX || (d.width  % MAP_SIZE_STEP_PX) != 0 ||
            d.height < MIN_MAP_SIZE_PX || d.height > MAX_MAP_SIZE_PX || (d.height % MAP_SIZE_STEP_PX) != 0)
        {
            PrintLoadError(L"%d행 - 크기 %dx%d (허용 = %d..%d 의 %d 배수)", g_mapCount, d.width, d.height,
                           MIN_MAP_SIZE_PX, MAX_MAP_SIZE_PX, MAP_SIZE_STEP_PX);
            valid = false; break;
        }

        if (d.monsterCap < 0)
        {
            PrintLoadError(L"%d행 - monsterCap %d 음수", g_mapCount, d.monsterCap);
            valid = false; break;
        }

        // 부활 좌표는 그 맵 안이어야 한다 (맵 밖 부활 = 즉시 클램프 보정으로 이상 위치).
        //   통행 칸 위인지는 통행 격자 적재 후 LoadWalkableTable 이 추가 검증한다 (여긴 격자가 아직 없음).
        if (d.respawn.x < 0 || d.respawn.x > d.width ||
            d.respawn.y < 0 || d.respawn.y > d.height)
        {
            PrintLoadError(L"%d행 - 부활 좌표 (%d,%d) 가 맵(%dx%d) 밖", g_mapCount, d.respawn.x, d.respawn.y, d.width, d.height);
            valid = false; break;
        }

        ++g_mapCount;
    }
    fclose(fp);

    if (!valid || g_mapCount <= 0)
    {
        if (valid && g_mapCount <= 0) { PrintLoadError(L"0행 (맵 최소 1행 필요)"); }
        g_mapCount = 0;
        return -1;   // 형식/검증 실패 또는 0행 = 적재 실패 (부팅 거부)
    }

    // 맵 전체 천장(cap) 합이 채널당 몬스터 풀 용량을 넘으면 스폰이 풀 고갈로 보류될 수 있다 - 부팅에서 잡는다.
    int capTotal = 0;
    for (int i = 0; i < g_mapCount; ++i) { capTotal += g_maps[i].monsterCap; }
    if (capTotal > (int)DEFAULT_MONSTER_POOL_CAPACITY)
    {
        PrintLoadError(L"monsterCap 합 %d 가 몬스터 풀 용량 %d 초과", capTotal, (int)DEFAULT_MONSTER_POOL_CAPACITY);
        g_mapCount = 0;
        return -1;
    }

    return g_mapCount;
}

int MapTableCount()
{
    return g_mapCount;
}

const MapDef& MapTableAt(int mapId)
{
    return g_maps[mapId];
}

// 경계 클램프용 안전 조회 - 미적재/범위 밖이면 상한(4000)으로 폴백.
//   서버/봇은 로드 실패 = 부팅 거부라 폴백 경로가 없고, GameClient 관대 폴백에서만 실제로 밟는다
//   (4000 = 현행 전 맵 크기라 "지형 못 읽은 클라"가 종전과 같은 경계로 동작하는 자연 강등).
int MapWidthFor(int mapId)
{
    if (mapId < 0 || mapId >= g_mapCount) { return MAX_MAP_SIZE_PX; }
    return g_maps[mapId].width;
}

int MapHeightFor(int mapId)
{
    if (mapId < 0 || mapId >= g_mapCount) { return MAX_MAP_SIZE_PX; }
    return g_maps[mapId].height;
}
