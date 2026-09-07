#include "pch_dummyclient.h"
#include "StressTest/StressTestManager.h"
#include "../GameCommon/MapData/MapTable.h"   // LoadMapTable (봇 목표 맵 범위 - 서버와 같은 Data/maps.csv 단일 진실원)
#include "../GameCommon/MapData/PortalTable.h"   // LoadPortalTable (봇 포탈 내비 - 서버와 같은 Data/portals.csv 단일 진실원)
#include "../GameCommon/MapData/WalkableTable.h"   // LoadWalkableTable (봇 이동이 서버와 같은 벽에서 멈추게 - 보정 폭풍 방지)
#include "../GameServer/Core/Config/ServerConfig.h"   // LoadServerConfig / ServerConfig (게임 서버 접속 포트 - 서버와 같은 설정 파일에서 읽는다)
#include <cwchar>    // fgetws / swscanf_s / wcslen / wcscpy_s
#include <clocale>   // setlocale

int main()
{
    // 한글 wprintf 가 콘솔에 보이게 시스템 로케일 설정. 없으면 한글 wide->multibyte 변환 실패로
    //   첫 한글에서 wprintf 가 멈춰 아무것도 출력되지 않는다(콘솔 한글 출력의 필수 1줄).
    ::setlocale(LC_ALL, "");

    // 게임 서버 접속 포트를 설정 파일에서 읽는다 - 서버가 여는 포트와 봇이 두드리는 포트가 같은 한 줄에서 나온다.
    //   봇은 MySQL 도 인터서버 링크도 쓰지 않으므로 1번째와 3번째 인자가 nullptr - 그 두 섹션은 읽지도 검증하지도 않는다.
    //   덕분에 DB 자격증명도 인터서버 비밀도 없는 머신에서 부하 테스트가 그대로 돈다.
    //   접속 IP 는 아래 stdin 입력이 정한다(봇은 로그인 서버 redirect 를 쓰지 않는다).
    ServerConfig serverConfig;
    if (!LoadServerConfig(nullptr, &serverConfig, nullptr))
    {
        ::wprintf(L"[DummyClient] Server_Config.ini 적재 실패 - 위 메시지의 키를 확인한다\n");
        return -1;
    }

    // 공유 맵 메타 표 적재 - 봇의 목표 맵 추첨 범위가 실제 맵 수(maps.csv 행 수)를 따라간다.
    //   서버와 같은 파일을 읽으므로 맵 수는 자동 동기 (하드코딩 맵 수 상수 폐기).
    //   포탈 내비게이션도 아래 portals.csv 공유 적재로 동기 - 맵/포탈 추가가 파일 편집만으로 봇에 반영된다.
    const int mapCount = LoadMapTable();
    if (mapCount < 0)
    {
        ::wprintf(L"맵 표 적재 실패 (Data/maps.csv 확인) - 종료\n");
        return 1;
    }
    ::wprintf(L"maps.csv 적재 OK (맵 %d개)\n", mapCount);

    // 통행 격자 적재 - 봇 이동이 서버와 같은 벽에서 멈춰야 서버 보정(SC_MOVE_BROADCAST 되당김)이 안 생긴다.
    //   봇은 부하 측정 도구라 관대 폴백 없이 fail-fast (지형 모르는 봇의 회귀 결과는 해석 불가).
    const int walkableCount = LoadWalkableTable();
    if (walkableCount < 0)
    {
        ::wprintf(L"통행 격자 적재 실패 (maps.csv walkableFile/Data 통행 CSV 확인) - 종료\n");
        return 1;
    }
    ::wprintf(L"통행 격자 적재 OK (맵 %d개)\n", walkableCount);

    // 공유 포탈 표 적재 - 봇 포탈 내비가 서버와 같은 파일(portals.csv)을 읽는다 (하드코딩 미러 폐기).
    const int portalCount = LoadPortalTable();
    if (portalCount < 0)
    {
        ::wprintf(L"포탈 표 적재 실패 (Data/portals.csv 확인) - 종료\n");
        return 1;
    }
    ::wprintf(L"portals.csv 적재 OK (포탈 %d개)\n", portalCount);

    // 서버 IP 입력 (Enter = 127.0.0.1 로컬). 다른 PC 서버면 그 IP 입력.
    wchar_t serverIp[64] = { 0 };
    ::wprintf(L"서버 IP (Enter = 127.0.0.1): ");
    if (::fgetws(serverIp, 64, stdin) == nullptr)
    {
        ::wprintf(L"입력 오류\n");
        return 1;
    }
    size_t ipLen = ::wcslen(serverIp);
    while (ipLen > 0 && (serverIp[ipLen - 1] == L'\n' || serverIp[ipLen - 1] == L'\r'))
    {
        serverIp[--ipLen] = L'\0';   // 개행 제거
    }
    if (serverIp[0] == L'\0')
    {
        ::wcscpy_s(serverIp, 64, L"127.0.0.1");   // 빈 입력 = 로컬
    }

    // 접속 대상을 여기서 못박는다 - 봇 전량 연결 실패가 "서버 문제"인지 "봇이 다른 포트를 두드린 것"인지
    //   이 한 줄로 갈린다. IP 는 방금 입력, 포트는 설정 파일에서 온 값이다.
    ::wprintf(L"[DummyClient] conf/Server_Config.ini 적재 OK - 게임 서버 %ls:%u 로 접속한다\n",
        serverIp, static_cast<unsigned int>(serverConfig.publicPort));

    // 초기 봇 수 입력
    ::wprintf(L"초기 봇 수: ");
    wchar_t line[64] = { 0 };
    if (::fgetws(line, 64, stdin) == nullptr)
    {
        ::wprintf(L"입력 오류\n");
        return 1;
    }
    int initialCount = 0;
    if (::swscanf_s(line, L"%d", &initialCount) != 1 || initialCount <= 0)
    {
        ::wprintf(L"잘못된 봇 수\n");
        return 1;
    }

    // 최대 봇 수 입력 (런타임 +추가의 상한 = 배열 사전 할당 크기). Enter = 초기와 동일.
    ::wprintf(L"최대 봇 수 (런타임 추가 상한·Enter=초기와 동일): ");
    wchar_t maxLine[64] = { 0 };
    int maxCount = initialCount;
    if (::fgetws(maxLine, 64, stdin) != nullptr && maxLine[0] != L'\n' && maxLine[0] != L'\r')
    {
        int parsed = 0;
        if (::swscanf_s(maxLine, L"%d", &parsed) == 1 && parsed >= initialCount)
        {
            maxCount = parsed;
        }
    }

    // 서버 세션 풀 상한을 넘으면 초과분은 서버가 거부하므로 미리 제한한다.
    if (maxCount > static_cast<int>(DEFAULT_SESSION_POOL_CAPACITY))
    {
        ::wprintf(L"경고: 서버 세션 풀=%d 초과 -> 최대 %d 로 제한\n",
            static_cast<int>(DEFAULT_SESSION_POOL_CAPACITY),
            static_cast<int>(DEFAULT_SESSION_POOL_CAPACITY));
        maxCount = static_cast<int>(DEFAULT_SESSION_POOL_CAPACITY);
    }
    if (initialCount > maxCount) { initialCount = maxCount; }

    // churn 모드 입력 (y = 봇이 수명 후 close->재연결 반복, 슬롯 재사용 stress). 재빌드 없이 입력으로 토글.
    bool churnMode = false;
    ::wprintf(L"churn 모드? (재연결 반복·y/n, Enter=n): ");
    wchar_t churnLine[16] = { 0 };
    if (::fgetws(churnLine, 16, stdin) != nullptr && (churnLine[0] == L'y' || churnLine[0] == L'Y'))
    {
        churnMode = true;
    }

    // 중복 로그인 모드 입력 (y = 봇이 좁은 계정 범위를 공유 -> 중복 로그인 -> kick 유발, kick 부하).
    //   봇 i 는 bot{start + (i % (end-start+1))} 계정으로 로그인. 봇 수 > 범위 면 초과 봇이 살아있는 세션을 kick.
    bool dupLoginMode = false;
    int acctStart = 0;
    int acctEnd = 0;
    ::wprintf(L"중복 로그인 모드? (계정 공유로 kick 유발·y/n, Enter=n): ");
    wchar_t dupLine[16] = { 0 };
    if (::fgetws(dupLine, 16, stdin) != nullptr && (dupLine[0] == L'y' || dupLine[0] == L'Y'))
    {
        dupLoginMode = true;
        ::wprintf(L"  계정 인덱스 시작: ");
        wchar_t sLine[64] = { 0 };
        if (::fgetws(sLine, 64, stdin) == nullptr || ::swscanf_s(sLine, L"%d", &acctStart) != 1 || acctStart < 0)
        {
            ::wprintf(L"잘못된 시작 인덱스\n");
            return 1;
        }
        ::wprintf(L"  계정 인덱스 끝(포함): ");
        wchar_t eLine[64] = { 0 };
        if (::fgetws(eLine, 64, stdin) == nullptr || ::swscanf_s(eLine, L"%d", &acctEnd) != 1 || acctEnd < acctStart)
        {
            ::wprintf(L"잘못된 끝 인덱스(시작 이상이어야 함)\n");
            return 1;
        }
        ::wprintf(L"  -> 계정 bot%05d ~ bot%05d (%d개) 를 봇 %d개가 공유 (초과분이 kick 유발)\n",
            acctStart, acctEnd, acctEnd - acctStart + 1, maxCount);
    }

    // 검증 하니스 모드 (B-1, 비우면 없음). 전부 off 면 프로덕션 부하와 바이트 동일 - 재빌드 없이 입력 토글.
    ::wprintf(L"하니스 모드 (검증용·비우면 없음. 예: h2 h1):\n");
    ::wprintf(L"  h1=STOP표류  h2=핫스팟과부하  h3=raw주입(PV)  h7=bogus귓속말  h9=채널스킵  h10=idle뮤트  h12=인증실패  b3=핸드오프TOCTOU  hw=벽게이트프로브: ");
    wchar_t harnessLine[128] = { 0 };
    if (::fgetws(harnessLine, 128, stdin) != nullptr)
    {
        // 공백 구분 토큰을 정확 비교한다 - wcsstr 은 "h1" 이 "h10"/"h12" 의 부분열이라 오검출하므로 토큰 단위로 본다.
        wchar_t* ctx = nullptr;
        for (wchar_t* tok = ::wcstok_s(harnessLine, L" \t\r\n", &ctx); tok != nullptr; tok = ::wcstok_s(nullptr, L" \t\r\n", &ctx))
        {
            if      (::wcscmp(tok, L"h1")  == 0) { DummySession::s_harness.stopMove     = true; }
            else if (::wcscmp(tok, L"h2")  == 0) { DummySession::s_harness.hotspot      = true; }
            else if (::wcscmp(tok, L"h3")  == 0) { DummySession::s_harness.rawInject    = true; }
            else if (::wcscmp(tok, L"h7")  == 0) { DummySession::s_harness.bogusWhisper = true; }
            else if (::wcscmp(tok, L"h9")  == 0) { DummySession::s_harness.skipChannel  = true; }
            else if (::wcscmp(tok, L"h10") == 0) { DummySession::s_harness.muteIdle     = true; }
            else if (::wcscmp(tok, L"h12") == 0) { DummySession::s_harness.badCred      = true; }
            else if (::wcscmp(tok, L"b3")  == 0) { DummySession::s_harness.b3repro      = true; }
            else if (::wcscmp(tok, L"hw")  == 0) { DummySession::s_harness.wallProbe    = true; }
        }
    }

    if (DummySession::s_harness.stopMove && !DummySession::s_harness.hotspot)
        ::wprintf(L"[!] H1(STOP)은 과부하(H2)가 있어야 STOP 이 shed 되어 재현됩니다 - h1 h2 로 함께 켜세요.\n");
    ::wprintf(L"-> 서버 %s : 초기 %d / 최대 %d 봇으로 부하 시작 (churn=%s · 중복로그인=%s)\n",
        serverIp, initialCount, maxCount, churnMode ? L"ON" : L"OFF", dupLoginMode ? L"ON" : L"OFF");
    ::wprintf(L"   하니스: h1(STOP)=%s h2(hotspot)=%s h3(raw)=%s h7(whisper)=%s h9(chskip)=%s h10(idle)=%s h12(badcred)=%s b3(toctou)=%s hw(wallprobe)=%s\n",
        DummySession::s_harness.stopMove     ? L"ON" : L"off",
        DummySession::s_harness.hotspot      ? L"ON" : L"off",
        DummySession::s_harness.rawInject    ? L"ON" : L"off",
        DummySession::s_harness.bogusWhisper ? L"ON" : L"off",
        DummySession::s_harness.skipChannel  ? L"ON" : L"off",
        DummySession::s_harness.muteIdle     ? L"ON" : L"off",
        DummySession::s_harness.badCred      ? L"ON" : L"off",
        DummySession::s_harness.b3repro      ? L"ON" : L"off",
        DummySession::s_harness.wallProbe    ? L"ON" : L"off");

    StressTestManager manager;
    if (!manager.Init(maxCount, serverIp, serverConfig.publicPort))
    {
        ::wprintf(L"Init 실패\n");
        return 2;
    }

    manager.SetTestMode(churnMode, dupLoginMode, acctStart, acctEnd);   // 입력 구동 모드 적용 (Run 전)
    manager.Run(initialCount);
    manager.Shutdown();
    return 0;
}
