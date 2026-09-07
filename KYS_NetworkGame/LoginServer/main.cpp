#include "pch_loginserver.h"
#include "Network/LoginIocpServer.h"            // KYS::LOGINSERVER::LoginIocpServer (: public IOCPServer)
#include "Network/LoginDispatcher.h"            // KYS::LOGINSERVER::LoginDispatcher (: public INetEventHandler)
#include "Network/InterServerIocpServer.h"      // KYS::LOGINSERVER::InterServerIocpServer (2번째 포트 - 게임 서버 전용)
#include "Network/InterServerDispatcher.h"      // KYS::LOGINSERVER::InterServerDispatcher (IS_* 인바운드 라우터)
#include "Auth/AccountManager.h"                // KYS::LOGINSERVER::AccountManager (accounts MySQL 적재 + 로그인 검증)
#include "../GameServer/Core/Config/ServerConfig.h"  // DbConfig / ServerConfig / InterServerConfig / LoadServerConfig
#include "Auth/TokenStore.h"                    // KYS::LOGINSERVER::TokenStore (일회용 토큰 적재 + 만료 청소)
#include "DataBase/LoginDbThread.h"            // KYS::LOGINSERVER::LoginDbThread (계정 생성 INSERT 전용 스레드)
#include "../GameServer/Types/Defines.h"        // DEFAULT_SESSION_POOL_CAPACITY / USHORT
#include "../GameServer/Core/Lifecycle/CrashDump.h"        // KYS::GAMESERVER::CrashDump (크래시 미니덤프 - 두 exe 공유 인프라)
#include "../GameServer/Core/Lifecycle/GracefulShutdown.h" // KYS::GAMESERVER::GracefulShutdown (graceful 종료 - 두 exe 공유 인프라)
#include "../GameServer/Core/Log/Logger.h"       // KYS::GAMESERVER::LOG::Logger (서버/유저 이벤트 파일 로그 - 두 exe 공유 인프라)
#include "Handler/LoginHandler.h"               // 로그인 시도 cap 차단 누적 (모니터 표출)
#include "../GameCommon/GameDefines.h"          // LOGIN_CLIENT_PORT (클라 대면 포트) / LOGIN_INTERSERVER_PORT (인터서버 전용 포트)
#include <process.h>                            // _beginthreadex (std::thread 금지)
#include <Windows.h>                            // CreateEventW / GetTickCount64 / WaitForSingleObject (timeBeginPeriod 계열은 pch <timeapi.h> 제공)
#include <conio.h>                              // _getwch (블로킹 1키 입력)
#include <clocale>                              // setlocale (콘솔 한글 출력)
#include <cstdlib>                              // strtol (information_schema COUNT 파싱 - 스키마 마이그레이션 점검)

using KYS::GAMESERVER::LOG::LogLevel;

// main 전용 축약 - server 채널 파일 로그 (INFO 이상은 콘솔에도 병행 출력)
template<typename... Args>
static void ServerLog(LogLevel level, const wchar_t* tag, const wchar_t* format, Args&&... args)
{
    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
        KYS::GAMESERVER::LOG::LogChannel::SERVER, level, tag, format, std::forward<Args>(args)...);
}

// 크래시 덤프(.dmp) 확보 후 로그 큐 잔량을 best-effort 로 밀어내는 다리 (ServerApp 과 동일 패턴).
static void FlushLogsAfterDump()
{
    KYS::GAMESERVER::LOG::Logger::GetInstance().EmergencyFlush();
}

// 콘솔 QuickEdit 끄기 - 드래그 선택의 콘솔 write 블록이 로그 찍던 스레드를 결박하는 것을 차단 (ServerApp 미러).
static void DisableConsoleQuickEdit()
{
    HANDLE hIn = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (hIn != INVALID_HANDLE_VALUE && ::GetConsoleMode(hIn, &mode))
    {
        ::SetConsoleMode(hIn, (mode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
    }
}

static volatile LONG g_reapedTotal = 0;   // 무음 pre-auth 소켓 회수 누적 (sweep 스레드 Interlocked - 모니터 표출)




// 콘솔을 지우고 커서를 홈으로 - 모니터 지표를 스크롤 없이 제자리 갱신(ServerApp 모니터와 동일 방식 미러).
//   모던 터미널(Windows Terminal/VS Code)은 Win32 FillConsole 이 스크롤백을 못 비워 과거 출력이 위로 쌓인다.
//   VT(가상터미널) 모드를 켜고 ANSI 이스케이프로 화면 + 스크롤백을 함께 비우면 cmd/Windows Terminal/VS Code 모두 제자리 갱신.
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

static const DWORD MONITOR_REFRESH_MS = 1000;   // 모니터 모드 화면 갱신 주기 ms

// 모니터 1프레임 출력(인터서버 통계). 화면 비우기는 호출자(모니터 루프)가 ClearConsole 로 한다.
//   online=현재 접속 계정 수(ServerApp CCU와 대조 - kick 정상이면 같아야 함), tokens=미소비 토큰,
//   verify(req/ok)=검증 요청/성공, kick=중복 로그인 축출, leave=접속 종료 통지.
static void DumpLoginMonitor(KYS::LOGINSERVER::TokenStore& tokenStore)
{
    ::wprintf(L"===== LoginServer 모니터 =====\n");
    ::wprintf(L"  online        = %d   (현재 접속 계정 - ServerApp CCU 와 대조)\n", tokenStore.GetOnlineCount());
    ::wprintf(L"  tokens        = %d   (미소비 토큰)\n", tokenStore.GetTokenCount());
    ::wprintf(L"  verify req/ok = %ld / %ld   (토큰 검증 요청/성공)\n",
        KYS::LOGINSERVER::InterServerDispatcher::s_verifyReq,
        KYS::LOGINSERVER::InterServerDispatcher::s_verifyOk);
    ::wprintf(L"  kick          = %ld   (중복 로그인 축출)\n", KYS::LOGINSERVER::InterServerDispatcher::s_kick);
    ::wprintf(L"  leave         = %ld   (접속 종료 통지)\n", KYS::LOGINSERVER::InterServerDispatcher::s_leave);
    ::wprintf(L"  send drop     = %ld   (인터서버 링크 송신 실패/백프레셔 - IS_KICK/RES 유실, 0 이 정상)\n",
        KYS::LOGINSERVER::InterServerDispatcher::GetSendDropCount());
    ::wprintf(L"  online sync   = %ld   (권위 online 스냅샷 수신 - 주기 수신이 정상)\n",
        KYS::LOGINSERVER::InterServerDispatcher::s_onlineSync);
    ::wprintf(L"  attempt block = %ld   (연결당 시도 cap 차단 - flood 방어, 0 이 정상)\n",
        KYS::LOGINSERVER::LoginHandler::GetAttemptBlockCount());
    ::wprintf(L"  reaped        = %ld   (무음 pre-auth 소켓 회수 - 슬롯 고갈 방어)\n", g_reapedTotal);
    ::wprintf(L"  log drop/fail = %llu / %llu   (로그 큐 가득 유실 / 파일 쓰기 실패 - 둘 다 0 이 정상)\n",
        KYS::GAMESERVER::LOG::Logger::GetInstance().GetDropCount(),
        KYS::GAMESERVER::LOG::Logger::GetInstance().GetWriteFailCount());
    ::wprintf(L"  db reconnect  = %ld   (계정 DB 재접속 - 0 이 정상)\n", KYS::LOGINSERVER::LoginDbThread::s_reconnectCount);
}


// 만료 토큰 청소 간격 ms. 발급 후 LOGIN_TOKEN_TTL_MS 안에 게임 서버로 제출 안 된 토큰을 이 주기로 걷어낸다.
static const DWORD TOKEN_SWEEP_INTERVAL_MS = 1000;
static const UINT64 LOGIN_NORECV_DEADLINE_MS = 15000;   // 무음 소켓(연결 후 CS_LOGIN 무전송) 회수 데드라인 - 로그인은 즉시성 커서 짧게(정상 handshake<2s). 7001 클라 서버만 적용(7002 링크는 장수명이라 제외)


// accounts 테이블 보장 (부팅 1회). LoadFromDb 의 SELECT 보다 '먼저' 실행해 빈 DB 첫 부팅에서
//   테이블 부재로 SELECT 가 실패하는 트랩을 피한다(EnsureSchema 가 LoadFromDb 보다 앞).
//   accounts 스키마는 LoginServer 가 전용 소유하므로 생성 책임도 LoginServer 부팅에 둔다.
//   account_id = AUTO_INCREMENT 영속 키(캐릭터 소유 FK 안정화, feature B), login_name = UNIQUE 로그인 키, pw_hash = self-describing 해시 문자열.
static bool EnsureAccountsSchema(const DbConfig& config)
{
    MYSQL* conn = mysql_init(nullptr);
    if (conn == nullptr)
    {
        return false;   // 핸들 할당 실패(OOM)
    }

    // charset 은 connect '전' 옵션 - 한글(utf8mb4) 유지. 날 SET NAMES 금지.
    mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (mysql_real_connect(conn, config.host, config.user, config.password,
                           config.dbname, static_cast<unsigned int>(config.port),
                           nullptr, 0) == nullptr)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"accounts 스키마 연결 실패: %S", mysql_error(conn));
        mysql_close(conn);
        return false;
    }

    // 마이그레이션(feature B characters 와 동형): 구 accounts(PK=login_name, account_id 없음, pw_hash INT)가 남아 있으면 통째 교체.
    //   account_id 컬럼 유무로 신/구를 가른다 - 없으면(구 스키마거나 미존재) DROP IF EXISTS 후 아래 CREATE 로 재생성.
    //   구 봇 시드는 seed_bots.sql 로 재적재 가능이라 통째 교체가 안전(학습/개발 DB 전제).
    static const char* const kHasAccountId =
        "SELECT COUNT(*) FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'accounts' AND COLUMN_NAME = 'account_id'";
    int hasAccountId = -1;
    if (mysql_query(conn, kHasAccountId) == 0)
    {
        MYSQL_RES* colRes = mysql_store_result(conn);
        if (colRes != nullptr)
        {
            MYSQL_ROW colRow = mysql_fetch_row(colRes);
            if (colRow != nullptr && colRow[0] != nullptr) { hasAccountId = static_cast<int>(strtol(colRow[0], nullptr, 10)); }
            mysql_free_result(colRes);
        }
    }
    if (hasAccountId < 0)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"accounts 스키마 점검 실패: %S", mysql_error(conn));
        mysql_close(conn);
        return false;
    }
    if (hasAccountId == 0)
    {
        // 구 스키마(login_name PK)거나 미존재 - 비호환이라 비우고 재생성(IF EXISTS 라 미존재면 무해).
        if (mysql_query(conn, "DROP TABLE IF EXISTS accounts") != 0)
        {
            ServerLog(LogLevel::LL_FATAL, L"boot", L"구 accounts DROP 실패: %S", mysql_error(conn));
            mysql_close(conn);
            return false;
        }
    }

    static const char* const kCreateAccounts =
        "CREATE TABLE IF NOT EXISTS accounts ("
        " account_id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
        " login_name VARCHAR(16) NOT NULL,"
        " pw_hash VARCHAR(128) NOT NULL,"
        " PRIMARY KEY (account_id),"
        " UNIQUE KEY uq_login_name (login_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci";

    const bool ok = (mysql_query(conn, kCreateAccounts) == 0);
    if (!ok)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"CREATE accounts 실패(권한?): %S", mysql_error(conn));
    }
    mysql_close(conn);
    return ok;
}


// 로드테스트 봇 계정 시드는 부팅 경로에서 제거됐다(기능 A 로 실 계정 생성 도입 후 매 부팅 6000봇 주입은 노이즈).
//   로드테스트 시에만 명시적으로 Data/seed_bots.sql 을 적재한다 - 절차는 그 파일 헤더 주석 참조
//   (요지: LoginServer 1회 부팅으로 스키마 생성 -> 종료 -> mysql < seed_bots.sql -> 재부팅[LoadFromDb 가 봇 적재] -> 로드테스트).


// 만료 토큰 청소 스레드. 정지 이벤트가 올 때까지 일정 주기로 깨어 TTL 지난 토큰을 제거한다.
//   stopEvent 가 신호되면 즉시 반환(종료). 큐 폴링이 아니라 이벤트 대기라 idle CPU 0.
static unsigned __stdcall TokenSweepThreadEntry(void* arg)
{
    HANDLE stopEvent = static_cast<HANDLE>(arg);
    KYS::LOGINSERVER::TokenStore& tokenStore = KYS::LOGINSERVER::TokenStore::GetInstance();
    KYS::LOGINSERVER::LoginIocpServer& loginServer = KYS::LOGINSERVER::LoginIocpServer::GetInstance();   // 7001 클라 서버(무음 소켓 회수 대상)

    for (;;)
    {
        const DWORD wait = ::WaitForSingleObject(stopEvent, TOKEN_SWEEP_INTERVAL_MS);
        if (wait == WAIT_OBJECT_0)
        {
            break;   // 종료 신호 - 마지막 청소 없이 반환
        }
        // TTL 지난 토큰 제거. nowMs = 부팅 후 경과 ms(단조 증가) - TokenIssuer 발급 시각과 같은 시계여야 함.
        //   모니터 출력은 sweep 스레드가 아니라 main 의 'q' 모니터 모드에서 그린다(ServerApp 방식 - 상시 출력 X).
        tokenStore.SweepExpired(::GetTickCount64());
        tokenStore.SweepOnline(ONLINE_GRACE_MS);   // grace 넘게 미갱신된 online ghost(RES 유실, LEAVE 드롭, 링크끊김, shutdown) 통합 정리 (now 는 SweepOnline 이 락 안에서 읽음)
        const int reaped = loginServer.ReapIdleSessions(::GetTickCount64(), LOGIN_NORECV_DEADLINE_MS);   // 무음 pre-auth 소켓(연결만 하고 CS_LOGIN 무전송) 회수 - 슬롯 고갈 방어
        if (reaped > 0)
        {
            ::InterlockedAdd(&g_reapedTotal, static_cast<LONG>(reaped));   // flood 방어 관측 누적 (모니터 표출)
            ServerLog(LogLevel::LL_INFO, L"reaper", L"무음 소켓 %d개 회수", reaped);
        }
    }
    return 0;
}


int wmain(int argc, wchar_t* argv[])    // 표준 진입점(전역). wchar_t(Unicode)
{
    (void)argc; (void)argv;             // 인자 미사용 (포트/설정은 conf/Server_Config.ini)
    ::setlocale(LC_ALL, "");

    // (0) 크래시 핸들러 등록 (무엇보다 먼저 - 초기화 중 크래시도 커버). 프로세스 전역이라 이후 만든 모든 스레드 커버.
    KYS::GAMESERVER::CrashDump::Install(L"LoginServer");

    // (0b) 파일 로거 기동 - 부팅 로그부터 Logs\ 에 영속 (ServerApp 미러: 덤프 후 flush 콜백 + QuickEdit 차단).
    KYS::GAMESERVER::LOG::Logger::GetInstance().Initialize(L"LoginServer");
    KYS::GAMESERVER::CrashDump::SetPostDumpCallback(&FlushLogsAfterDump);
    DisableConsoleQuickEdit();

    // (1) 타이머 분해능 1ms 상향 (프로세스 전역, 1회).
    timeBeginPeriod(1);

    // (2) 싱글턴 3개 획득 (Meyers static - Start 보류, 주소만 확보).
    //     GetInstance 첫 호출 순서가 소멸 역순을 고정. 참조 별칭이라 split-brain 0.
    KYS::LOGINSERVER::LoginIocpServer& loginServer = KYS::LOGINSERVER::LoginIocpServer::GetInstance();
    KYS::LOGINSERVER::LoginDispatcher& dispatcher  = KYS::LOGINSERVER::LoginDispatcher::GetInstance();
    KYS::LOGINSERVER::TokenStore&      tokenStore  = KYS::LOGINSERVER::TokenStore::GetInstance();
    (void)tokenStore;   // 적재는 핸들러/sweep 스레드에서. 여기선 생성 순서만 고정.

    // 인터서버(게임 서버 전용) 서버 + 디스패처 - 클라 대면과 별개의 2번째 포트. 같은 TokenStore 를 소비한다.
    KYS::LOGINSERVER::InterServerIocpServer& interServer     = KYS::LOGINSERVER::InterServerIocpServer::GetInstance();
    KYS::LOGINSERVER::InterServerDispatcher& interDispatcher = KYS::LOGINSERVER::InterServerDispatcher::GetInstance();
    KYS::LOGINSERVER::LoginDbThread&         dbThread        = KYS::LOGINSERVER::LoginDbThread::GetInstance();   // 계정 생성 INSERT 전용 스레드

    // (3) MySQL C 클라이언트 라이브러리 전역 초기화 - 어떤 스레드도 MySQL 쓰기 전 1회(스레드 생성 앞).
    if (mysql_library_init(0, NULL, NULL) != 0)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"mysql_library_init 실패");
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }

    // (4) 서버 설정 적재 (conf/Server_Config.ini). 실패(파일 없음/필수 키 누락/값 형식 오류) = fail-fast.
    //     한 파일에서 DB 접속 / 게임 서버 공개 주소 / 인터서버 링크 비밀을 한 번에 읽는다 - 어느 키가 문제인지는 로더가 콘솔에 남긴다.
    DbConfig          dbConfig;
    ServerConfig      serverConfig;
    InterServerConfig interConfig;
    if (!LoadServerConfig(&dbConfig, &serverConfig, &interConfig))
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"Server_Config.ini 적재 실패 - 위 콘솔 메시지의 키를 확인한다");
        mysql_library_end();
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }

    // (5) accounts 스키마 보장 - LoadFromDb '전'에 테이블 생성(빈 DB 첫 부팅 SELECT 실패 트랩 회피).
    if (!EnsureAccountsSchema(dbConfig))
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"accounts 스키마 생성 실패 - 종료");
        mysql_library_end();
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }

    // (6) 계정 적재 (부팅 1회, accounts MySQL, LoginServer 전용 소유) - 런타임 계정 생성은 AccountManager 가 SRWLock 으로 추가.
    //     로드테스트 봇은 부팅 시드를 폐기 - 미리 Data/seed_bots.sql 을 적재해 두면 이 LoadFromDb 가 m_accounts 에 함께 올린다.
    //     음수 = 연결/쿼리 실패(fail-fast). 0 = 계정 없음(시드 확인, 치명 아님).
    const int loadedAccounts =
        KYS::LOGINSERVER::AccountManager::GetInstance().LoadFromDb(dbConfig);
    if (loadedAccounts < 0)
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"accounts 적재 DB 실패");
        mysql_library_end();
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }
    ServerLog(LogLevel::LL_INFO, L"boot", L"accounts loaded: %d", loadedAccounts);

    // (7) 디스패처에 서버 핸들 주입 (응답 SendTo / DisconnectTo 진입점). Start 전 필수
    //     (첫 패킷의 EnqueuePacket 이 서버 핸들로 응답을 보내므로 Start 보다 앞).
    if (!dispatcher.Init(&loginServer))
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"LoginDispatcher.Init 실패");
        mysql_library_end();
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }

    // (7b) 인터서버 디스패처에 인터서버 서버 핸들 주입 (응답 Send 진입점). Start 전 필수.
    if (!interDispatcher.Init(&interServer))
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"InterServerDispatcher.Init 실패");
        mysql_library_end();
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }

    interDispatcher.SetAuthSecret(interConfig.secret);   // conf/Server_Config.ini [interserver] secret 주입 (부팅 검증으로 non-zero 보장) - IS_REGISTER authKeyHash 대조에 사용
    KYS::LOGINSERVER::LoginHandler::SetGameServerAddress(serverConfig.publicIp, serverConfig.publicPort);   // conf/Server_Config.ini [server] public_ip/public_port 주입 - SC_SERVER_LIST/SC_LOGIN_TOKEN 응답에 실림 (워커 Start 전이라 이후 read-only)

    // (8) 만료 토큰 청소 스레드 기동 (TTL = LOGIN_TOKEN_TTL_MS, 청소 주기 = TOKEN_SWEEP_INTERVAL_MS).
    //     manual-reset 이벤트로 종료 신호. CreateEventW(W-API).
    HANDLE sweepStop = ::CreateEventW(NULL, TRUE, FALSE, NULL);   // manual-reset, 초기 비신호
    HANDLE sweepThread = NULL;
    if (sweepStop != NULL)   // 이벤트 생성 실패 시 스레드 안 만듦 - NULL stop 이벤트면 WaitForSingleObject(NULL)이 매번 즉시 실패해 busy-spin + 종료 hang (ServerApp reaper 가드와 동형)
    {
        sweepThread = reinterpret_cast<HANDLE>(
            _beginthreadex(NULL, 0, TokenSweepThreadEntry, sweepStop, 0, NULL));
    }

    // (8b) 계정 생성 전용 DB 스레드 기동 (상시, async, 클라 accept 직전). 스레드측 connect 결과로 fail-fast.
    if (!dbThread.Start(dbConfig))
    {
        ServerLog(LogLevel::LL_FATAL, L"boot", L"LoginDbThread.Start 실패 (DB 연결?)");
        dbThread.Stop();   // connect 실패 시 DB 스레드는 mysql_thread_end() 중 - join 후에야 mysql_library_end() 안전(미호출 시 race/UB + 핸들 누수). (9)/(9b)와 동형.
        ::SetEvent(sweepStop);
        if (sweepThread != NULL) { ::WaitForSingleObject(sweepThread, INFINITE); ::CloseHandle(sweepThread); }
        ::CloseHandle(sweepStop);
        mysql_library_end();
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -1;
    }

    // (9) LoginIocpServer.Start - 클라 포트 listen + 디스패처를 통로로 주입. WSAStartup 은 Start 내부.
    //     workerCount=-1(CPU x2 자동), acceptPoolSize=32. maxSession = 라이브러리 SessionPool 용량.
    if (!loginServer.Start(LOGIN_CLIENT_PORT,
        &dispatcher,                            // LoginDispatcher -> INetEventHandler* 로 변환
        -1, 32,
        static_cast<int>(DEFAULT_SESSION_POOL_CAPACITY)))
    {
        // Start 실패: DB 스레드 + sweep 스레드 종료 -> 핸들 정리 -> MySQL/타이머 복구.
        dbThread.Stop();
        ::SetEvent(sweepStop);
        if (sweepThread != NULL)
        {
            ::WaitForSingleObject(sweepThread, INFINITE);
            ::CloseHandle(sweepThread);
        }
        ::CloseHandle(sweepStop);
        mysql_library_end();
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -2;
    }

    // (9b) InterServerIocpServer.Start - 게임 서버 전용 2번째 포트. 외부 클라는 못 닿는다(신뢰 경계를 포트로 물리화).
    //      트래픽이 극히 적어 워커 2 / accept 풀 4 / 세션 슬롯 64 로 경량 구성(클라 서버와 별도 풀).
    if (!interServer.Start(LOGIN_INTERSERVER_PORT, &interDispatcher, 2, 4, 64))
    {
        // 인터서버 포트 Start 실패: 클라 서버 + DB 스레드 + sweep 스레드 정리 후 종료.
        loginServer.Stop();
        dbThread.Stop();
        ::SetEvent(sweepStop);
        if (sweepThread != NULL)
        {
            ::WaitForSingleObject(sweepThread, INFINITE);
            ::CloseHandle(sweepThread);
        }
        ::CloseHandle(sweepStop);
        mysql_library_end();
        KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();   // 부팅 실패 - 큐에 남은 로그를 파일로 밀어낸 뒤 종료
        timeEndPeriod(1);
        return -3;
    }

    // 부팅 완료 - 두 포트 listen 개시를 파일에도 남긴다 (server.log 상 부팅 서사가 여기서 완결).
    ServerLog(LogLevel::LL_INFO, L"boot", L"부팅 완료 - 클라 포트 %u / 인터서버 포트 %u listen, 광고할 게임 서버 %u.%u.%u.%u:%u",
        LOGIN_CLIENT_PORT, LOGIN_INTERSERVER_PORT,
        (serverConfig.publicIp >> 24) & 0xFF, (serverConfig.publicIp >> 16) & 0xFF,
        (serverConfig.publicIp >> 8) & 0xFF, serverConfig.publicIp & 0xFF, serverConfig.publicPort);

    // graceful 종료 게이트 등록 - 콘솔 창 X / 로그오프 / 시스템 종료를 붙잡아 정리 후 종료.
    //   LoginServer 는 write-through(계정)라 종료 저장 pass 는 없다 - 기존 정리(Stop/join) 만 끝까지 돌리면 유실 0.
    if (!KYS::GAMESERVER::GracefulShutdown::Install())
    {
        ServerLog(LogLevel::LL_WARN, L"boot", L"graceful 종료 핸들러 등록 실패 - 콘솔 강제 종료 시 즉시 종료됨('x' 종료는 정상)");
    }

    // (10) 가동 - 종료('x')/모니터 입력 또는 콘솔 종료 이벤트 대기.
    ::wprintf(L"[LoginServer] 클라 포트 %u / 인터서버 포트 %u 대기 중  /  [q] 모니터  /  [x] 종료\n",
        LOGIN_CLIENT_PORT, LOGIN_INTERSERVER_PORT);
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
                DumpLoginMonitor(tokenStore);
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
            ::wprintf(L"[LoginServer] 클라 포트 %u / 인터서버 포트 %u 대기 중  /  [q] 모니터  /  [x] 종료\n",
                LOGIN_CLIENT_PORT, LOGIN_INTERSERVER_PORT);
        }
        else if (key == L'x' || key == L'X')
        {
            KYS::GAMESERVER::GracefulShutdown::RequestShutdown();   // 종료 이벤트 세움 (콘솔 핸들러와 동일 경로)
            break;   // 이벤트 생성 실패(극단 OOM) 시에도 즉시 탈출 보장
        }
        // 그 외 키 = 무시
    }

    // ===== 종료 정리 (역순 해제): Stop -> sweep 종료 신호 -> join -> CloseHandle -> MySQL/타이머 복구 =====
    ServerLog(LogLevel::LL_INFO, L"shutdown", L"종료 요청 감지 - 정리 시작 ('x' 키 또는 콘솔 종료)");
    interServer.Stop();                          // 인터서버 accept 중단 (게임 서버 링크 통로)
    loginServer.Stop();                          // AcceptEx 중단, Worker 정리(라이브러리)
    dbThread.Stop();                             // 계정 생성 DB 스레드 종료 (남은 명령 마저 처리 후 mysql_close)
    ::SetEvent(sweepStop);                        // sweep 스레드 종료 신호
    if (sweepThread != NULL)
    {
        ::WaitForSingleObject(sweepThread, INFINITE);   // sweep 스레드 join
        ::CloseHandle(sweepThread);
    }
    ::CloseHandle(sweepStop);
    mysql_library_end();                         // MySQL C 라이브러리 전역 정리

    // 로거 정리 - 모든 로그 생산 스레드(worker/DB/sweep)가 join 된 뒤. 마지막 행까지 파일로 밀어낸 뒤 닫는다.
    ServerLog(LogLevel::LL_INFO, L"shutdown", L"종료 정리 완료 - 프로세스 종료");
    KYS::GAMESERVER::LOG::Logger::GetInstance().Shutdown();

    timeEndPeriod(1);                            // 타이머 분해능 복구 (timeBeginPeriod 짝)

    KYS::GAMESERVER::GracefulShutdown::SignalTeardownDone();   // 정리 완주 - 콘솔 핸들러가 대기 중이면(창 X 경로) 깨워 프로세스 종료 진행

    return 0;
    // (loginServer/dispatcher/tokenStore 는 싱글턴 - 프로그램 종료 시 생성 역순 소멸. 위 명시 Stop/join 으로 스레드는 이미 정리됨.
    //  ~IOCPServer/Stop 은 INVALID_SOCKET 가드로 멱등 - 통로 deref 0.)
}
