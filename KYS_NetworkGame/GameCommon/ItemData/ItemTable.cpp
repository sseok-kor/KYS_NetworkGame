#include "pch_gamecommon.h"
#include "ItemTable.h"
#include <cstdio>      // _wfopen_s / fgets / fclose / swprintf_s
#include <cstdlib>     // strtol
#include <cstring>     // strchr / strlen
#include <cwchar>      // wcsrchr
#include <Windows.h>   // GetModuleFileNameW / MultiByteToWideChar / MAX_PATH

// 아이템 사전은 부팅에 한 번 적재 후 읽기 전용 - 고정 배열(게임 서버 예측 가능 메모리).
static const int MAX_ITEM_DEFS = 128;
static ItemDef g_items[MAX_ITEM_DEFS];
static int     g_itemCount = 0;

// items.csv 를 열어 FILE* 반환 (실패 = nullptr). PortalTable 과 같은 탐색 -
//   exe 디렉터리 기준 0..5 단계 위를 훑고, 실패하면 working dir 폴백.
static FILE* OpenItemFile()
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
            L"\\Data\\items.csv",
            L"\\..\\Data\\items.csv",
            L"\\..\\..\\Data\\items.csv",
            L"\\..\\..\\..\\Data\\items.csv",          // Build\x64\<Config>\ -> repo 루트 (정상 경로)
            L"\\..\\..\\..\\..\\Data\\items.csv",
            L"\\..\\..\\..\\..\\..\\Data\\items.csv",
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
    static const wchar_t* const cwdCandidates[] = { L"Data\\items.csv", L"..\\Data\\items.csv" };
    for (int i = 0; i < 2; ++i)
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, cwdCandidates[i], L"rt") == 0 && fp != nullptr) { return fp; }
    }
    return nullptr;
}

// cursor 에서 다음 콤마 전까지를 한 필드로 잘라 앞뒤 공백을 벗겨 outField 에 준다.
//   반환 = 다음 필드 시작 위치 (마지막 필드였으면 nullptr).
//   이름(문자열) 컬럼이 있어서 PortalTable 의 sscanf_s 정수 전용 파싱 대신 수동 분리를 쓴다.
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

int LoadItemTable()
{
    FILE* fp = OpenItemFile();
    if (fp == nullptr) { return -1; }   // 어느 위치에서도 못 찾음 = fail-fast (서버는 부팅 중단)

    g_itemCount = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) != nullptr)
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0') { continue; }   // 주석/빈 줄
        if (g_itemCount >= MAX_ITEM_DEFS) { break; }   // 표 가득 - 나머지 줄 무시

        // 컬럼 7개: templateId, name, type, atkPower, defPower, hpRestore, stackMax
        char* fields[7] = {};
        char* cursor = line;
        int fieldCount = 0;
        while (fieldCount < 7 && cursor != nullptr)
        {
            cursor = SplitField(cursor, &fields[fieldCount]);
            ++fieldCount;
        }
        if (fieldCount < 7) { continue; }   // 필드 부족 줄 무시 (불완전 줄)

        ItemDef& d = g_items[g_itemCount];
        d.templateId = (int)strtol(fields[0], nullptr, 10);

        // 이름은 UTF-8 -> wchar 변환 (파일은 UTF-8, 게임 문자열 규약은 wchar_t)
        const int converted = ::MultiByteToWideChar(CP_UTF8, 0, fields[1], -1, d.name, ITEM_NAME_MAX);
        if (converted == 0) { d.name[0] = L'?'; d.name[1] = L'\0'; }   // 변환 실패(너무 길거나 깨진 바이트) - 자리표시
        d.name[ITEM_NAME_MAX - 1] = L'\0';

        const int typeValue = (int)strtol(fields[2], nullptr, 10);
        if (typeValue < 0 || typeValue >= (int)EItemType::COUNT) { continue; }   // 모르는 종류 줄 무시
        d.itemType  = (EItemType)typeValue;
        d.atkPower  = (int)strtol(fields[3], nullptr, 10);
        d.defPower  = (int)strtol(fields[4], nullptr, 10);
        d.hpRestore = (int)strtol(fields[5], nullptr, 10);
        d.stackMax  = (int)strtol(fields[6], nullptr, 10);

        if (d.templateId <= 0 || d.stackMax <= 0) { continue; }        // 값 검증 실패 줄 무시
        if (FindItemDef(d.templateId) != nullptr) { continue; }        // 중복 templateId - 첫 행 우선
        ++g_itemCount;
    }
    fclose(fp);
    return (g_itemCount > 0) ? g_itemCount : -1;   // 0행 = 적재 실패 취급
}

int ItemTableCount()
{
    return g_itemCount;
}

const ItemDef* FindItemDef(int templateId)
{
    for (int i = 0; i < g_itemCount; ++i)
    {
        if (g_items[i].templateId == templateId) { return &g_items[i]; }
    }
    return nullptr;
}
