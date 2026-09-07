#include "pch_serverapp.h"
#include "Network/MainServer.h"                          // KYS::SERVERAPP::MainServer (: public IOCPServer)
#include "Game/Channel/Channel.h"               // KYS::SERVERAPP::Channel (: public IChannel)
#include "Game/Channel/ChannelManager.h"        // KYS::SERVERAPP::ChannelManager (: public INetEventHandler)
#include "Game/Channel/IChannel.h"              // IChannel* 다형 배열 타입
#include "Game/Channel/ChannelWatchdog.h"       // KYS::SERVERAPP::ChannelWatchdog (채널 행 감시 - 행을 크래시로 변환해 덤프 연계)
#include "Monitor/ServerMonitor.h"
#include "../GameServer/Core/Config/ServerConfig.h"  // DbConfig / ServerConfig / InterServerConfig / LoadServerConfig (conf/Server_Config.ini 적재)
#include "Network/LoginLinkThread.h"            // KYS::SERVERAPP::LoginLinkThread (인터서버 링크 connector)
#include "Monitor/MetricExporter.h"                  // 지표 시계열 export (10s JSONL - 'q' 모니터와 독립)
#include "Game/Path/PathfinderThread.h"          // KYS::SERVERAPP::PathfinderThread (전용 길찾기 스레드 - 우회 경로 A*/JPS)
#include "Game/Monster/MonsterTable.h"          // KYS::SERVERAPP::LoadMonsterTable (몬스터 스탯 + 드랍 - 서버 전용 monsters.yml)
#include "Game/Monster/SpawnTable.h"            // KYS::SERVERAPP::LoadSpawnTable (몬스터 스폰 표 - 서버 전용 spawns.csv)
#include "Game/Item/DropTable.h"                // KYS::SERVERAPP::DropTableCount (부팅 로그 - 적재 드랍 규칙 수 표기)
#include "../GameServer/Types/Defines.h"        // DEFAULT_SESSION_POOL_CAPACITY / USHORT
#include "../GameServer/Core/Lifecycle/CrashDump.h"        // KYS::GAMESERVER::CrashDump (크래시 미니덤프 - 두 exe 공유 인프라)
#include "../GameServer/Core/Lifecycle/GracefulShutdown.h" // KYS::GAMESERVER::GracefulShutdown (graceful 종료 - 두 exe 공유 인프라)
#include "../GameServer/Core/Log/Logger.h"       // KYS::GAMESERVER::LOG::Logger (서버/유저 이벤트 파일 로그 - 두 exe 공유 인프라)
#include "../GameCommon/GameDefines.h"          // LOGIN_INTERSERVER_PORT (로그인 서버 인터서버 포트)
#include "../GameCommon/MapData/PortalTable.h"          // LoadPortalTable (공유 포탈 표 - 클라와 같은 Data/portals.csv)
#include "../GameCommon/ItemData/ItemTable.h"            // LoadItemTable (공유 아이템 사전 - 클라와 같은 Data/items.csv)
#include "../GameCommon/MapData/MapTable.h"             // LoadMapTable (공유 맵 메타 표 - Data/maps.csv, 맵 수/유지 인구/부활 좌표)
#include "../GameCommon/MapData/WalkableTable.h"        // LoadWalkableTable/IsWalkable (통행 격자 + 포탈 좌표 교차검증)
#ifdef _DEBUG
#include "../GameServer/Core/Thread/IJob.h"     // HangInjectJob 베이스 (워치독 검증용 행 주입 - Debug 전용)
#include "DataBase/MySqlDBProvider.h"           // H4 인벤 저장 강제 실패 무장 (재현 - Debug 전용)
#endif
#include <conio.h>
#include <clocale>
#include <process.h>                             // _beginthreadex
#include <Windows.h>                             // HANDLE / WaitForMultipleObjects (timeBeginPeriod 계열은 pch <timeapi.h> 제공)




#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004   // 구형 SDK 폴백 (Windows 10 1511+ 콘솔 VT 플래그)
#endif

// 콘솔 화면 클리어 + 커서 홈 (라이브 갱신 시 잔상 제거).
//   모던 터미널(Windows Terminal/VS Code)은 Win32 FillConsole이 스크롤백을 못 비워 과거 덤프가 위로 쌓인다.
//   VT(가상터미널) 모드를 켜고 ANSI 이스케이프로 화면 + 스크롤백을 함께 비우면 cmd/Windows Terminal/VS Code 모두 제자리 갱신된다.
static void ClearConsole()
{
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);

    DWORD mode = 0;
    if (::GetConsoleMode(hOut, &mode) &&
        ::SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
    {
        // \x1b[3J = 스크롤백 삭제(쌓임 제거 핵심), \x1b[2J = 화면 삭제, \x1b[H = 커서 홈
        ::wprintf(L"\x1b[3J\x1b[2J\x1b[H");
        return;
    }

    // VT 미지원(구형 콘솔) 폴백 - 화면 버퍼만 비움(스크롤백은 못 비움).
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!::GetConsoleScreenBufferInfo(hOut, &csbi)) { return; }
    const DWORD cells = static_cast<DWORD>(csbi.dwSize.X) * static_cast<DWORD>(csbi.dwSize.Y);
    const COORD home = { 0, 0 };
    DWORD written = 0;
    ::FillConsoleOutputCharacterW(hOut, L' ', cells, home, &written);
    ::FillConsoleOutputAttribute(hOut, csbi.wAttributes, cells, home, &written);
    ::SetConsoleCursorPosition(hOut, home);
}

static const DWORD MONITOR_REFRESH_MS = 1000;

using KYS::GAMESERVER::LOG::LogLevel;

// main 전용 축약 - server 채널 파일 로그 (INFO 이상은 콘솔에도 병행 출력이라 기존 wprintf 가시성 유지)
template<typename... Args>
static void ServerLog(LogLevel level, const wchar_t* tag, const wchar_t* format, Args&&... args)
{
    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
        KYS::GAMESERVER::LOG::LogChannel::SERVER, level, tag, format, std::forward<Args>(args)...);
}

// 크래시 덤프(.dmp) 확보 후 로그 큐 잔량을 best-effort 로 밀어내는 다리 (CrashDump 는 Logger 를 모른다 - 앱이 연결).
static void FlushLogsAfterDump()
{
    KYS::GAMESERVER::LOG::Logger::GetInstance().EmergencyFlush();
}

// 콘솔 QuickEdit 끄기 - 텍스트 드래그 선택이 콘솔 write 를 블록해 로그 찍던 스레드가 결박되는 것을 차단.
//   대상은 입력 핸들이고 ENABLE_EXTENDED_FLAGS 를 함께 세워야 발효된다. 복사/검색은 이제 Logs\ 파일이 담당.
static void DisableConsoleQuickEdit()
{
    HANDLE hIn = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (hIn != INVALID_HANDLE_VALUE && ::GetConsoleMode(hIn, &mode))
    {
        ::SetConsoleMode(hIn, (mode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
    }
}

static const int CHANNEL_COUNT = 6;   // 게임 채널 6개 (로그인 분배 0번 채널 폐기, 전부 게임 채널) - 6 게임 스레드 < 12 코어 여유

static const DWORD GAME_REAPER_INTERVAL_MS = 1000;     // 무음 소켓 회수 sweep 주기
static const UINT64 GAME_NORECV_DEADLINE_MS = 35000;   // 무수신 회수 데드라인 - 게임측 idle reaper(CollectIdleSessions 30s)보다 위로 둬 인증세션은 게임측이 소유, 라이브러리 reaper는 pre-auth 무음 소켓만(+인증-dead 백스톱)

static unsigned __stdcall ChannelThreadEntry(void* arg)
{
    KYS::SERVERAPP::Channel* channel = static_cast<KYS::SERVERAPP::Channel*>(arg);
    channel->TickLoop();    // 30Hz 루프 (한 틱 처리 시간 재고 남는 만큼 대기). m_running=false면 반환 -> 스레드 종료
    return 0;
}

static unsigned __stdcall SharedServicesThreadEntry(void* arg)
{
    KYS::SERVERAPP::SharedServicesThread* sst = static_cast<KYS::SERVERAPP::SharedServicesThread*>(arg);
    sst->Run();    // 큐 비우기 루프. Stop()으로 m_running=false면 마지막 비우기 후 반환
    return 0;
}

// 무음 소켓 회수 스레드 - 주기적으로 라이브러리 SessionPool을 sweep해 무수신 타임아웃 넘긴 소켓(pre-auth 좀비 + 인증-dead 백스톱)을 회수한다.
//   워치독(행 감지->abort)과 분리한 전용 스레드 - reaper의 풀 락/Disconnect 지연이 행 감지를 방해하지 않게. LoginServer TokenSweepThread와 대칭.
static unsigned __stdcall GameReaperThreadEntry(void* arg)
{
    HANDLE stopEvent = static_cast<HANDLE>(arg);
    KYS::SERVERAPP::MainServer& gameServer = KYS::SERVERAPP::MainServer::GetInstance();
    for (;;)
    {
        if (::WaitForSingleObject(stopEvent, GAME_REAPER_INTERVAL_MS) == WAIT_OBJECT_0)
        {
            break;   // 종료 신호
        }
        const int reaped = gameServer.ReapIdleSessions(::GetTickCount64(), GAME_NORECV_DEADLINE_MS);   // 무수신 소켓 일괄 회수
        if (reaped > 0)
        {
            KYS::SERVERAPP::ServerMonitor::GetInstance().OnReaped(reaped);   // flood 방어 관측 누적 (표출/export)
            ServerLog(LogLevel::LL_INFO, L"reaper", L"무수신 소켓 %d개 회수", reaped);
        }
    }
    return 0;
}

#ifdef _DEBUG
// 워치독 검증용 행(hang) 주입 job (Debug 전용) - 채널 스레드를 90초 재워 tick을 멈춘다.
//   90초 = 행 판정 임계(60초) 초과. 워치독이 없거나 고장이어도 90초 뒤 스스로 풀리는 안전한 주입.
struct HangInjectJob : public KYS::GAMESERVER::THREAD::IJob
{
    void Execute() override
    {
        ::wprintf(L"[test] 행 주입 발동 - 채널 스레드 90초 정지 (워치독 발동 대기)\n");
        ::Sleep(90000);
    }
};
#endif

int wmain(int argc, wchar_t* argv[])    // 표준 진입점(전역 - 네임스페이스 밖). wchar_t(Unicode)
{
    
    (void)argc; (void)argv;             // 인자 미사용 (CHANNEL_COUNT는 상수, 게임 포트는 conf/Server_Config.ini)
    ::setlocale(LC_ALL, "");

    // (0) 크래시 핸들러 등록 (무엇보다 먼저 - 초기화 중 크래시도 커버). 프로세스 전역이라 이후 만든 모든 스레드 커버.
    KYS::GAMESERVER::CrashDump::Install(L"ServerApp");

    // (0b) 파일 로거 기동 - 이후 부팅 로그부터 Logs\ 에 영속 (콘솔 병행 출력이라 화면 가시성 동일).
    //   덤프(.dmp) 확보 후 로그 잔량을 밀어내는 콜백 + QuickEdit 차단(드래그 선택의 콘솔 write 블록 차단)도 여기서.
    KYS::GAMESERVER::LOG::Logger::GetInstance().Initialize(L"ServerApp");
    KYS::GAMESERVER::CrashDump::SetPostDumpCallback(&FlushLogsAfterDump);
    DisableConsoleQuickEdit();

    // (1) 타이머 분해능 1ms 상향 (Sleep 정밀도 - 프로세스 전역, 1회)
    timeBeginPeriod(1);

    // (2) 싱글턴 매니저 5개 획득 (Meyers static - Start 보류, 주소만 확보).
    //     GetInstance 첫 호출 순서(gameServer->...->monitor)가 소멸 역순을 고정. 참조 별칭이라
    //     기존 &channelManager/.Init/teardown 그대로 동작 (스택 선언 0 -> split-brain 0, 단일 인스턴스).
    KYS::SERVERAPP::MainServer&            gameServer     = KYS::SERVERAPP::MainServer::GetInstance();
    KYS::SERVERAPP::ChannelManager&        channelManager = KYS::SERVERAPP::ChannelManager::GetInstance();
    KYS::SERVERAPP::DBThread&              dbThread       = KYS::SERVERAPP::DBThread::GetInstance();
    KYS::SERVERAPP::SharedServicesThread&  sst            = KYS::SERVERAPP::SharedServicesThread::GetInstance();
    KYS::SERVERAPP::ServerMonitor&         monitor        = KYS::SERVERAPP::ServerMonitor::GetInstance();

    // (3) 구상 Channel N개 생성 (전부 게임 채널, 0..N-1). 같은 ctor, 같은 타입.
    //     IChannel* 배열로 모아 ChannelManager에 넘긴다 (Init 시그니처).
    KYS::SERVERAPP::Channel* gameChannels[CHANNEL_COUNT];
    KYS::SERVERAPP::IChannel* channels[CHANNEL_COUNT];
    for (int i = 0; i < CHANNEL_COUNT; ++i)
    {
        // Channel 생성 - 채널 id 주입 (ChannelManager는 GetInstance 직접).
        gameChannels[i] = new KYS::SERVERAPP::Channel(i);
        channels[i] = gameChannels[i];    // Channel* -> IChannel* 로 변환 (다형 포인터)
    }

    // (4) ChannelManager.Init - 채널 배열/서버 핸들 보유(게임세션 풀은 앞선 GetInstance 생성자에서 확보). Start 전 필수
    //     (EnqueuePacket이 m_channels[chId] 참조하므로 첫 패킷 전에 배열이 있어야 함).
    if (!channelManager.Init(static_cast<int>(DEFAULT_SESSION_POOL_CAPACITY), channels, CHANNEL_COUNT,&gameServer))  // size_t -> int 축소 변환 명시 (/W4 C4267 회피)
    {
        // Init 실패: 스레드 미기동 상태 -> Channel만 정리.
        for (int i = 0; i < CHANNEL_COUNT; ++i)
        {
            delete gameChannels[i];
        }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }

    // MySQL C 클라이언트 라이브러리 전역 초기화 - 어떤 스레드도 MySQL 을 쓰기 전에 1회(스레드 생성 앞).
    if (mysql_library_init(0, NULL, NULL) != 0)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"mysql_library_init 실패");
        for (int i = 0; i < CHANNEL_COUNT; ++i) { delete gameChannels[i]; }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }

    ::wprintf(L"========== ServerApp 부팅 ==========\n");

    // 서버 설정 적재 (conf/Server_Config.ini - DB 접속 + 게임 서버 공개 주소 + 인터서버 비밀을 한 파일에서).
    //   실패(파일 없음/필수 키 누락/값 형식 오류) = fail-fast. 어느 섹션의 어느 키가 문제인지는 로더가 콘솔에 찍는다.
    DbConfig          dbConfig;
    ServerConfig      serverConfig;
    InterServerConfig interConfig;
    if (!LoadServerConfig(&dbConfig, &serverConfig, &interConfig))
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"Server_Config.ini 적재 실패 - 위 콘솔 메시지의 키를 확인한다");
        mysql_library_end();
        for (int i = 0; i < CHANNEL_COUNT; ++i) { delete gameChannels[i]; }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }

    ServerLog(LogLevel::LL_INFO, L"boot", L"Server_Config.ini 적재 OK (host=%hs port=%d db=%hs user=%hs / 공개 주소 %u.%u.%u.%u:%u)",
        dbConfig.host, dbConfig.port, dbConfig.dbname, dbConfig.user,
        (serverConfig.publicIp >> 24) & 0xFF, (serverConfig.publicIp >> 16) & 0xFF,
        (serverConfig.publicIp >> 8) & 0xFF, serverConfig.publicIp & 0xFF,
        serverConfig.publicPort);

    // 공유 포탈 표 적재 (Data/portals.csv - 서버+클라 단일 진실원). 실패(파일 없음/0행) = fail-fast.
    const int portalCount = LoadPortalTable();
    if (portalCount < 0)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"포탈 표 적재 실패 (Data/portals.csv 확인)");
        mysql_library_end();
        for (int i = 0; i < CHANNEL_COUNT; ++i) { delete gameChannels[i]; }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }
    ServerLog(LogLevel::LL_INFO, L"boot", L"portals.csv 적재 OK (포탈 %d개)", portalCount);

    // 공유 아이템 사전 적재 (Data/items.csv - 서버+클라 단일 진실원). 실패 = fail-fast (아이템 검증 불가).
    const int itemCount = LoadItemTable();
    if (itemCount < 0)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"아이템 사전 적재 실패 (Data/items.csv 확인)");
        mysql_library_end();
        for (int i = 0; i < CHANNEL_COUNT; ++i) { delete gameChannels[i]; }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }
    ServerLog(LogLevel::LL_INFO, L"boot", L"items.csv 적재 OK (아이템 %d종)", itemCount);

    // 공유 맵 메타 표 적재 (Data/maps.csv - 맵 수/유지 인구/부활 좌표. DummyClient와 공유). 실패 = fail-fast.
    //   이후 스폰 표(mapId 검증)와 초기 스폰(monsterCap)이 이 표를 참조하므로 콘텐츠 표 중 가장 먼저.
    const int mapCount = LoadMapTable();
    if (mapCount < 0)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"맵 표 적재 실패 (Data/maps.csv 확인 - 행수/mapId 연속/크기 범위·250 배수/부활 좌표)");
        mysql_library_end();
        for (int i = 0; i < CHANNEL_COUNT; ++i) { delete gameChannels[i]; }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }
    ServerLog(LogLevel::LL_INFO, L"boot", L"maps.csv 적재 OK (맵 %d개)", mapCount);

    // 통행 격자 적재 (maps.csv walkableFile 열 - "-"=벽 없는 맵/파일명=강제 적재. 부활 좌표 통행 검증 포함).
    //   반드시 maps.csv 뒤(파일명/크기 참조) + spawns.csv 앞(스폰 구역 통행 교차검증이 이 격자를 씀). 실패 = fail-fast.
    const int walkableCount = LoadWalkableTable();
    if (walkableCount < 0)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"통행 격자 적재 실패 (maps.csv walkableFile/Data 통행 CSV 확인 - 상세 사유는 콘솔 [walkable])");
        mysql_library_end();
        for (int i = 0; i < CHANNEL_COUNT; ++i) { delete gameChannels[i]; }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }
    ServerLog(LogLevel::LL_INFO, L"boot", L"통행 격자 적재 OK (맵 %d개)", walkableCount);

    // 포탈 <-> 맵/통행 교차 검증 - 포탈 표는 맵 표보다 먼저 적재되므로(순서 유지) 여기서 사후 대조한다.
    //   맵 밖/벽 위를 가리키는 포탈은 밟아도 무음 no-op(죽은 포탈)이거나 도착 즉시 끼임으로 남으므로 부팅에서 잡는다.
    //   trigger 도 검증한다 - 맵을 줄이면 발동 지점이 맵 밖에 남아 아무도 못 밟는 죽은 포탈이 되기 때문 (조용한 무시 금지).
    for (int i = 0; i < PortalTableCount(); ++i)
    {
        const PortalDef& portal = PortalTableAt(i);
        bool portalBad = false;
        if (portal.srcMapId < 0 || portal.srcMapId >= mapCount ||
            portal.dstMapId < 0 || portal.dstMapId >= mapCount)
        {
            ServerLog(LogLevel::LL_FATAL, L"boot", L"portals.csv %d행 - 맵 표 범위(0..%d) 밖 (src=%d dst=%d)",
                i, mapCount - 1, portal.srcMapId, portal.dstMapId);
            portalBad = true;
        }
        else if (portal.trigger.x < 0 || portal.trigger.x > MapTableAt(portal.srcMapId).width ||
                 portal.trigger.y < 0 || portal.trigger.y > MapTableAt(portal.srcMapId).height)
        {
            ServerLog(LogLevel::LL_FATAL, L"boot", L"portals.csv %d행 - 발동 지점 (%d,%d) 이 출발 맵 %d(%dx%d) 밖",
                i, portal.trigger.x, portal.trigger.y, portal.srcMapId,
                MapTableAt(portal.srcMapId).width, MapTableAt(portal.srcMapId).height);
            portalBad = true;
        }
        else if (!IsWalkable(portal.srcMapId, portal.trigger.x, portal.trigger.y))
        {
            ServerLog(LogLevel::LL_FATAL, L"boot", L"portals.csv %d행 - 발동 지점 (%d,%d) 이 맵 %d 의 벽 위 (도달 불가 포탈)",
                i, portal.trigger.x, portal.trigger.y, portal.srcMapId);
            portalBad = true;
        }
        else if (portal.dst.x < 0 || portal.dst.x > MapTableAt(portal.dstMapId).width ||
                 portal.dst.y < 0 || portal.dst.y > MapTableAt(portal.dstMapId).height)
        {
            ServerLog(LogLevel::LL_FATAL, L"boot", L"portals.csv %d행 - 도착 좌표 (%d,%d) 이 도착 맵 %d(%dx%d) 밖",
                i, portal.dst.x, portal.dst.y, portal.dstMapId,
                MapTableAt(portal.dstMapId).width, MapTableAt(portal.dstMapId).height);
            portalBad = true;
        }
        else if (!IsWalkable(portal.dstMapId, portal.dst.x, portal.dst.y))
        {
            ServerLog(LogLevel::LL_FATAL, L"boot", L"portals.csv %d행 - 도착 좌표 (%d,%d) 이 맵 %d 의 벽 위 (도착 즉시 끼임)",
                i, portal.dst.x, portal.dst.y, portal.dstMapId);
            portalBad = true;
        }
        if (portalBad)
        {
            mysql_library_end();
            for (int j = 0; j < CHANNEL_COUNT; ++j) { delete gameChannels[j]; }
            KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
            timeEndPeriod(1);
            return -1;
        }
    }

    // 몬스터 사전 적재 (Data/monsters.yml - 스탯 + 드랍 규칙. 서버 전용, 클라 비배포). 반드시 items.csv 뒤
    //   (드랍 행의 templateId 를 아이템 사전과 대조하는 참조 무결성 검증 때문 - 기존 drops.csv 소관 흡수).
    const int monsterTypeCount = KYS::SERVERAPP::LoadMonsterTable();
    if (monsterTypeCount < 0)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"몬스터 사전 적재 실패 (Data/monsters.yml 확인 - 상세 사유는 직전 FATAL 로그)");
        mysql_library_end();
        for (int i = 0; i < CHANNEL_COUNT; ++i) { delete gameChannels[i]; }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }
    ServerLog(LogLevel::LL_INFO, L"boot", L"monsters.yml 적재 OK (몬스터 %d종, 드랍 규칙 %d행)", monsterTypeCount, KYS::SERVERAPP::DropTableCount());

    // 몬스터 스폰 표 적재 (Data/spawns.csv - 서버 전용). 반드시 maps.csv 뒤 (mapId 범위 검증).
    const int spawnGroupCount = KYS::SERVERAPP::LoadSpawnTable();
    if (spawnGroupCount < 0)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"스폰 표 적재 실패 (Data/spawns.csv 확인 - 상세 사유는 직전 FATAL 로그)");
        mysql_library_end();
        for (int i = 0; i < CHANNEL_COUNT; ++i) { delete gameChannels[i]; }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }
    ServerLog(LogLevel::LL_INFO, L"boot", L"spawns.csv 적재 OK (스폰 그룹 %d개)", spawnGroupCount);

    // 통행 연결성 교차검증 - 앞의 포탈/스폰 검증은 "그 점이 벽 위인가"만 봤다. 여기선 "부활 지점에서
    //   그 점까지 통행 칸으로 이어지는가"(도달성)를 본다. 벽으로 완전히 둘러싸인 통행 섬(격리 영역)은
    //   개별 점 검사는 통과하지만 아무도 못 가는 죽은 포탈/영원히 못 닿는 스폰이 된다 - 부팅에서 잡는다.
    //   맵별 respawn 기점 4방 flood-fill (A*/JPS 8방 no-corner-cut 도달성과 동일). 부팅 1회라 비용 무시 가능.
    {
        bool connectivityBad = false;
        for (int i = 0; i < PortalTableCount() && !connectivityBad; ++i)
        {
            const PortalDef& portal = PortalTableAt(i);
            const Position srcRespawn = MapTableAt(portal.srcMapId).respawn;
            const Position dstRespawn = MapTableAt(portal.dstMapId).respawn;
            if (!IsReachable(portal.srcMapId, srcRespawn, portal.trigger))
            {
                ServerLog(LogLevel::LL_FATAL, L"boot", L"portals.csv %d행 - 발동 지점 (%d,%d) 이 맵 %d 부활 지점에서 도달 불가 (벽으로 격리된 통행 섬)",
                    i, portal.trigger.x, portal.trigger.y, portal.srcMapId);
                connectivityBad = true;
            }
            else if (!IsReachable(portal.dstMapId, dstRespawn, portal.dst))
            {
                ServerLog(LogLevel::LL_FATAL, L"boot", L"portals.csv %d행 - 도착 좌표 (%d,%d) 이 맵 %d 부활 지점에서 도달 불가 (도착해도 갇힘)",
                    i, portal.dst.x, portal.dst.y, portal.dstMapId);
                connectivityBad = true;
            }
        }
        const KYS::SERVERAPP::SpawnGroup* groups = KYS::SERVERAPP::SpawnGroupData();
        for (int i = 0; i < KYS::SERVERAPP::SpawnGroupCount() && !connectivityBad; ++i)
        {
            const KYS::SERVERAPP::SpawnGroup& g = groups[i];
            const Position respawn = MapTableAt(g.mapId).respawn;
            // 구역 대표점(중앙) 하나가 아니라 구역 안 통행 칸을 전부 검사한다 - 몬스터는 구역 어디든 랜덤 스폰되므로
            //   중앙만 보면 (a) 중앙이 벽이면 멀쩡한 구역을 오거부(false-FATAL) (b) 구석 격리 주머니를 놓친다(false-pass).
            //   로더(SpawnTable)의 통행칸 열거와 같은 규약 - 벽 칸은 스폰 안 되므로 제외, 통행 칸은 전부 부활 지점에서 도달 가능해야.
            const int c0 = g.x0 / WALK_CELL_SIZE;
            const int c1 = g.x1 / WALK_CELL_SIZE;
            const int r0 = g.y0 / WALK_CELL_SIZE;
            const int r1 = g.y1 / WALK_CELL_SIZE;
            for (int r = r0; r <= r1 && !connectivityBad; ++r)
            {
                for (int c = c0; c <= c1; ++c)
                {
                    const int cellX = (c * WALK_CELL_SIZE < g.x0) ? g.x0 : c * WALK_CELL_SIZE;   // 칸∩구역 교집합의 한 점(로더와 동일)
                    const int cellY = (r * WALK_CELL_SIZE < g.y0) ? g.y0 : r * WALK_CELL_SIZE;
                    if (!IsWalkable(g.mapId, cellX, cellY)) { continue; }   // 벽 칸 = 스폰 대상 아님 - 도달성 검사 제외
                    const Position cell = { cellX, cellY };
                    if (!IsReachable(g.mapId, respawn, cell))
                    {
                        ServerLog(LogLevel::LL_FATAL, L"boot", L"spawns.csv %d행 - 스폰 구역 통행 칸 (%d,%d) 이 맵 %d 부활 지점에서 도달 불가 (몬스터가 갇힌 섬에 스폰)",
                            i, cellX, cellY, g.mapId);
                        connectivityBad = true;
                        break;
                    }
                }
            }
        }
        if (connectivityBad)
        {
            mysql_library_end();
            for (int j = 0; j < CHANNEL_COUNT; ++j) { delete gameChannels[j]; }
            KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
            timeEndPeriod(1);
            return -1;
        }
    }
    ServerLog(LogLevel::LL_INFO, L"boot", L"통행 연결성 교차검증 OK (포탈/스폰 전부 부활 지점에서 도달 가능)");

    // 초기 몬스터 스폰 (부팅 2단계) - 콘텐츠 표(maps/monsters/spawns) 적재 검증이 전부 끝난 지금 명시 호출.
    //   생성자 스폰이면 파일 적재보다 먼저 돌아 빈 표로 스폰한다. 반드시 채널 스레드 기동 전(단일 스레드 구간) -
    //   기동 후엔 채널 스레드가 같은 맵 격자/풀을 무락으로 만지므로 여기가 마지막 안전 지점.
    for (int i = 0; i < CHANNEL_COUNT; ++i)
    {
        gameChannels[i]->SpawnInitialMonsters();
    }
    ServerLog(LogLevel::LL_INFO, L"boot", L"초기 몬스터 스폰 OK (채널 %d개 x 스폰 그룹 %d개)", CHANNEL_COUNT, spawnGroupCount);

    // DB 스레드 기동 + 연결(스레드측). 연결/스키마 실패면 fail-fast(서버는 DB 없이 못 돈다).
    //   Connect 안의 EnsureSchema 가 characters 테이블을 만든다 (accounts 는 로그인 서버 전용 소유, 게임 서버 미접근).
    if (!dbThread.Start(dbConfig))
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"DB 연결 실패 - 종료 (mysqld/자격증명/Server_Config.ini 확인)");
        dbThread.Stop();
        mysql_library_end();
        for (int i = 0; i < CHANNEL_COUNT; ++i) { delete gameChannels[i]; }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }

    ServerLog(LogLevel::LL_INFO, L"boot", L"MySQL 연결 OK (characters 스키마 확인)");

    HANDLE sstThread = reinterpret_cast<HANDLE>(
        _beginthreadex(NULL, 0, SharedServicesThreadEntry, &sst, 0, NULL));
    if (sstThread == NULL)
    {
        // SST 스레드 생성 실패 - 채널 스레드 기동 전이라 DB/채널 객체/타이머만 정리하고 부팅 중단.
        ServerLog(LogLevel::LL_FATAL, L"boot", L"SharedServicesThread 스레드 생성 실패");
        dbThread.Stop();
        mysql_library_end();
        for (int i = 0; i < CHANNEL_COUNT; ++i) { delete gameChannels[i]; }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -3;
    }

    // (5) 각 Channel TickLoop 스레드 기동 (_beginthreadex 사용, std::thread 금지).
    //     Start보다 먼저 - 스레드가 떠 있어야 첫 패킷의 메일박스 처리가 동작.
    //     all-or-abort: 하나라도 생성 실패면 이미 뜬 스레드를 정상 종료시키고 부팅 중단
    //     -> 정상 경로의 WaitForMultipleObjects 가 NULL 핸들을 만나지 않도록 전 핸들 valid 를 보장.
    HANDLE threads[CHANNEL_COUNT];
    for (int i = 0; i < CHANNEL_COUNT; ++i)
    {
        threads[i] = reinterpret_cast<HANDLE>(
            _beginthreadex(NULL, 0, ChannelThreadEntry, gameChannels[i], 0, NULL));
        if (threads[i] == NULL)
        {
            ServerLog(LogLevel::LL_FATAL, L"boot", L"채널 %d 스레드 생성 실패 - 이미 뜬 채널 정리 후 종료", i);
            for (int j = 0; j < i; ++j) { gameChannels[j]->Stop(); }   // 이미 뜬 채널 TickLoop 종료 신호
            if (i > 0) { WaitForMultipleObjects(static_cast<DWORD>(i), threads, TRUE, INFINITE); }   // 뜬 스레드 join
            for (int j = 0; j < i; ++j) { CloseHandle(threads[j]); }
            sst.Stop();
            WaitForSingleObject(sstThread, INFINITE);
            CloseHandle(sstThread);
            dbThread.Stop();
            mysql_library_end();
            for (int j = 0; j < CHANNEL_COUNT; ++j) { delete gameChannels[j]; }
            KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
            timeEndPeriod(1);
            return -3;
        }
    }

    // (6) gameServer.Start - INetEventHandler*로 주입. 첫 AcceptEx 게시 = 패킷 수신 시작.
    //     workerCount=-1(CPU x2 자동), acceptPoolSize=32 기본값 사용 -> port/sink/maxSession만 지정.
    if (!gameServer.Start(serverConfig.publicPort,
        &channelManager,                       // ChannelManager -> INetEventHandler* 로 변환
        -1, 32,
        static_cast<int>(DEFAULT_SESSION_POOL_CAPACITY)))  // 라이브러리 SessionPool 용량. 게임세션 풀은 별도(map+pool)라 sid>>32 슬롯 미러/범위초과 전제와 무관
    {
        // Start 실패: (5)에서 뜬 TickLoop 스레드를 정상 종료시키고 정리.

        sst.Stop();
        WaitForSingleObject(sstThread, INFINITE);
        CloseHandle(sstThread);

        for (int i = 0; i < CHANNEL_COUNT; ++i)
        {
            gameChannels[i]->Stop();    // m_running = false 신호 -> TickLoop 반환
        }
        WaitForMultipleObjects(CHANNEL_COUNT, threads, TRUE, INFINITE);
        dbThread.Stop();
        mysql_library_end();   // 다른 실패 경로/정상 teardown 과 대칭 (mysql_library_init 짝 - DB 스레드 join 후)
        for (int i = 0; i < CHANNEL_COUNT; ++i)
        {
            CloseHandle(threads[i]);
            delete gameChannels[i];
        }
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -2;
    }

    ServerLog(LogLevel::LL_INFO, L"boot", L"게임 서버 기동 OK (채널 %d개 / 게임 포트 %d listen)",
        CHANNEL_COUNT, static_cast<int>(serverConfig.publicPort));

    // (6a) 길찾기 스레드 기동 - WalkableTable 로드 후, 첫 우회 경로 요청 전(플레이어 접속 후에야 발생)이라 여기서 안전.
    //   실패해도 비치명(우회 없이 벽 앞 정지로 강등 - Phase C 동작) - WARN 후 계속.
    KYS::SERVERAPP::PathfinderThread& pathfinderThread = KYS::SERVERAPP::PathfinderThread::GetInstance();
    if (pathfinderThread.Start())
    {
        ServerLog(LogLevel::LL_INFO, L"boot", L"길찾기 스레드 기동 OK (A*/JPS 우회 경로 - 요청 큐 %d)", KYS::SERVERAPP::MAX_PATH_REQUESTS);
    }
    else
    {
        ServerLog(LogLevel::LL_WARN, L"boot", L"길찾기 스레드 기동 실패 - 우회 없이 가동 (벽 앞 정지로 강등)");
    }

    // (6b) 인터서버 링크 스레드 기동 - 로그인 서버 인터서버 포트로 connect + IS_REGISTER.
    //   게임 로그인 경로의 일부: 클라 CS_GAME_AUTH -> ChannelManager pre-auth -> EnqueueVerify -> 이 링크가 LoginServer 검증 질의 -> admission.
    //   링크 다운이어도 boot 비치명(백그라운드 재연결, 복구 전 로그인은 거부) - LoginLinkThread 가 자체 WSAStartup 하므로 기동 순서 무관.
    KYS::SERVERAPP::LoginLinkThread& loginLink = KYS::SERVERAPP::LoginLinkThread::GetInstance();
    const UINT32 kLoopbackIp = 0x7F000001u;   // 127.0.0.1 (host order) - 로그인 서버 접속 대상 전용. 자기 보고 주소는 설정에서 온다
    if (loginLink.Start(kLoopbackIp, LOGIN_INTERSERVER_PORT, 0, serverConfig.publicIp, serverConfig.publicPort, interConfig.secret))
    {
        ServerLog(LogLevel::LL_INFO, L"link", L"인터서버 링크 연결됨 (LoginServer 인터서버 포트 %u)", LOGIN_INTERSERVER_PORT);
    }
    else
    {
        ServerLog(LogLevel::LL_INFO, L"link", L"인터서버 링크 미연결 - 백그라운드 재연결 중 (LoginServer 기동 확인)");
    }

    // (6c) 워치독 기동 - 부팅 완료 후 시작 (로딩 구간을 감시 밖에 둬 오탐 원천 차단).
    //   채널 tick 카운터가 60초 무진행이면 행 판정 -> abort() -> CrashDump 덤프 -> 프로세스 종료 (재기동은 수동).
    KYS::SERVERAPP::ChannelWatchdog watchdog;
    if (watchdog.Start(gameChannels, CHANNEL_COUNT))
    {
        ServerLog(LogLevel::LL_INFO, L"boot", L"워치독 기동 OK (채널 %d개 감시 / 행 임계 %llums)",
            CHANNEL_COUNT, KYS::SERVERAPP::ChannelWatchdog::STALL_LIMIT_MS);
    }
    else
    {
        ServerLog(LogLevel::LL_WARN, L"boot", L"워치독 기동 실패 - 행 감지 없이 가동 (서비스는 정상)");
    }

    // (6d) 무음 소켓 회수 스레드 기동 - 라이브러리 SessionPool을 주기 sweep (pre-auth 슬롯 고갈 방어).
    //   event 생성 성공을 확인한 뒤에만 스레드를 띄운다 - 무효 event면 WaitForSingleObject가 즉시 실패로 돌아와 busy-spin이 되므로.
    HANDLE reaperStop = ::CreateEventW(NULL, TRUE, FALSE, NULL);   // 수동 리셋 종료 이벤트
    HANDLE reaperThread = NULL;
    if (reaperStop != NULL)
    {
        reaperThread = reinterpret_cast<HANDLE>(
            _beginthreadex(NULL, 0, GameReaperThreadEntry, reaperStop, 0, NULL));
    }
    if (reaperThread != NULL)
    {
        ServerLog(LogLevel::LL_INFO, L"boot", L"무음 소켓 회수 스레드 기동 OK (sweep %lums / 무수신 데드라인 %llums)",
            GAME_REAPER_INTERVAL_MS, GAME_NORECV_DEADLINE_MS);
    }
    else
    {
        ServerLog(LogLevel::LL_WARN, L"boot", L"회수 스레드 기동 실패 - 무음 소켓 회수 없이 가동 (서비스는 정상)");
    }

    // (6e) 지표 시계열 export 기동 - 전용 스레드가 10s 마다 지표를 read-only 수집해 Logs\ 에 JSONL 1행씩.
    //   'q' 모니터(순간값)와 독립 - 20초 표본 창 밖의 역사를 영속해 튜닝 전후 비교/사후 분석을 가능하게 한다.
    KYS::SERVERAPP::MetricExporter metricExporter;
    if (metricExporter.Start(L"ServerApp", gameChannels, CHANNEL_COUNT, &channelManager, &gameServer))
    {
        ServerLog(LogLevel::LL_INFO, L"boot", L"메트릭 export 기동 OK (주기 %lums / Logs\\*_metrics_*.jsonl)",
            KYS::SERVERAPP::MetricExporter::EXPORT_INTERVAL_MS);
    }
    else
    {
        ServerLog(LogLevel::LL_WARN, L"boot", L"메트릭 export 기동 실패 - 시계열 없이 가동 (서비스는 정상)");
    }

    // (7) graceful 종료 게이트 등록 - 콘솔 창 X / 로그오프 / 시스템 종료를 붙잡아 저장 후 종료.
    //   부팅 완료 후 등록(로딩 중 콘솔 종료 = 저장할 것 0이라 즉사 무해 - 워치독 배치 철학과 동일).
    if (!KYS::GAMESERVER::GracefulShutdown::Install())
    {
        ServerLog(LogLevel::LL_WARN, L"boot", L"graceful 종료 핸들러 등록 실패 - 콘솔 강제 종료 시 즉시 종료됨(저장 생략, 'x' 종료는 정상)");
    }

    // (8) 서버 가동 - 종료 입력('x') 또는 콘솔 종료 이벤트 대기.
    ServerLog(LogLevel::LL_INFO, L"boot", L"부팅 완료 - 모든 설정/리소스 적재됨");
    ::wprintf(L"====================================\n");
    ::wprintf(L"[q] 모니터  /  [x] 종료\n");
#ifdef _DEBUG
    ::wprintf(L"[h] 행 주입(워치독)  [d] 인벤저장 강제실패(H4) - Debug 전용\n");
#endif
    const DWORD SHUTDOWN_POLL_MS = 50;   // 키 입력 폴링 간격 - 블로킹 대신 짧게 폴링해 콘솔 종료 이벤트에도 반응
    while (!KYS::GAMESERVER::GracefulShutdown::WaitShutdownRequested(SHUTDOWN_POLL_MS))
    {
        if (!::_kbhit())
        {
            continue;   // 키 없음 - 종료 이벤트만 폴링(콘솔 창 X 시 WaitShutdownRequested 가 즉시 탈출시킴)
        }
        const wint_t key = ::_getwch();   // _kbhit 통과 후라 비블로킹
        if (key == L'q' || key == L'Q')
        {
            ::FlushConsoleInputBuffer(::GetStdHandle(STD_INPUT_HANDLE));   // 'q' 입력의 잔여(Enter 등) 제거
            // 모니터 모드 중에는 INFO 로그의 콘솔 병행 출력을 죽여 대시보드 재그리기와 섞이지 않게 (파일 기록은 그대로).
            KYS::GAMESERVER::LOG::Logger::GetInstance().SetConsoleEchoMinLevel(KYS::GAMESERVER::LOG::LogLevel::LL_WARN);
            // 모니터 모드: 화면을 주기적으로 다시 그린다. 아무 키나 누르거나 콘솔 종료 시 모드 종료.
            for (;;)
            {
                ClearConsole();
                monitor.Dump(gameChannels, CHANNEL_COUNT, channelManager.GetAuthenticatedCount(),
                    channelManager.GetPlayerPoolUsed(), channelManager.GetGameSessionPoolUsed(),
                    gameServer.GetSessionPoolUsedCount(),   // socketCount = 활성 소켓 슬롯 수 (누수 관측용 근사치)
                    &gameServer,                            // 송신 압력 스냅샷 진입점 (SnapshotSendMetrics + GetRetiredSendTotals - drop/wsaPost/wrap 총량 + 상위 N 느린 세션)
                    &channelManager);                       // 채널별/맵별 플레이어 집계 + 상위 N 세션 라벨(accountId/channelId)
                ::wprintf(L"\n(모니터 모드 - 아무 키나 누르면 종료)\n");
                if (KYS::GAMESERVER::GracefulShutdown::WaitShutdownRequested(MONITOR_REFRESH_MS))   // 갱신 주기 대기 겸 종료 이벤트 감지
                {
                    break;   // 콘솔 종료 - 모니터 중단하고 정리로 (바깥 while 도 즉시 탈출)
                }
                if (::_kbhit())          // 비블로킹 폴링 - 키 있을 때만 종료
                {
                    (void)::_getwch();   // 종료 키 소비
                    break;
                }
                // 키 없으면 루프 -> 다음 프레임 갱신 (라이브)
            }
            ClearConsole();
            KYS::GAMESERVER::LOG::Logger::GetInstance().SetConsoleEchoMinLevel(KYS::GAMESERVER::LOG::LogLevel::LL_INFO);   // 모니터 종료 - 콘솔 echo 복원
            ::wprintf(L"[q] 모니터  /  [x] 종료\n");
#ifdef _DEBUG
            ::wprintf(L"[h] 행 주입(워치독)  [d] 인벤저장 강제실패(H4) - Debug 전용\n");
#endif
        }
#ifdef _DEBUG
        else if (key == L'h' || key == L'H')
        {
            // 워치독 검증: 채널 0에 90초 정지 job 주입 -> tick 무진행 -> 60초 뒤 워치독이 강제 크래시(덤프).
            gameChannels[0]->EnqueueJob(new HangInjectJob());
            ::wprintf(L"[test] 채널 0에 행 주입 - 약 60초 뒤 워치독 강제 크래시(덤프) 예상\n");
        }
        else if (key == L'd' || key == L'D')
        {
            // H4 검증: 다음 인벤 저장 1회를 강제 실패시켜 낙관적 ClearDbDirty 재현. 줍기 후 눌러 관측.
            KYS::SERVERAPP::MySqlDBProvider::ArmInventoryFaultOnce();
            ::wprintf(L"[test] 다음 인벤 저장 1회 강제 실패 무장 (H4 재현·다음 저장 주기/로그아웃에 발동)\n");
        }
#endif
        else if (key == L'x' || key == L'X')
        {
            KYS::GAMESERVER::GracefulShutdown::RequestShutdown();   // 종료 이벤트 세움 (콘솔 핸들러와 동일 경로)
            break;   // 이벤트 생성 실패(극단 OOM) 시에도 즉시 탈출 보장
        }

        // 그 외 키 = 무시
    }

    // ===== 종료 정리 (역순 해제): Stop -> 스레드 신호 -> join -> CloseHandle -> delete -> timeEndPeriod =====
    // gameServer.Stop() 은 listen/accept/포트/Winsock 만 정리하고 active 세션은 끊지 않는다 - 접속 중 플레이어의 LEAVE 는 여기서 생기지 않는다(그 저장은 종료 저장 루틴이 담당).
    //   loginLink.Stop() 앞에 두는 건 종료 직전 정상 끊긴 세션의 잔여 OnSessionGone -> EnqueueSessionGone 을 링크 스레드가 살아있을 때 처리/송신하기 위함이고, loginLink.Stop() 직전 ThreadLoop 종료 drain 이 잔여를 비운다.
    //   shutdown 시점 접속 중이던 플레이어의 online 상태는 LEAVE 가 아니라 grace 후 SweepOnline 이 정리한다(수렴 backstop).
    ServerLog(LogLevel::LL_INFO, L"shutdown", L"종료 요청 감지 - 정리 시작 ('x' 키 또는 콘솔 종료)");   // 'x'/콘솔핸들러 양 경로의 수렴점
    watchdog.Stop();                                     // 워치독 최우선 정지 - 아래 정리로 채널 tick이 멈추는 것을 행으로 오판하지 않게
    if (reaperThread != NULL)                            // 무음 소켓 회수 스레드 정지 (pool teardown 전에 - reaper가 pool을 만짐)
    {
        ::SetEvent(reaperStop);
        ::WaitForSingleObject(reaperThread, INFINITE);
        ::CloseHandle(reaperThread);
    }
    if (reaperStop != NULL) { ::CloseHandle(reaperStop); }
    metricExporter.Stop();                               // 마지막 JSONL 1행 기록 후 정지 (채널 Stop 이전 - 살아있는 값 + UAF 차단)
    gameServer.Stop();                                   // AcceptEx 중단 + Worker 정리(라이브러리) - active 세션은 끊지 않음(접속 중 LEAVE 없음, 종료 저장 루틴이 담당)
    loginLink.Stop();                                    // 인터서버 링크 스레드 종료 (위 LEAVE 송신 후, 종료 drain, 활성 1왕복이면 recv 타임아웃 내 종료)
    sst.Stop();                                  // m_running=false -> Run이 마지막 비우기 후 반환
    WaitForSingleObject(sstThread, INFINITE);    // SST 스레드 join (채널 살아 있을 때 - 마지막 비우기의 배달 작업 전달 보장)
    CloseHandle(sstThread);

    pathfinderThread.Stop();   // 길찾기 스레드 종료 (채널 join 전 - 채널이 살아있는 동안 in-flight 결과가 mailbox로 회신되게. 잔여 요청은 undrained·잔여 결과는 mailbox 잔류, 프로세스 종료라 무해)

    for (int i = 0; i < CHANNEL_COUNT; ++i)
    {
        gameChannels[i]->Stop();                            // 각 TickLoop m_running=false
    }
    WaitForMultipleObjects(CHANNEL_COUNT, threads, TRUE, INFINITE);   // 모든 TickLoop 반환 대기
    dbThread.Stop();
    mysql_library_end();   // MySQL C 라이브러리 전역 정리 (DB 스레드 join 후).

    // 로거 정리 - 모든 로그 생산 스레드(채널/SST/DB/링크/reaper/워치독)가 join 된 지금이 유일하게 안전한 지점.
    //   마지막 행까지 파일로 밀어낸 뒤 닫는다. 이후 늦은 Log 호출(콘솔핸들러 스레드 등)은 콘솔로만 나간다.
    ServerLog(LogLevel::LL_INFO, L"shutdown", L"종료 정리 완료 - 프로세스 종료");
    KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();

    for (int i = 0; i < CHANNEL_COUNT; ++i)
    {
        CloseHandle(threads[i]);                         // 스레드 핸들 닫기
        delete gameChannels[i];                             // 스레드 join 후에만 delete (죽은 객체 참조 방지)
    }
    timeEndPeriod(1);                                    // 타이머 분해능 복구 (timeBeginPeriod 짝)

    KYS::GAMESERVER::GracefulShutdown::SignalTeardownDone();   // 정리 완주 - 콘솔 핸들러가 대기 중이면(창 X 경로) 깨워 프로세스 종료 진행

    return 0;
    // (gameServer/channelManager 등은 싱글턴 - 프로그램 종료 시 생성 역순으로 소멸. 위 명시 Stop/join으로 스레드는 이미 정리됨.
    //  ChannelManager 소멸자가 게임세션 풀 자동 정리. ~IOCPServer/Stop은 INVALID_SOCKET 가드로 멱등 - sink deref 0.)
}
