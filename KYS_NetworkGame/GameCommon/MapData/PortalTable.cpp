#include "pch_gamecommon.h"
#include "PortalTable.h"
#include <cstdio>      // _wfopen_s / fgets / sscanf_s / fclose / swprintf_s
#include <cwchar>      // wcsrchr
#include <Windows.h>   // GetModuleFileNameW / MAX_PATH

// 포탈 표는 부팅에 한 번 적재 후 읽기 전용 - 고정 배열(게임 서버 예측 가능 메모리).
static const int MAX_PORTALS = 64;
static PortalDef g_portals[MAX_PORTALS];
static int       g_portalCount = 0;

// portals.csv 를 열어 FILE* 반환 (실패 = nullptr). working dir 에 의존하지 않는다.
//   exe 위치 기준으로 찾는다 - exe 는 Build\x64\<Config>\ 에 있고 Data 는 repo 루트라 보통 3단계 위.
//   exe 디렉터리에서 위로 0..5 단계를 훑어 어디서 실행하든(출력 폴더 / 프로젝트 폴더 / repo 루트) 같은 파일을 찾는다.
static FILE* OpenPortalFile()
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
            L"\\Data\\portals.csv",
            L"\\..\\Data\\portals.csv",
            L"\\..\\..\\Data\\portals.csv",
            L"\\..\\..\\..\\Data\\portals.csv",          // Build\x64\<Config>\ -> repo 루트 (정상 경로)
            L"\\..\\..\\..\\..\\Data\\portals.csv",
            L"\\..\\..\\..\\..\\..\\Data\\portals.csv",
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

    // (2) 폴백: working dir 기준 (repo 루트 / 프로젝트 폴더에서 직접 실행하는 경우)
    static const wchar_t* const cwdCandidates[] = { L"Data\\portals.csv", L"..\\Data\\portals.csv" };
    for (int i = 0; i < 2; ++i)
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, cwdCandidates[i], L"rt") == 0 && fp != nullptr) { return fp; }
    }
    return nullptr;
}

int LoadPortalTable()
{
    FILE* fp = OpenPortalFile();
    if (fp == nullptr) { return -1; }   // 어느 위치에서도 못 찾음 = fail-fast

    g_portalCount = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) != nullptr)
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0') { continue; }   // 주석/빈 줄
        if (g_portalCount >= MAX_PORTALS) { break; }   // 표 가득 - 나머지 줄 무시
        PortalDef& d = g_portals[g_portalCount];
        const int fields = sscanf_s(line, "%d,%d,%d,%d,%d,%d,%d",
            &d.srcMapId, &d.trigger.x, &d.trigger.y, &d.triggerRadius,
            &d.dstMapId, &d.dst.x, &d.dst.y);
        if (fields == 7) { ++g_portalCount; }   // 7필드 정상 줄만 채택 (불완전 줄 무시)
    }
    fclose(fp);
    return (g_portalCount > 0) ? g_portalCount : -1;   // 0행 = 적재 실패 취급
}

int PortalTableCount()
{
    return g_portalCount;
}

const PortalDef& PortalTableAt(int portalId)
{
    return g_portals[portalId];
}
