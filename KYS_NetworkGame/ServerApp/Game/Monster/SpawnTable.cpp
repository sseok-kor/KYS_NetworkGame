#include "pch_serverapp.h"
#include "SpawnTable.h"
#include "MonsterTable.h"                          // MonsterIsBoss/MonsterCapFor/MonsterTemplateFor (보스 cap/어그로 교차검증)
#include "../../../GameCommon/MapData/MapTable.h"          // MapTableCount/MapTableAt (mapId 범위/맵 크기/monsterCap/부활 좌표 교차검증)
#include "../../../GameCommon/MapData/WalkableTable.h"     // IsWalkable (구역 안 통행 칸 존재 교차검증 - 스폰 불가 구역 부팅 거부)
#include "../../../GameCommon/GameDefines.h"       // WALK_CELL_SIZE/MAX_GROUP_COUNT/MAX_MAP_COUNT/DEFAULT_MONSTER_POOL_CAPACITY
#include "../../../GameServer/Core/Log/Logger.h"   // 부팅 fatal 파일 로그
#include <Windows.h>   // GetModuleFileNameW / MAX_PATH
#include <cstdio>      // _wfopen_s / fgets / sscanf_s / fclose / swprintf_s
#include <cwchar>      // wcsrchr

namespace KYS
{
    namespace SERVERAPP
    {
        // 스폰 그룹 표는 부팅에 한 번 적재 후 읽기 전용 - 고정 배열 (상한 = GameDefines MAX_SPAWN_GROUPS, MapManager 런타임 배열과 공유).
        static SpawnGroup g_spawnGroups[MAX_SPAWN_GROUPS];
        static int        g_spawnGroupCount = 0;

        // spawns.csv 를 열어 FILE* 반환 (실패 = nullptr). PortalTable/ItemTable 과 같은 탐색.
        static FILE* OpenSpawnFile()
        {
            wchar_t exeDir[MAX_PATH];
            const DWORD len = GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
            if (len > 0 && len < MAX_PATH)
            {
                wchar_t* slash = wcsrchr(exeDir, L'\\');
                if (slash != nullptr) { *slash = L'\0'; }

                static const wchar_t* const relFromExe[] =
                {
                    L"\\Data\\spawns.csv",
                    L"\\..\\Data\\spawns.csv",
                    L"\\..\\..\\Data\\spawns.csv",
                    L"\\..\\..\\..\\Data\\spawns.csv",          // Build\x64\<Config>\ -> repo 루트 (정상 경로)
                    L"\\..\\..\\..\\..\\Data\\spawns.csv",
                    L"\\..\\..\\..\\..\\..\\Data\\spawns.csv",
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
            static const wchar_t* const cwdCandidates[] = { L"Data\\spawns.csv", L"..\\Data\\spawns.csv" };
            for (int i = 0; i < 2; ++i)
            {
                FILE* fp = nullptr;
                if (_wfopen_s(&fp, cwdCandidates[i], L"rt") == 0 && fp != nullptr) { return fp; }
            }
            return nullptr;
        }

        // 부팅 fatal 로그 축약 (MonsterTable 과 동형 - 실패 사유를 파일 로그에 남긴다).
        template <typename... Args>
        static void LogBootFatal(const wchar_t* fmt, Args... args)
        {
            KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_FATAL, L"boot",
                fmt, args...);
        }

        // 사각 구역에서 한 점까지의 최단 거리 제곱 - 구역이 부활 지점의 어그로 반경 안까지 다가왔는지 판정.
        //   점이 구역 안이면 0. long long 곱으로 int 오버플로 방지.
        static long long RectPointDistSq(int x0, int y0, int x1, int y1, int px, int py)
        {
            int dx = 0;
            if (px < x0) { dx = x0 - px; } else if (px > x1) { dx = px - x1; }
            int dy = 0;
            if (py < y0) { dy = y0 - py; } else if (py > y1) { dy = py - y1; }
            return static_cast<long long>(dx) * dx + static_cast<long long>(dy) * dy;
        }

        int LoadSpawnTable()
        {
            FILE* fp = OpenSpawnFile();
            if (fp == nullptr)
            {
                LogBootFatal(L"spawns.csv - 파일을 찾지 못함 (Data/spawns.csv 확인)");
                return -1;
            }

            g_spawnGroupCount = 0;
            char line[256];
            while (fgets(line, sizeof(line), fp) != nullptr)
            {
                // 파일 선두 UTF-8 BOM(EF BB BF)은 건너뛴다 - 에디터가 붙였을 때 첫 행이 형식 오류로 오진되는 것 방지.
                char* text = line;
                if (static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
                    static_cast<unsigned char>(text[2]) == 0xBF) { text += 3; }
                if (text[0] == '#' || text[0] == '\n' || text[0] == '\r' || text[0] == '\0') { continue; }   // 주석/빈 줄
                if (g_spawnGroupCount >= MAX_SPAWN_GROUPS)
                {
                    LogBootFatal(L"spawns.csv - 스폰 그룹 상한(%d) 초과", MAX_SPAWN_GROUPS);
                    fclose(fp);
                    return -1;   // 조용한 절단 대신 부팅 거부 (뒤쪽 행 무음 소실 방지)
                }

                // 컬럼 8개: mapId, type, x0, y0, x1, y1, count, respawnDelayMs
                int mapId = 0, type = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, count = 0, respawnDelayMs = 0;
                const int fields = sscanf_s(text, "%d,%d,%d,%d,%d,%d,%d,%d",
                    &mapId, &type, &x0, &y0, &x1, &y1, &count, &respawnDelayMs);
                if (fields != 8)
                {
                    LogBootFatal(L"spawns.csv - 필드 8개가 아닌 줄 (형식 오류)");
                    fclose(fp);
                    return -1;
                }

                // 하드코딩 표는 오타가 컴파일에서 잡혔지만 파일은 아니다 - 위반은 행 무시가 아니라 부팅 거부
                //   (조용히 무시하면 맵 몬스터가 무음으로 줄어든 채 돌게 된다).
                if (mapId < 0 || mapId >= MapTableCount())
                {
                    LogBootFatal(L"spawns.csv - mapId %d 가 맵 표 범위(0..%d) 밖", mapId, MapTableCount() - 1);
                    fclose(fp);
                    return -1;
                }
                if (type < 0 || type >= static_cast<int>(EMonsterType::COUNT))
                {
                    LogBootFatal(L"spawns.csv - type %d 범위 밖 (0..%d)", type, static_cast<int>(EMonsterType::COUNT) - 1);
                    fclose(fp);
                    return -1;
                }
                if (x0 > x1 || y0 > y1)
                {
                    LogBootFatal(L"spawns.csv - 구역 좌표 역전 (%d,%d)~(%d,%d) (x0<=x1, y0<=y1 이어야 함)", x0, y0, x1, y1);
                    fclose(fp);
                    return -1;
                }
                if (x0 < 0 || x1 > MapTableAt(mapId).width || y0 < 0 || y1 > MapTableAt(mapId).height)
                {
                    LogBootFatal(L"spawns.csv - 구역 (%d,%d)~(%d,%d) 이 맵 %d(%dx%d) 밖", x0, y0, x1, y1,
                                 mapId, MapTableAt(mapId).width, MapTableAt(mapId).height);
                    fclose(fp);
                    return -1;
                }
                if (count < 1 || count > MAX_GROUP_COUNT)
                {
                    LogBootFatal(L"spawns.csv - count %d 범위 밖 (1..%d)", count, MAX_GROUP_COUNT);
                    fclose(fp);
                    return -1;
                }
                if (respawnDelayMs < 1)
                {
                    // 0 이면 구버전(점 스폰/"예약 필드") 파일이 남은 것 - 그대로 읽으면 인구 의미가 붕괴하므로 부팅에서 구분한다.
                    LogBootFatal(L"spawns.csv - respawnDelayMs %d (1 이상 필수 - 0행은 구버전 점 스폰 파일)", respawnDelayMs);
                    fclose(fp);
                    return -1;
                }

                // 스폰 구역이 그 맵 부활 지점의 어그로 반경 안까지 오면 부활 직후 즉시 피격 - 데이터 실수를 부팅에서 잡는다.
                const MapDef& mapDef = MapTableAt(mapId);
                const int aggro = MonsterTemplateFor(static_cast<EMonsterType>(type)).aggroRange;
                const long long distSq = RectPointDistSq(x0, y0, x1, y1, mapDef.respawn.x, mapDef.respawn.y);
                if (distSq < static_cast<long long>(aggro) * aggro)
                {
                    LogBootFatal(L"spawns.csv - 맵 %d 구역 (%d,%d)~(%d,%d) 이 부활 지점 (%d,%d) 의 어그로 %d 안 (부활 즉시 피격)",
                        mapId, x0, y0, x1, y1, mapDef.respawn.x, mapDef.respawn.y, aggro);
                    fclose(fp);
                    return -1;
                }

                // 구역 안에 통행 칸이 하나도 없으면 스폰 재추첨이 영원히 실패해 그 그룹 인구가 무음 결손 - 부팅에서 잡는다.
                //   (통행 격자는 LoadWalkableTable 이 이 로더보다 먼저 적재 - main 부팅 순서 계약.)
                //   구역과 겹치는 통행 칸마다 교집합 안 한 점을 골라 검사한다.
                {
                    const int c0 = x0 / WALK_CELL_SIZE;
                    const int c1 = x1 / WALK_CELL_SIZE;
                    const int r0 = y0 / WALK_CELL_SIZE;
                    const int r1 = y1 / WALK_CELL_SIZE;
                    bool hasWalkableSpot = false;
                    for (int r = r0; r <= r1 && !hasWalkableSpot; ++r)
                    {
                        for (int c = c0; c <= c1; ++c)
                        {
                            const int sampleX = (c * WALK_CELL_SIZE < x0) ? x0 : c * WALK_CELL_SIZE;
                            const int sampleY = (r * WALK_CELL_SIZE < y0) ? y0 : r * WALK_CELL_SIZE;
                            if (IsWalkable(mapId, sampleX, sampleY)) { hasWalkableSpot = true; break; }
                        }
                    }
                    if (!hasWalkableSpot)
                    {
                        LogBootFatal(L"spawns.csv - 맵 %d 구역 (%d,%d)~(%d,%d) 안에 통행 칸이 없음 (스폰 불가 구역)",
                            mapId, x0, y0, x1, y1);
                        fclose(fp);
                        return -1;
                    }
                }

                SpawnGroup& group = g_spawnGroups[g_spawnGroupCount];
                group.mapId          = mapId;
                group.monsterType    = static_cast<EMonsterType>(type);
                group.x0 = x0; group.y0 = y0; group.x1 = x1; group.y1 = y1;
                group.count          = count;
                group.respawnDelayMs = respawnDelayMs;
                ++g_spawnGroupCount;
            }
            fclose(fp);

            if (g_spawnGroupCount <= 0)
            {
                LogBootFatal(L"spawns.csv - 0행 (스폰 그룹 최소 1행 필요)");
                return -1;
            }

            // 맵별 count 합 <= monsterCap (cap 은 안전 천장 - 초과 데이터는 튜닝 실수라 부팅 거부. 게임 중 cap 게이트는 이 검증 덕에 발동하지 않는다).
            //   + 종류별 cap 이 있으면(monsters.yml cap > 0) 같은 맵 count 합 <= cap (보스 떼 증식 등을 데이터 단계에서 차단. cap 0 = 상한 없음).
            int totalCount = 0;
            int mapTotals[MAX_MAP_COUNT] = {};   // 부팅 로그용 맵별 인구 (stale 파일/오타를 로그 대조로 잡는 2차 방어선)
            for (int mapId = 0; mapId < MapTableCount(); ++mapId)
            {
                int mapTotal = 0;
                int typeTotal[static_cast<int>(EMonsterType::COUNT)] = {};
                for (int i = 0; i < g_spawnGroupCount; ++i)
                {
                    if (g_spawnGroups[i].mapId != mapId) { continue; }
                    mapTotal += g_spawnGroups[i].count;
                    typeTotal[static_cast<int>(g_spawnGroups[i].monsterType)] += g_spawnGroups[i].count;
                }
                if (mapTotal > MapTableAt(mapId).monsterCap)
                {
                    LogBootFatal(L"spawns.csv - 맵 %d 그룹 count 합 %d 가 monsterCap %d 초과", mapId, mapTotal, MapTableAt(mapId).monsterCap);
                    return -1;
                }
                for (int t = 0; t < static_cast<int>(EMonsterType::COUNT); ++t)
                {
                    const int cap = MonsterCapFor(static_cast<EMonsterType>(t));
                    if (cap > 0 && typeTotal[t] > cap)
                    {
                        LogBootFatal(L"spawns.csv - 맵 %d type %d count 합 %d 가 monsters.yml cap %d 초과", mapId, t, typeTotal[t], cap);
                        return -1;
                    }
                }
                mapTotals[mapId] = mapTotal;
                totalCount += mapTotal;
            }

            // 전체 count 합 <= 몬스터 풀 용량 (맵별 cap 검증으로 이미 성립하지만 - 풀 고갈은 열린 루프에서 영구 결손이라 이중 안전망).
            if (totalCount > static_cast<int>(DEFAULT_MONSTER_POOL_CAPACITY))
            {
                LogBootFatal(L"spawns.csv - 전체 count 합 %d 가 몬스터 풀 용량 %d 초과", totalCount, static_cast<int>(DEFAULT_MONSTER_POOL_CAPACITY));
                return -1;
            }

            // 맵별 인구 요약을 부팅 로그에 남긴다 - 옛 파일이 섞이거나 count 오타가 나면 운영자가 이 줄과 기대치를 대조해 잡는다.
            wchar_t perMapSummary[256] = L"";
            size_t summaryLen = 0;
            for (int mapId = 0; mapId < MapTableCount(); ++mapId)
            {
                wchar_t piece[16];
                swprintf_s(piece, (mapId == 0) ? L"%d" : L"/%d", mapTotals[mapId]);
                if (summaryLen + wcslen(piece) + 1 >= 256) { break; }   // 버퍼 상한 방어 (MAX_MAP_COUNT 16이라 실도달 없음)
                wcscat_s(perMapSummary, piece);
                summaryLen += wcslen(piece);
            }
            KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"boot",
                L"spawns.csv 인구 요약 - 총 %d 마리 (맵별 %s)", totalCount, perMapSummary);

            return g_spawnGroupCount;
        }

        int SpawnGroupCount()
        {
            return g_spawnGroupCount;
        }

        const SpawnGroup* SpawnGroupData()
        {
            return g_spawnGroups;
        }
    }
}
