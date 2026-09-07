#include "pch_serverapp.h"
#include "MonsterTable.h"
#include "../Item/DropTable.h"                     // ClearDropRules / AddDropRule (드랍 저장소 채움)
#include "../../../GameCommon/ItemData/ItemTable.h"         // FindItemDef (드랍 templateId 참조 무결성 검증)
#include "../../../GameCommon/GameDefines.h"       // MAX_MOVE_SPEED (moveSpeed 고정 검증)
#include "../../../GameServer/Core/Log/Logger.h"   // 부팅 fatal 파일 로그
#include <Windows.h>   // GetModuleFileNameW / MultiByteToWideChar / MAX_PATH
#include <cstdio>      // _wfopen_s / fread / fclose / swprintf_s
#include <cwchar>      // wcsrchr

// rapidyaml (vendored 단일 헤더, ThirdParty/rapidyaml) - rAthena 가 쓰는 현행 YAML 파서.
//   서드파티 헤더는 /W4 경고 소유권이 우리에게 없으므로 경고 수준을 내려서 포함한다 (mysql/imgui 와 같은 취급).
//   RYML_SINGLE_HDR_DEFINE_NOW: amalgamated 단일 헤더 규약 - 정의부는 프로젝트에서 이 TU 한 곳만 포함한다.
//   min/max undef: pch 의 <Windows.h> 가 min/max 를 매크로로 정의해 ryml 의 max() 멤버 함수 선언을 깨뜨린다
//   (NOMINMAX 를 pch 에 넣으면 전 프로젝트 파급이라 이 TU 에서만 국소 해제).
#undef min
#undef max
#pragma warning(push, 0)
#define RYML_SINGLE_HDR_DEFINE_NOW
#include <rapidyaml.hpp>
#pragma warning(pop)

namespace KYS
{
    namespace SERVERAPP
    {
        // 종류별 스탯 표 - 부팅에 한 번 적재 후 읽기 전용 (기존 Monster.cpp 하드코딩 kMonsterTemplates 대체).
        static MonsterTemplate g_monsterTemplates[static_cast<int>(EMonsterType::COUNT)];

        // name 은 예약(미사용 - 표시/로그 예정). isBoss/cap 은 스폰 그룹 검증(SpawnTable)과 리스폰 보스 우선 처리(MapManager)가 소비.
        static const int kMonsterNameMax = 16;
        static wchar_t g_monsterNames[static_cast<int>(EMonsterType::COUNT)][kMonsterNameMax];
        static bool    g_monsterIsBoss[static_cast<int>(EMonsterType::COUNT)];
        static int     g_monsterCap[static_cast<int>(EMonsterType::COUNT)];

        // 종류로 스탯 조회 (선언은 Monster.h - 하드코딩 표 시절과 호출부 시그니처 동일 유지).
        const MonsterTemplate& MonsterTemplateFor(EMonsterType type)
        {
            int idx = static_cast<int>(type);
            if (idx < 0 || idx >= static_cast<int>(EMonsterType::COUNT))
            {
                idx = 0;   // 범위 밖 = GOBLIN 기본값 (범위 밖 접근 방지. 부팅 검증이 전 종류 적재를 보장하므로 정상 경로 도달 불가)
            }
            return g_monsterTemplates[idx];
        }

        // 보스 여부 조회 - 스폰 그룹 검증(보스 cap 대조)과 리스폰 보스 우선 처리가 사용.
        bool MonsterIsBoss(EMonsterType type)
        {
            const int idx = static_cast<int>(type);
            if (idx < 0 || idx >= static_cast<int>(EMonsterType::COUNT)) { return false; }
            return g_monsterIsBoss[idx];
        }

        // 종류별 맵당 상한 조회 (0 = 상한 없음 - 잡몹 기본값).
        int MonsterCapFor(EMonsterType type)
        {
            const int idx = static_cast<int>(type);
            if (idx < 0 || idx >= static_cast<int>(EMonsterType::COUNT)) { return 0; }
            return g_monsterCap[idx];
        }

        // YAML 파싱 실패를 부팅 -1 로 되돌리기 위한 신호 - ryml 은 오류 시 에러 콜백을 부르므로
        //   콜백에서 이 타입을 던지고 LoadMonsterTable 이 받아 -1 로 변환한다 (프로세스 abort 방지).
        struct MonsterYamlError {};

        // ryml 은 오류를 3계층(basic/parse/visit)으로 나눠 각각 다른 콜백을 부른다 - 셋 다 교체해야
        //   어느 경로(문법 오류=parse, 값 변환 실패=visit, 그 외=basic)로 와도 abort 대신 부팅 -1 로 내려온다.
        // 오류 위치(줄/칸)는 아래 static 에 남겨 fatal 로그에 동봉한다 (부팅 단일 스레드 한정 사용).
        static size_t g_yamlErrLine = 0;
        static size_t g_yamlErrCol  = 0;

        [[noreturn]] static void OnYamlError(ryml::csubstr /*msg*/, ryml::ErrorDataBasic const& /*errdata*/, void* /*user*/)
        {
            g_yamlErrLine = 0; g_yamlErrCol = 0;   // basic 오류는 위치 정보 없음
            throw MonsterYamlError{};
        }
        [[noreturn]] static void OnYamlParseError(ryml::csubstr /*msg*/, ryml::ErrorDataParse const& errdata, void* /*user*/)
        {
            g_yamlErrLine = errdata.ymlloc.line; g_yamlErrCol = errdata.ymlloc.col;   // 문법 오류 위치
            throw MonsterYamlError{};
        }
        [[noreturn]] static void OnYamlVisitError(ryml::csubstr /*msg*/, ryml::ErrorDataVisit const& errdata, void* /*user*/)
        {
            g_yamlErrLine = errdata.cpploc.line; g_yamlErrCol = 0;
            throw MonsterYamlError{};
        }

        // 함수를 벗어나는 모든 경로(정상/실패 return, throw)에서 ryml 전역 콜백을 기본값으로 되돌리는 스코프 가드 -
        //   throw 형 에러 핸들러를 상주시키면 나중에 다른 코드가 ryml 을 쓸 때 catch 없는 파싱 오류가 즉시 종료로 이어진다.
        struct YamlCallbacksRestore
        {
            YamlCallbacksRestore() = default;
            ~YamlCallbacksRestore() { ryml::reset_callbacks(); }
            YamlCallbacksRestore(const YamlCallbacksRestore&) = delete;
            YamlCallbacksRestore& operator=(const YamlCallbacksRestore&) = delete;
        };

        // monsters.yml 을 열어 FILE* 반환 (실패 = nullptr). PortalTable/ItemTable/MapTable 과 같은 탐색.
        static FILE* OpenMonsterFile()
        {
            wchar_t exeDir[MAX_PATH];
            const DWORD len = GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
            if (len > 0 && len < MAX_PATH)
            {
                wchar_t* slash = wcsrchr(exeDir, L'\\');
                if (slash != nullptr) { *slash = L'\0'; }

                static const wchar_t* const relFromExe[] =
                {
                    L"\\Data\\monsters.yml",
                    L"\\..\\Data\\monsters.yml",
                    L"\\..\\..\\Data\\monsters.yml",
                    L"\\..\\..\\..\\Data\\monsters.yml",          // Build\x64\<Config>\ -> repo 루트 (정상 경로)
                    L"\\..\\..\\..\\..\\Data\\monsters.yml",
                    L"\\..\\..\\..\\..\\..\\Data\\monsters.yml",
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
            static const wchar_t* const cwdCandidates[] = { L"Data\\monsters.yml", L"..\\Data\\monsters.yml" };
            for (int i = 0; i < 2; ++i)
            {
                FILE* fp = nullptr;
                if (_wfopen_s(&fp, cwdCandidates[i], L"rt") == 0 && fp != nullptr) { return fp; }
            }
            return nullptr;
        }

        // 부팅 fatal 로그 축약 (이 파일 전용 - 실패 사유를 파일 로그에 남기고 -1 리턴하는 자리마다 반복 방지).
        template <typename... Args>
        static void LogBootFatal(const wchar_t* fmt, Args... args)
        {
            KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_FATAL, L"boot",
                fmt, args...);
        }

        // 필수 키 읽기 헬퍼 - 키가 없으면 false (호출부가 부팅 거부). ryml 트리는 읽기 전용 조회.
        static bool ReadInt(ryml::ConstNodeRef node, const char* key, int& out)
        {
            const ryml::csubstr k = ryml::to_csubstr(key);
            if (!node.has_child(k)) { return false; }
            node[k] >> out;
            return true;
        }
        static bool ReadFloat(ryml::ConstNodeRef node, const char* key, float& out)
        {
            const ryml::csubstr k = ryml::to_csubstr(key);
            if (!node.has_child(k)) { return false; }
            node[k] >> out;
            return true;
        }

        int LoadMonsterTable()
        {
            FILE* fp = OpenMonsterFile();
            if (fp == nullptr)
            {
                LogBootFatal(L"monsters.yml - 파일을 찾지 못함 (Data/monsters.yml 확인)");
                return -1;
            }

            // 파일 전체를 한 번에 읽는다 - ryml 은 버퍼를 제자리 파싱(parse_in_place)한다.
            //   64KB 고정 버퍼 (현재 ~4KB, 종류 추가 여유. 넘치면 상한을 늘리라는 뜻이므로 부팅 거부).
            static char yamlBuf[65536];
            const size_t readLen = fread(yamlBuf, 1, sizeof(yamlBuf) - 1, fp);
            fclose(fp);
            if (readLen == 0 || readLen >= sizeof(yamlBuf) - 1)
            {
                LogBootFatal(L"monsters.yml - 빈 파일이거나 버퍼 상한(64KB) 초과");
                return -1;
            }
            yamlBuf[readLen] = '\0';

            // 파일 선두 UTF-8 BOM(EF BB BF)은 건너뛴다 - 에디터가 붙였을 때 첫 키 파싱이 깨지는 오진 방지.
            char* parseStart = yamlBuf;
            size_t parseLen = readLen;
            if (parseLen >= 3 && static_cast<unsigned char>(parseStart[0]) == 0xEF &&
                static_cast<unsigned char>(parseStart[1]) == 0xBB && static_cast<unsigned char>(parseStart[2]) == 0xBF)
            {
                parseStart += 3; parseLen -= 3;
            }

            // 파싱 오류가 abort 로 죽지 않고 부팅 -1 로 내려오도록 에러 콜백 3계층 전부를 던지는 형태로 교체
            //   (basic 만 바꾸면 문법 오류가 기본 parse 핸들러의 abort 를 타고 크래시 - fix-diff 리뷰가 잡은 실버그).
            ryml::Callbacks yamlCallbacks;                    // 기본 콜백에서 에러 핸들러만 교체 (4인자 ctor 는 deprecated)
            yamlCallbacks.set_error_basic(&OnYamlError);
            yamlCallbacks.set_error_parse(&OnYamlParseError);
            yamlCallbacks.set_error_visit(&OnYamlVisitError);
            ryml::set_callbacks(yamlCallbacks);
            YamlCallbacksRestore callbacksRestore;            // 어느 경로로 나가든 전역 콜백 원복 (RAII)

            bool seen[static_cast<int>(EMonsterType::COUNT)] = {};   // 종류 전수/중복 검증 (하드코딩 시절 static_assert 대체)
            int dropRuleCount = 0;
            ClearDropRules();   // 드랍 저장소 초기화 (이 로더가 유일한 채움 주체)

            try
            {
                ryml::Tree tree = ryml::parse_in_place(ryml::substr(parseStart, parseLen));
                ryml::ConstNodeRef root = tree.crootref();
                const ryml::csubstr monstersKey = ryml::to_csubstr("monsters");
                if (!root.is_map() || !root.has_child(monstersKey))
                {
                    LogBootFatal(L"monsters.yml - 최상위 monsters 목록이 없음");
                    return -1;
                }

                for (ryml::ConstNodeRef m : root[monstersKey].children())
                {
                    // 필수 스탯 필드 - 하나라도 빠지면 부팅 거부 (무음 기본값으로 돌면 보스가 잡몹 스탯이 되는 사고).
                    int type = -1;
                    MonsterTemplate stats = {};
                    if (!ReadInt(m, "type", type) ||
                        !ReadInt(m, "hp", stats.hp) ||
                        !ReadInt(m, "atkPower", stats.atkPower) ||
                        !ReadInt(m, "moveSpeed", stats.moveSpeed) ||
                        !ReadInt(m, "aggroRange", stats.aggroRange) ||
                        !ReadInt(m, "attackRange", stats.attackRange) ||
                        !ReadFloat(m, "attackCooldown", stats.attackCooldown))
                    {
                        LogBootFatal(L"monsters.yml - 필수 스탯 필드 누락 (type/hp/atkPower/moveSpeed/aggroRange/attackRange/attackCooldown)");
                        return -1;
                    }

                    if (type < 0 || type >= static_cast<int>(EMonsterType::COUNT))
                    {
                        LogBootFatal(L"monsters.yml - type %d 범위 밖 (0..%d)", type, static_cast<int>(EMonsterType::COUNT) - 1);
                        return -1;
                    }
                    if (seen[type])
                    {
                        LogBootFatal(L"monsters.yml - type %d 중복 정의", type);
                        return -1;
                    }

                    // moveSpeed 는 SC_MONSTER_SPAWN 스냅샷으로 클라에 전달되어 종류별 속도가 가능하다.
                    //   하한 43 = 30Hz tick(0.0333s)의 int 절단 이동에서 "대각" 성분이 한 tick 에 1px 이상 전진하는 최소 속도
                    //   (대각 단위벡터 0.7071 -> 43*0.7071/30 = 1.01px). 8방 전환 전(직교만)엔 31 이면 됐으나,
                    //   대각은 성분이 0.7071 배라 42 이하면 대각 이동량이 0 으로 잘려 "벽에 막힘"과 구별 안 되는 반쪽 정지 몹이 된다
                    //   (부팅은 통과하는데 대각으로만 얼어붙는 무음 함정) - 데이터 실수로 보고 부팅에서 잡는다.
                    //   상한 = 공유 상한(클라 예측 신뢰 경계).
                    if (stats.moveSpeed < 43 || stats.moveSpeed > MAX_MOVE_SPEED)
                    {
                        LogBootFatal(L"monsters.yml - type %d moveSpeed %d 범위 밖 (43..%d - 42 이하는 30Hz 절단으로 대각 이동량 0)", type, stats.moveSpeed, MAX_MOVE_SPEED);
                        return -1;
                    }

                    g_monsterTemplates[type] = stats;
                    seen[type] = true;

                    // name 은 예약(없으면 빈 문자열). isBoss/cap 은 소비 필드 (없으면 기본값 false/0 = 잡몹/상한 없음).
                    const ryml::csubstr nameKey = ryml::to_csubstr("name");
                    g_monsterNames[type][0] = L'\0';
                    if (m.has_child(nameKey))
                    {
                        const ryml::csubstr nm = m[nameKey].val();
                        const int converted = ::MultiByteToWideChar(CP_UTF8, 0, nm.str, static_cast<int>(nm.len),
                                                                    g_monsterNames[type], kMonsterNameMax - 1);
                        if (converted <= 0)
                        {
                            // 버퍼 초과(15자 초과)나 깨진 바이트 - 무음 빈 이름이 되면 데이터 실수가 숨으므로 부팅에서 잡는다.
                            LogBootFatal(L"monsters.yml - type %d name 이 %d자 초과이거나 손상", type, kMonsterNameMax - 1);
                            return -1;
                        }
                        g_monsterNames[type][converted] = L'\0';
                    }
                    g_monsterIsBoss[type] = false;
                    const ryml::csubstr bossKey = ryml::to_csubstr("isBoss");
                    if (m.has_child(bossKey)) { m[bossKey] >> g_monsterIsBoss[type]; }
                    g_monsterCap[type] = 0;
                    if (!ReadInt(m, "cap", g_monsterCap[type])) { g_monsterCap[type] = 0; }

                    // 보스는 맵당 상한이 반드시 있어야 한다 - cap 0(상한 없음)인 보스는 스폰 그룹 검증이 통제 불능이라 부팅에서 잡는다.
                    if (g_monsterIsBoss[type] && g_monsterCap[type] < 1)
                    {
                        LogBootFatal(L"monsters.yml - type %d 는 isBoss 인데 cap %d (보스는 cap 1 이상 필수)", type, g_monsterCap[type]);
                        return -1;
                    }

                    // 드랍 규칙 (몬스터 블록 안 중첩) - 기존 drops.csv 로더의 행 검증을 이식하되 3곳은 의도적으로 강화:
                    //   유지 = 참조 무결성(FK)/스택 한도 위반 -> 부팅 거부, 확률/수량 정합 위반 -> 그 행만 무시.
                    //   강화 = (1) 종류 범위는 블록 구조상 위반 불가 (2) 저장소 가득 = 조용한 절단 대신 부팅 거부
                    //          (3) 드랍 0행 = 부팅 거부 (옛 로더도 0행이면 -1 이었으나 이제 몬스터 파일 전체 기준).
                    const ryml::csubstr dropsKey = ryml::to_csubstr("drops");
                    if (m.has_child(dropsKey))
                    {
                        for (ryml::ConstNodeRef dn : m[dropsKey].children())
                        {
                            DropRule rule = {};
                            rule.monsterType = static_cast<EMonsterType>(type);
                            if (!ReadInt(dn, "item", rule.templateId) ||
                                !ReadInt(dn, "chancePermil", rule.chancePermil) ||
                                !ReadInt(dn, "min", rule.minQty) ||
                                !ReadInt(dn, "max", rule.maxQty))
                            {
                                LogBootFatal(L"monsters.yml - type %d 드랍 행 필드 누락 (item/chancePermil/min/max)", type);
                                return -1;
                            }

                            const ItemDef* def = FindItemDef(rule.templateId);
                            if (def == nullptr)
                            {
                                LogBootFatal(L"monsters.yml - items.csv 에 없는 templateId %d", rule.templateId);
                                return -1;   // 드랍이 유령 아이템을 만들기 전에 부팅에서 잡는다
                            }
                            if (rule.maxQty > def->stackMax)
                            {
                                LogBootFatal(L"monsters.yml - templateId %d 의 maxQty %d 가 stackMax %d 초과", rule.templateId, rule.maxQty, def->stackMax);
                                return -1;   // 스택 한도 초과 드랍이 인벤 슬롯 오버플로를 만들기 전에 부팅에서 잡는다
                            }
                            if (rule.chancePermil <= 0 || rule.chancePermil > 1000) { continue; }   // 확률 정합 위반 행 무시
                            if (rule.minQty <= 0 || rule.maxQty < rule.minQty) { continue; }         // 수량 정합 위반 행 무시

                            if (!AddDropRule(rule))
                            {
                                LogBootFatal(L"monsters.yml - 드랍 규칙 저장소 가득 (상한 초과)");
                                return -1;
                            }
                            ++dropRuleCount;
                        }
                    }
                }
            }
            catch (const MonsterYamlError&)
            {
                LogBootFatal(L"monsters.yml - YAML 파싱 오류 (줄 %zu 칸 %zu 근처 - 들여쓰기/형식 확인)", g_yamlErrLine, g_yamlErrCol);
                return -1;
            }

            // 종류 전수 검증 - 누락 종류가 있으면 그 종류 스폰이 0 스탯으로 돌게 되므로 부팅 거부.
            for (int t = 0; t < static_cast<int>(EMonsterType::COUNT); ++t)
            {
                if (!seen[t])
                {
                    LogBootFatal(L"monsters.yml - 몬스터 종류 %d 누락 (전 종류 0..%d 정의 필수)", t, static_cast<int>(EMonsterType::COUNT) - 1);
                    return -1;
                }
            }
            if (dropRuleCount <= 0)
            {
                LogBootFatal(L"monsters.yml - 드랍 규칙 0행 (기존 drops.csv 소관 흡수 - 최소 1행 필요)");
                return -1;
            }

            return static_cast<int>(EMonsterType::COUNT);
        }
    }
}
