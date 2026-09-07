#include "pch_gamecommon.h"
#include "WalkableTable.h"
#include "MapTable.h"    // MapTableCount / MapTableAt / MapWidthFor / MapHeightFor (파일명·크기·부활 좌표)
#include "Pathfinding/Direction.h"   // MOVE_DIR_DELTA (AdvanceMove 위치 예측 단위 벡터)
#include <cstdio>       // _wfopen_s / fgets / fread / fwrite / fclose / swprintf_s / wprintf
#include <cstring>      // memset
#include <cwchar>       // wcscmp / wcsrchr / wcscpy_s / wcscat_s
#include <Windows.h>    // GetModuleFileNameW / GetFileAttributesExW / MoveFileExW / DeleteFileW / MAX_PATH

// 통행 비트 저장소 - 부팅 1회 채운 뒤 읽기 전용(채널 스레드 무락 조회).
//   행 stride 는 맵 폭과 무관하게 고정(WALK_ROW_STRIDE_BYTES) - 파일과 메모리가 같은 인덱스 공식을 써서
//   "파일은 8바이트 행·메모리는 10바이트 행" 식의 어긋남(2행부터 전 비트가 밀리는 조용한 오염)이 안 생긴다.
//   폭이 좁은 맵은 행 끝 비트가 놀지만 조회가 닿지 않아 무해.
static BYTE g_walkBits[MAX_MAP_COUNT][WALK_BYTES_PER_MAP];
static int  g_walkMapCount = 0;   // 적재된 맵 수 (0 = 미적재 - IsWalkable 이 전-통행 폴백)

static const UINT32 WALK_BIN_MAGIC = 0x4B4C4157;   // "WALK" - 캐시 파일 첫 4바이트 (다른 파일 오인 적재 방어)

// 부팅 실패/경고 사유를 콘솔에 남긴다 - 서버/봇/클라가 공유하는 로더라 특정 앱 로거에 묶지 않는다 (MapTable 과 동일).
template <typename... Args>
static void PrintWalkError(const wchar_t* fmt, Args... args)
{
    wprintf(L"[walkable] ");
    wprintf(fmt, args...);
    wprintf(L"\n");
}

// 좌표 -> 칸 번호. 맵 끝 좌표(x==width, 폐구간)는 마지막 칸으로 취급 - AOI 격자 Map::ColOf 와 같은 규칙.
//   (4000/50=80 은 칸 번호 0..79 밖이라 클램프가 없으면 배열 밖 비트를 읽는다.)
static int CellIndexOf(int coord, int sizePx)
{
    int cell = coord / WALK_CELL_SIZE;
    const int cellCount = sizePx / WALK_CELL_SIZE;
    if (cell < 0) { cell = 0; }
    if (cell >= cellCount) { cell = cellCount - 1; }
    return cell;
}

// 칸 하나의 통행 비트 (호출자가 mapId/칸 범위를 보장)
static bool CellWalkable(int mapId, int col, int row)
{
    const BYTE packed = g_walkBits[mapId][row * WALK_ROW_STRIDE_BYTES + col / 8];
    return ((packed >> (col % 8)) & 1) != 0;
}

// from->to 선분이 이 칸(사각)을 실제로 지나는가 - 칸 네 모서리가 선분 직선의 같은 쪽에 전부 몰려 있으면
//   안 지난다 (정수 외적 부호 판정 - 부동소수 없음). 모서리가 선분 위(외적 0)면 양쪽 다로 세어 "지남"으로
//   본다(안전측) - 축평행 선분이 격자선(50배수 좌표) 위를 지날 때 그 선을 낀 벽 칸이 supercover 에서
//   빠져 이동 게이트가 벽을 통과시키는 구멍을 막는다(IsWalkable 의 칸 판정과 경계에서 일치).
//   호출자는 두 점을 덮는 칸 사각형 안의 칸만 넘기므로, 직선이 칸을 지나면 선분 구간도 그 칸을 지난다.
static bool SegmentTouchesCell(const Position& from, const Position& to, int col, int row)
{
    const long long dirX = to.x - from.x;
    const long long dirY = to.y - from.y;
    const int left = col * WALK_CELL_SIZE;
    const int top  = row * WALK_CELL_SIZE;
    const int cornerX[4] = { left, left + WALK_CELL_SIZE, left,                  left + WALK_CELL_SIZE };
    const int cornerY[4] = { top,  top,                   top + WALK_CELL_SIZE, top + WALK_CELL_SIZE };

    bool anyLeft = false;
    bool anyRight = false;
    for (int i = 0; i < 4; ++i)
    {
        const long long cross = dirX * (cornerY[i] - from.y) - dirY * (cornerX[i] - from.x);
        if (cross >= 0) { anyLeft = true; }    // 0(선분 위)은 양쪽 다로 셈 - 경계선 위 벽 칸 누락 방지
        if (cross <= 0) { anyRight = true; }
    }
    if (anyLeft && !anyRight) { return false; }    // 네 모서리 전부 직선 왼쪽 - 안 지남
    if (anyRight && !anyLeft) { return false; }    // 네 모서리 전부 직선 오른쪽 - 안 지남
    return true;                                   // 양쪽에 걸침(또는 모서리 접촉/점 구간) - 지남
}

// 대각 코너 컷 방어 - 선분이 격자 코너점(X=col*50, Y=row*50)을 대각으로 지날 때, 그 코너를 낀
//   두 직교 이웃 칸 중 하나라도 벽이면 대각 전진 불가로 본다 (no-corner-cut 표준 - 벽 모서리를 스쳐
//   비집고 들어가는 것을 막는다). SegmentTouchesCell 이 외적 0 을 양쪽으로 세면서부터는 코너를 점으로만
//   스치는 벽 칸도 "지남"으로 잡히므로, 픽셀 선분 경로에서 이 함수만 단독으로 거부하는 경우는 없다(중첩 방어).
//   그래도 두는 이유 - 격자 좌표를 바로 다루는 A* 이웃 확장에는 여기가 1차 방어이고,
//   "이동은 코너 컷 금지, 시야(HasLineOfSight)는 모서리 스침 허용"이라는 규칙 분리를 이 함수의 유무가 나타낸다.
//   순수 기하 판정이라 스텝 크기 무관 - 같은 궤적을 4px 로 쪼개든 23px 로 쪼개든 결과가 같다
//   (예전 "from 셀 -> next 셀 대각 전이" 분류는 스텝 크기에 따라 판정이 갈렸다).
//   A* 대각 이웃도 이 규칙과 동일해야 계산한 경로를 이동이 그대로 밟을 수 있다 (같은 규약 = desync 봉인).
//   축평행 선분(dx==0 or dy==0)은 코너 컷 불가라 즉시 통과. 맵 밖 이웃 칸은 경계 클램프 소관이라 벽으로 안 친다.
static bool HasDiagonalCornerCut(int mapId, const Position& from, const Position& to, int w, int h)
{
    const long long dx = to.x - from.x;
    const long long dy = to.y - from.y;
    if (dx == 0 || dy == 0) { return false; }

    const int cols = w / WALK_CELL_SIZE;
    const int rows = h / WALK_CELL_SIZE;
    const bool sameSign = (dx > 0) == (dy > 0);   // 왼위<->오른아래 대각(true) vs 오른위<->왼아래(false)

    const int minX = (from.x < to.x) ? from.x : to.x;
    const int maxX = (from.x < to.x) ? to.x : from.x;
    const int minY = (from.y < to.y) ? from.y : to.y;
    const int maxY = (from.y < to.y) ? to.y : from.y;

    // 두 점 사이의 "내부" 격자 코너만 후보 (AdvanceMove 한 걸음<=~10px·A* 한 칸 50px 라 후보 극소).
    for (int cx = minX / WALK_CELL_SIZE + 1; cx <= maxX / WALK_CELL_SIZE; ++cx)
    {
        const long long X = static_cast<long long>(cx) * WALK_CELL_SIZE;
        for (int cy = minY / WALK_CELL_SIZE + 1; cy <= maxY / WALK_CELL_SIZE; ++cy)
        {
            const long long Y = static_cast<long long>(cy) * WALK_CELL_SIZE;
            if (dx * (Y - from.y) - dy * (X - from.x) != 0) { continue; }   // 코너가 선분 위 아님 - 대각 통과 아님

            // 코너를 낀 두 직교 이웃 (대각 통과쌍의 옆칸). sameSign 이면 오른위+왼아래, 아니면 왼위+오른아래.
            int colA, rowA, colB, rowB;
            if (sameSign) { colA = cx;     rowA = cy - 1; colB = cx - 1; rowB = cy; }
            else          { colA = cx - 1; rowA = cy - 1; colB = cx;     rowB = cy; }

            const bool aWall = (colA >= 0 && colA < cols && rowA >= 0 && rowA < rows) && !CellWalkable(mapId, colA, rowA);
            const bool bWall = (colB >= 0 && colB < cols && rowB >= 0 && rowB < rows) && !CellWalkable(mapId, colB, rowB);
            if (aWall || bWall) { return true; }   // 직교 이웃 하나라도 벽 = 코너 컷 차단
        }
    }
    return false;
}

// Data/ 밑의 파일 실제 경로를 찾는다 (exe 기준 0..5단계 위 + 작업 폴더 폴백 - 다른 표 로더와 같은 탐색).
//   .bin 캐시 경로는 여기서 찾은 CSV 경로에서 확장자만 바꿔 만들므로(짝 고정) 서로 다른 디렉터리의
//   파일끼리 짝지어져 엉뚱한 캐시를 신선하다고 믿는 사고가 없다.
static bool ResolveDataPath(const wchar_t* fileName, wchar_t* outPath, size_t outCap)
{
    wchar_t exeDir[MAX_PATH];
    const DWORD len = ::GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
    {
        wchar_t* slash = wcsrchr(exeDir, L'\\');
        if (slash != nullptr) { *slash = L'\0'; }

        static const wchar_t* const relFromExe[] =
        {
            L"%s\\Data\\%s",
            L"%s\\..\\Data\\%s",
            L"%s\\..\\..\\Data\\%s",
            L"%s\\..\\..\\..\\Data\\%s",
            L"%s\\..\\..\\..\\..\\Data\\%s",
            L"%s\\..\\..\\..\\..\\..\\Data\\%s",
        };
        for (int i = 0; i < 6; ++i)
        {
            if (swprintf_s(outPath, outCap, relFromExe[i], exeDir, fileName) > 0)
            {
                WIN32_FILE_ATTRIBUTE_DATA attr;
                if (::GetFileAttributesExW(outPath, GetFileExInfoStandard, &attr)) { return true; }
            }
        }
    }

    static const wchar_t* const cwdCandidates[] = { L"Data\\%s", L"..\\Data\\%s" };
    for (int i = 0; i < 2; ++i)
    {
        if (swprintf_s(outPath, outCap, cwdCandidates[i], fileName) > 0)
        {
            WIN32_FILE_ATTRIBUTE_DATA attr;
            if (::GetFileAttributesExW(outPath, GetFileExInfoStandard, &attr)) { return true; }
        }
    }
    return false;
}

// 파일의 마지막 수정 시각 (FILETIME 64bit. 실패 = 0)
static UINT64 FileWriteTime(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!::GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) { return 0; }
    ULARGE_INTEGER t;
    t.LowPart  = attr.ftLastWriteTime.dwLowDateTime;
    t.HighPart = attr.ftLastWriteTime.dwHighDateTime;
    return t.QuadPart;
}

// .bin 캐시 적재 시도. 헤더(magic/크기/원본 CSV 수정시각)가 하나라도 어긋나면 false -
//   호출자가 CSV 재파싱으로 흡수한다 (캐시는 순수 최적화 - 손상이 부팅 거부가 되면 안 됨).
static bool TryLoadWalkBin(const wchar_t* binPath, int mapId, int width, int height, UINT64 csvWriteTime)
{
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, binPath, L"rb") != 0 || fp == nullptr) { return false; }

    UINT32 magic = 0;
    int    w = 0;
    int    h = 0;
    UINT64 src = 0;
    bool ok = fread(&magic, 4, 1, fp) == 1 && fread(&w, 4, 1, fp) == 1 &&
              fread(&h, 4, 1, fp) == 1 && fread(&src, 8, 1, fp) == 1;
    ok = ok && (magic == WALK_BIN_MAGIC) && (w == width) && (h == height) && (src == csvWriteTime);

    if (ok)
    {
        const size_t rowBytes = static_cast<size_t>(height / WALK_CELL_SIZE) * WALK_ROW_STRIDE_BYTES;
        ok = fread(g_walkBits[mapId], 1, rowBytes, fp) == rowBytes;
        if (ok)
        {
            BYTE extra = 0;
            ok = (fread(&extra, 1, 1, fp) == 0);   // 기대 크기보다 길면 손상(절단/이어붙임) - 캐시 불신
        }
    }
    fclose(fp);
    return ok;
}

// .bin 캐시 기록 - 임시 파일에 다 쓴 뒤 원자 교체. 서버/봇/클라가 동시에 떠서 같이 재생성해도
//   서로의 파일을 찢지 않는다 (교체에 진 쪽 파일도 완전본이라 무해). 실패는 경고만 - 캐시 없이도 CSV 로 동작.
static void SaveWalkBin(const wchar_t* binPath, int mapId, int width, int height, UINT64 csvWriteTime)
{
    wchar_t tmpPath[MAX_PATH];
    if (swprintf_s(tmpPath, L"%s.%u.tmp", binPath, ::GetCurrentProcessId()) <= 0) { return; }

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, tmpPath, L"wb") != 0 || fp == nullptr)
    {
        PrintWalkError(L"캐시 쓰기 실패 - 무시하고 진행: %s", binPath);
        return;
    }

    const size_t rowBytes = static_cast<size_t>(height / WALK_CELL_SIZE) * WALK_ROW_STRIDE_BYTES;
    bool ok = fwrite(&WALK_BIN_MAGIC, 4, 1, fp) == 1 && fwrite(&width, 4, 1, fp) == 1 &&
              fwrite(&height, 4, 1, fp) == 1 && fwrite(&csvWriteTime, 8, 1, fp) == 1 &&
              fwrite(g_walkBits[mapId], 1, rowBytes, fp) == rowBytes;
    fclose(fp);

    if (!ok || !::MoveFileExW(tmpPath, binPath, MOVEFILE_REPLACE_EXISTING))
    {
        ::DeleteFileW(tmpPath);
        PrintWalkError(L"캐시 교체 실패 - 무시하고 진행: %s", binPath);
    }
}

// 통행 CSV(정본) 파싱 - '0'/'1' 문자가 폭/50 자 x 높이/50 줄. 주석(#)/빈 줄 허용, CRLF/LF 모두 허용.
//   행 수/행 길이/문자가 하나라도 어긋나면 실패 (조용한 무시 없이 부팅 거부 - 다른 표 로더와 같은 원칙).
static bool ParseWalkCsv(const wchar_t* csvPath, int mapId, int width, int height)
{
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, csvPath, L"rt") != 0 || fp == nullptr)
    {
        PrintWalkError(L"파일 열기 실패: %s", csvPath);
        return false;
    }

    const int cols = width / WALK_CELL_SIZE;
    const int rows = height / WALK_CELL_SIZE;
    char line[256];   // 데이터 행(최대 80자+개행)은 넉넉하고, 더 긴 줄은 주석뿐 - 아래 잘린 줄 처리로 소비
    int row = 0;
    bool valid = true;
    bool firstLine = true;
    bool skipRestOfLongLine = false;   // 버퍼보다 긴 줄(한글 주석 등)의 잘린 뒷부분을 데이터 행으로 오인하지 않게 버린다

    while (valid && fgets(line, sizeof(line), fp) != nullptr)
    {
        // fgets 가 개행 없이 꽉 찼다 = 줄이 버퍼보다 길어 잘렸다 (다음 fgets 는 같은 줄의 나머지).
        const bool lineComplete = (strchr(line, '\n') != nullptr) || (feof(fp) != 0);
        if (skipRestOfLongLine)
        {
            skipRestOfLongLine = !lineComplete;   // 잘린 줄의 나머지 - 줄 끝을 만날 때까지 버림
            continue;
        }
        skipRestOfLongLine = !lineComplete;

        char* text = line;
        if (firstLine)   // 파일 선두 UTF-8 BOM 은 건너뜀 (에디터가 붙였을 때 첫 행 오진 방지)
        {
            firstLine = false;
            if (static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
                static_cast<unsigned char>(text[2]) == 0xBF) { text += 3; }
        }
        if (text[0] == '#' || text[0] == '\n' || text[0] == '\r' || text[0] == '\0') { continue; }   // 주석/빈 줄

        if (row >= rows)
        {
            PrintWalkError(L"%s - 행이 %d줄보다 많음 (크기 %dx%d 와 불일치)", csvPath, rows, width, height);
            valid = false; break;
        }

        int col = 0;
        for (; text[col] != '\0' && text[col] != '\n' && text[col] != '\r'; ++col)
        {
            if (col >= cols)
            {
                PrintWalkError(L"%s - %d행이 %d자보다 김", csvPath, row, cols);
                valid = false; break;
            }
            if (text[col] == '1')
            {
                g_walkBits[mapId][row * WALK_ROW_STRIDE_BYTES + col / 8] |= static_cast<BYTE>(1u << (col % 8));
            }
            else if (text[col] != '0')
            {
                PrintWalkError(L"%s - %d행 %d번째 문자가 '0'/'1' 이 아님", csvPath, row, col);
                valid = false; break;
            }
        }
        if (valid && col != cols)
        {
            PrintWalkError(L"%s - %d행이 %d자가 아님 (%d자)", csvPath, row, cols, col);
            valid = false; break;
        }
        ++row;
    }
    fclose(fp);

    if (valid && row != rows)
    {
        PrintWalkError(L"%s - 행 수 %d (기대 %d - 크기 %dx%d)", csvPath, row, rows, width, height);
        valid = false;
    }
    return valid;
}

int LoadWalkableTable()
{
    g_walkMapCount = 0;
    memset(g_walkBits, 0, sizeof(g_walkBits));

    const int mapCount = MapTableCount();
    if (mapCount <= 0)
    {
        PrintWalkError(L"maps.csv 미적재 - LoadMapTable 성공 뒤에 호출해야 함");
        return -1;
    }

    for (int i = 0; i < mapCount; ++i)
    {
        const MapDef& def = MapTableAt(i);

        if (wcscmp(def.walkableFile, L"-") == 0)
        {
            memset(g_walkBits[i], 0xFF, WALK_BYTES_PER_MAP);   // 벽 없는 맵 선언 - 전 칸 통행
            continue;
        }

        // 파일명이 적혔는데 못 찾으면 부팅 거부 - 오타 파일명이 조용히 "벽 없는 맵"이 되는 사고 차단
        //   ("-" 와 파일명의 이분법. 벽 없는 맵은 반드시 "-" 로 선언).
        wchar_t csvPath[MAX_PATH];
        if (!ResolveDataPath(def.walkableFile, csvPath, MAX_PATH))
        {
            PrintWalkError(L"맵 %d - 통행 파일 %s 을 찾지 못함 (Data/ 확인. 벽 없는 맵이면 '-')", i, def.walkableFile);
            return -1;
        }

        // .bin 캐시 경로 = 찾은 CSV 경로에서 확장자만 교체 (같은 폴더에 짝으로 고정).
        //   점 탐색은 마지막 경로 구분자 뒤(파일명)로 한정한다 - 확장자 없는 파일명이면 경로 성분 "..\" 의
        //   점이 잡혀 경로 중간이 잘린 엉뚱한 위치에 캐시가 생긴다.
        wchar_t binPath[MAX_PATH];
        wcscpy_s(binPath, csvPath);
        wchar_t* fileNamePart = wcsrchr(binPath, L'\\');
        fileNamePart = (fileNamePart != nullptr) ? (fileNamePart + 1) : binPath;
        wchar_t* dot = wcsrchr(fileNamePart, L'.');
        if (dot != nullptr) { *dot = L'\0'; }
        wcscat_s(binPath, L".bin");

        const UINT64 csvTime = FileWriteTime(csvPath);
        if (!TryLoadWalkBin(binPath, i, def.width, def.height, csvTime))
        {
            memset(g_walkBits[i], 0, WALK_BYTES_PER_MAP);   // 캐시 부분 적재 흔적 제거 - 정본(CSV)에서 다시
            if (!ParseWalkCsv(csvPath, i, def.width, def.height)) { return -1; }
            SaveWalkBin(binPath, i, def.width, def.height, csvTime);
        }
    }

    g_walkMapCount = mapCount;

    // 부활 좌표가 벽 위면 부활 즉시 끼임 - 격자가 준비된 지금 검증 (maps.csv 적재 시점엔 격자가 없어 여기로 미룸)
    for (int i = 0; i < mapCount; ++i)
    {
        const Position rp = MapTableAt(i).respawn;
        if (!IsWalkable(i, rp.x, rp.y))
        {
            PrintWalkError(L"맵 %d - 부활 좌표 (%d,%d) 가 벽 위", i, rp.x, rp.y);
            g_walkMapCount = 0;
            return -1;
        }
    }

    return mapCount;
}

bool IsWalkableTableLoaded()
{
    return g_walkMapCount > 0;
}

bool IsWalkable(int mapId, int x, int y)
{
    if (g_walkMapCount <= 0) { return true; }                     // 미적재 = 전-통행 (GameClient 관대 폴백)
    if (mapId < 0 || mapId >= g_walkMapCount) { return false; }   // 손상 mapId 방어 (배열 밖 비트 읽기 차단)

    const int w = MapWidthFor(mapId);
    const int h = MapHeightFor(mapId);
    if (x < 0 || x > w || y < 0 || y > h) { return false; }       // 맵 밖 = 비통행

    return CellWalkable(mapId, CellIndexOf(x, w), CellIndexOf(y, h));
}

// 선분이 지나는 칸(supercover) 중 벽이 하나라도 있는가 - 이동/시야 공용 하부(코너 컷 무관).
//   두 점을 덮는 칸 사각형 안에서, 선분이 "실제로 지나는" 칸만 검사한다 - 사각형 전체를 검사하면
//   대각 선분(8방 이동의 일상)이 벽 모서리 옆을 지날 때 직선이 안 닿는 모서리 벽 칸 때문에 오거부가 난다.
//   허용 오차 64px 구간은 최대 3x3칸 = O(1). 같은 점(from==to)은 칸 1개로 자연 처리.
static bool AnyWallOnSegment(int mapId, const Position& from, const Position& to, int w, int h)
{
    int c0 = CellIndexOf(from.x, w);
    int c1 = CellIndexOf(to.x, w);
    int r0 = CellIndexOf(from.y, h);
    int r1 = CellIndexOf(to.y, h);
    if (c0 > c1) { const int t = c0; c0 = c1; c1 = t; }
    if (r0 > r1) { const int t = r0; r0 = r1; r1 = t; }

    for (int r = r0; r <= r1; ++r)
    {
        for (int c = c0; c <= c1; ++c)
        {
            if (!SegmentTouchesCell(from, to, c, r)) { continue; }   // 직선이 안 지나는 모서리 칸 - 판정 제외
            if (!CellWalkable(mapId, c, r)) { return true; }
        }
    }
    return false;
}

bool IsPathWalkable(int mapId, const Position& from, const Position& to)
{
    if (g_walkMapCount <= 0) { return true; }
    if (mapId < 0 || mapId >= g_walkMapCount) { return false; }

    const int w = MapWidthFor(mapId);
    const int h = MapHeightFor(mapId);
    if (from.x < 0 || from.x > w || from.y < 0 || from.y > h) { return false; }
    if (to.x < 0 || to.x > w || to.y < 0 || to.y > h) { return false; }

    // 이동: 지나는 칸(supercover) + 대각 코너 컷 (이동 스텝/보고 수용/A* 공통 규약).
    if (AnyWallOnSegment(mapId, from, to, w, h)) { return false; }
    if (HasDiagonalCornerCut(mapId, from, to, w, h)) { return false; }
    return true;
}

bool HasLineOfSight(int mapId, const Position& from, const Position& to)
{
    if (g_walkMapCount <= 0) { return true; }                     // 미적재 = 벽 모름 = 시야 통과(전-통행 폴백)
    if (mapId < 0 || mapId >= g_walkMapCount) { return false; }

    const int w = MapWidthFor(mapId);
    const int h = MapHeightFor(mapId);
    if (from.x < 0 || from.x > w || from.y < 0 || from.y > h) { return false; }
    if (to.x < 0 || to.x > w || to.y < 0 || to.y > h) { return false; }

    // 시야: 지나는 칸에 벽이 있으면 가림. 코너 컷은 "이동" 규칙이라 시야엔 무관
    //   (벽 모서리를 스쳐 보거나 쏘는 것은 허용 - 화살/시선은 몸을 밀어 넣지 않는다).
    return !AnyWallOnSegment(mapId, from, to, w, h);
}

bool IsCellWalkable(int mapId, int col, int row)
{
    if (g_walkMapCount <= 0) { return true; }                     // 미적재 = 전-통행 폴백
    if (mapId < 0 || mapId >= g_walkMapCount) { return false; }
    const int cols = MapWidthFor(mapId) / WALK_CELL_SIZE;
    const int rows = MapHeightFor(mapId) / WALK_CELL_SIZE;
    if (col < 0 || col >= cols || row < 0 || row >= rows) { return false; }   // 격자 밖 = 비통행
    return CellWalkable(mapId, col, row);
}

int WalkCellCols(int mapId) { return MapWidthFor(mapId) / WALK_CELL_SIZE; }
int WalkCellRows(int mapId) { return MapHeightFor(mapId) / WALK_CELL_SIZE; }

bool IsReachable(int mapId, const Position& from, const Position& to)
{
    if (g_walkMapCount <= 0)                      { return true; }   // 미적재 = 전-통행 (격리 개념 없음)
    if (mapId < 0 || mapId >= g_walkMapCount)     { return true; }   // 범위 밖 = 폴백(도달 가능 취급) - IsWalkable 은 false 라 이 함수만 비대칭(호출부가 mapId 선검증 전제)

    const int cols = MapWidthFor(mapId) / WALK_CELL_SIZE;
    const int fromCol = CellIndexOf(from.x, MapWidthFor(mapId));
    const int fromRow = CellIndexOf(from.y, MapHeightFor(mapId));
    const int toCol   = CellIndexOf(to.x,   MapWidthFor(mapId));
    const int toRow   = CellIndexOf(to.y,   MapHeightFor(mapId));

    // 출발/도착 칸 자체가 벽이면 도달 불가 (per-point 통행 검증이 앞서지만 방어적으로 재확인)
    if (!IsCellWalkable(mapId, fromCol, fromRow)) { return false; }
    if (!IsCellWalkable(mapId, toCol, toRow))     { return false; }
    if (fromCol == toCol && fromRow == toRow)     { return true; }

    // 4방 BFS - 고정 배열 큐/방문표 (부팅 1회·최대 6400칸이라 스택 배치·힙 할당 0).
    //   각 칸은 visited 로 한 번만 큐에 들어가므로 큐 길이 <= 칸 수 <= MAX_CELLS (오버플로 없음).
    static const int MAX_CELLS = WALK_CELLS_MAX * WALK_CELLS_MAX;
    bool visited[MAX_CELLS];
    int  queue[MAX_CELLS];
    const int used = cols * (MapHeightFor(mapId) / WALK_CELL_SIZE);
    memset(visited, 0, sizeof(bool) * used);   // 실제 쓰는 범위(cols*rows)만 초기화

    int head = 0, tail = 0;
    const int startIdx = fromRow * cols + fromCol;
    visited[startIdx] = true;
    queue[tail++] = startIdx;

    static const int NX[4] = { 1, -1, 0, 0 };
    static const int NY[4] = { 0, 0, 1, -1 };

    while (head < tail)
    {
        const int cur = queue[head++];
        const int cc = cur % cols;
        const int cr = cur / cols;
        for (int d = 0; d < 4; ++d)
        {
            const int nc = cc + NX[d];
            const int nr = cr + NY[d];
            if (!IsCellWalkable(mapId, nc, nr)) { continue; }   // 격자 밖/벽 = 확장 안 함
            if (nc == toCol && nr == toRow)     { return true; }   // 도착 - 조기 종료
            const int nidx = nr * cols + nc;
            if (visited[nidx]) { continue; }
            visited[nidx] = true;
            queue[tail++] = nidx;
        }
    }
    return false;   // flood 소진했는데 to 미도달 = 격리 영역
}

Position AdvanceMove(int mapId, const Position& from, EMoveDirection dir, int speedPxPerSec, float dt)
{
    const int d = static_cast<int>(dir);
    if (d < 0 || d >= static_cast<int>(EMoveDirection::COUNT)) { return from; }   // 손상 방향 방어(0..7) - 이동 없음

    // dead-reckoning 전진: 방향 단위벡터 * 속도 * dt 를 int 절단 - 서버/클라/봇이 같은 비트 결과를 내는 공용 수식.
    //   대각은 단위벡터가 ±0.7071 이라 대각 이동이 직교보다 빠르지 않다.
    Position next = from;
    next.x += static_cast<int>(MOVE_DIR_DELTA[d][0] * speedPxPerSec * dt);
    next.y += static_cast<int>(MOVE_DIR_DELTA[d][1] * speedPxPerSec * dt);

    // 맵 경계 클램프 (맵마다 크기가 다름 - 표 미적재 클라는 접근자가 4000 을 돌려줘 옛 경계로 동작)
    const int w = MapWidthFor(mapId);
    const int h = MapHeightFor(mapId);
    next.x = (next.x < 0) ? 0 : ((next.x > w) ? w : next.x);
    next.y = (next.y < 0) ? 0 : ((next.y > h) ? h : next.y);

    // from->next 선분이 벽을 지나거나 대각 코너를 컷하면 제자리(from 그대로) - 끝점만 보던 예전과 달리
    //   지나는 칸 전부 + 코너 규칙을 본다(대각 스텝이 벽 모서리로 삐져 들어가는 것을 봉인). 보고 수용 게이트·
    //   A* 이웃과 같은 함수(IsPathWalkable)라 세 곳 규약이 구조로 일치한다.
    //   호출자는 "반환값 == from" 으로 벽/경계에 막혔음을 안다 (몬스터는 이걸로 STOP 전이 판단).
    if (!IsPathWalkable(mapId, from, next)) { return from; }
    return next;
}
