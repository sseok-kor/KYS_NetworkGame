#include "pch_dummyclient.h"
#include "StressTestManager.h"
#include "../../GameCommon/GameDefines.h"   // LOGIN_CLIENT_PORT (로그인 서버 클라 대면 포트 - Stage 1 토큰 획득)
#include <process.h>   // _beginthreadex
#include <cwchar>      // wcscpy_s

StressTestManager::StressTestManager()
    : m_iocp(NULL)
    , m_workers(NULL)
    , m_workerCount(0)
    , m_bots(NULL)
    , m_maxBotCount(0)
    , m_activeCount(0)
    , m_serverIp{}
    , m_gamePort(0)
    , m_timerThread(NULL)
    , m_timerRunning(false)
    , m_churnMode(false)
    , m_dupLoginMode(false)
    , m_acctStart(0)
    , m_acctEnd(0)
{
}

void StressTestManager::SetTestMode(bool churnMode, bool dupLoginMode, int acctStart, int acctEnd)
{
    m_churnMode = churnMode;
    m_dupLoginMode = dupLoginMode;
    m_acctStart = acctStart;
    m_acctEnd = acctEnd;
}

StressTestManager::~StressTestManager()
{
    // main 이 명시적으로 Shutdown 을 부른다는 전제. 안전망으로 다시 호출(가드로 idempotent).
    Shutdown();
}

bool StressTestManager::Init(int maxBotCount, const wchar_t* serverIp, unsigned short gamePort)
{
    ::wcscpy_s(m_serverIp, 64, serverIp);   // 서버 IP 보관 (RampConnect 가 사용)
    m_gamePort = gamePort;                  // 게임 서버 포트 보관 (설정 파일 값 - RampConnect/TimerProc 가 사용)

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;

    m_iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (m_iocp == NULL)
    {
        ::WSACleanup();
        return false;
    }

    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    m_workerCount = static_cast<int>(si.dwNumberOfProcessors);
    if (m_workerCount < 1)
        m_workerCount = 1;

    m_workers = new HANDLE[m_workerCount];
    for (int i = 0; i < m_workerCount; ++i)
        m_workers[i] = NULL;

    for (int i = 0; i < m_workerCount; ++i)
    {
        m_workers[i] = reinterpret_cast<HANDLE>(
            ::_beginthreadex(NULL, 0, WorkerProc, this, 0, NULL));
        if (m_workers[i] == NULL)
            return false;   // 부분 실패 - dtor/Shutdown 가드가 회수
    }

    m_maxBotCount = maxBotCount;
    m_activeCount = 0;
    m_bots = new DummySession[maxBotCount];   // 상한 사전 할당 (런타임 추가 슬롯)

    // Stage 2: 주기 행동 타이머 스레드 1개 기동 (봇당 스레드 금지 - 타이머가 PQCS로 워커에 행동 분배).
    m_timerRunning = true;
    m_timerThread = reinterpret_cast<HANDLE>(::_beginthreadex(NULL, 0, TimerProc, this, 0, NULL));
    if (m_timerThread == NULL)
        return false;   // 부분 실패 - dtor/Shutdown 가드가 회수

    return true;
}

void StressTestManager::RampConnect(int fromIndex, int count)
{
    // 점진 ramp-up: 한 배치(RAMP_BATCH)만 연결한 뒤 잠깐 쉬어 accept 폭주를 완화한다(즉시 burst 금지 - thundering herd).
    const int RAMP_BATCH = 50;          // 배치당 연결 수
    const int RAMP_INTERVAL_MS = 100;   // 배치 간 간격 (초당 ~500 connect)

    int end = fromIndex + count;
    if (end > m_maxBotCount) { end = m_maxBotCount; }   // 배열 상한 가드 (초과 요청은 상한까지만)

    // 중복 로그인 모드면 봇 i 는 좁은 계정 범위를 % 로 공유(bot{start + i%range}) -> 봇 수 > 범위 면 중복 로그인 -> kick.
    //   일반 모드면 계정 = 슬롯 인덱스(봇마다 고유). 슬롯 i 는 위상/재연결 식별로 그대로 사용(공유 계정과 분리).
    const int acctRange = (m_dupLoginMode && m_acctEnd >= m_acctStart) ? (m_acctEnd - m_acctStart + 1) : 0;

    int connected = 0;
    int failed = 0;
    for (int i = fromIndex; i < end; ++i)
    {
        const int accountIndex = (acctRange > 0) ? (m_acctStart + (i % acctRange)) : i;
        const bool badCred = DummySession::s_harness.badCred && (i < HARNESS_BADCRED_COUNT);   // H12: 앞 N개 슬롯을 존재하지 않는 계정으로
        if (m_bots[i].AcquireToken(m_serverIp, LOGIN_CLIENT_PORT, accountIndex, badCred)   // Stage 1: LoginServer 핸드셰이크 -> 토큰 (로그인 계정=accountIndex)
            && m_bots[i].Connect(m_serverIp, m_gamePort)   // Stage 2: 게임 서버 connect (포트 = 설정 파일 값)
            && m_bots[i].AssociateAndStart(m_iocp, i))                                   // Stage 2: IOCP 등록 + CS_GAME_AUTH 송신 (슬롯 i=위상/식별)
        {
            ++connected;   // 2단계 접속 성공 (인증 성공은 SC_ENTER_WORLD 수신 = s_authSuccess 가 별도 집계)
        }
        else
        {
            ++failed;
            m_bots[i].Close();
        }
        m_activeCount = i + 1;   // 봇 연결 즉시 active 갱신 - TimerProc가 이 봇부터 sweep해 위상 전진(시드 rand%167 stale=phase-lock 방지)

        if ((i - fromIndex + 1) % RAMP_BATCH == 0)
            ::Sleep(RAMP_INTERVAL_MS);
    }

    m_activeCount = end;   // 활성 끝 인덱스 (실패 슬롯도 점유 - 단순 스택 모델)
    // 인증 카운터는 비동기 - SC_ENTER_WORLD가 recv 워커로 곧 도착하므로 방금 연결한 봇은 아직 미반영(잠깐 뒤 's'로 재확인).
    ::wprintf(L"[StressTest] 연결(TCP): 성공 %d / 실패 %d  -> active=%d/%d  (인증: 성공 %lld / 거부 %lld - 방금 연결분은 곧 반영, 's'로 확인)\n",
        connected, failed, m_activeCount, m_maxBotCount,
        static_cast<long long>(DummySession::s_authSuccess),
        static_cast<long long>(DummySession::s_authFail));
}

unsigned __stdcall StressTestManager::WorkerProc(void* arg)
{
    StressTestManager* manager = reinterpret_cast<StressTestManager*>(arg);

    for (;;)
    {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = NULL;
        BOOL ok = ::GetQueuedCompletionStatus(manager->m_iocp, &transferred, &key, &ov, INFINITE);

        // 루프 종료: 종료 sentinel(Shutdown 이 key=0, ov=NULL 로 게시) 또는 완료 포트가 닫혀
        //   패킷 없이 실패한 경우(ov==NULL). 실제 완료는 key=this(0 아님) && ov!=NULL 이라
        //   여기서 종료되지 않고 아래로 진행한다(실제 완료를 오인 종료시키지 않음).
        if (key == 0 || ov == NULL)
            break;

        DummySession* bot = reinterpret_cast<DummySession*>(key);
        BotIoContext* ctx = CONTAINING_RECORD(ov, BotIoContext, m_overlapped);

        // ACTION = 타이머가 PQCS로 주입한 행동 트리거(transferred=0). 실제 I/O가 아니므로
        //   아래 transferred==0(원격 종료) 검사보다 *먼저* 처리한다(0을 종료로 오인해 Close 하는 것 방지).
        if (ctx->m_op == EBotOp::ACTION)
        {
            bot->OnActionTick();
            continue;
        }

        if (!ok || transferred == 0)
        {
            bot->Close();   // 실제 I/O(RECV/SEND/MOVE) 실패 또는 원격 종료
            continue;
        }

        switch (ctx->m_op)
        {
        case EBotOp::RECV:
            bot->OnRecvComplete(static_cast<int>(transferred));
            break;
        case EBotOp::SEND:
            bot->OnSendComplete(static_cast<int>(transferred));
            break;
        case EBotOp::ACTION_SEND:
            bot->OnActionSendComplete(static_cast<int>(transferred));
            break;
        default:
            break;
        }
    }

    return 0;
}

unsigned __stdcall StressTestManager::TimerProc(void* arg)
{
    StressTestManager* manager = reinterpret_cast<StressTestManager*>(arg);

    // 16ms마다 연결된 봇 전수 sweep -> 각 봇에 행동 틱 게시(PQCS). 봇당 스레드 0 -
    //   타이머 1개가 기존 워커 풀에 행동을 분배(봇당 in-flight ACTION 1개는 PostAction 가드가 보장).
    //   균일 주기 수천 봇엔 O(N) sweep으로 충분(PQCS는 값싼 커널 호출).
    UINT64 lastChurnMs = ::GetTickCount64();   // churn 은 기존 100ms 주기 유지(sweep 은 16ms 고해상도)
    while (manager->m_timerRunning)
    {
        ::Sleep(BOT_SWEEP_INTERVAL_MS);
        const UINT64 now = ::GetTickCount64();

        // action: 매 sweep 전 봇 PostAction(now) - 봇별 nextActionMs 게이트가 위상 분산(thundering herd 회피).
        //   시각 안 됐거나/INVALID 소켓/이미 대기 중이면 PostAction 내부서 skip.
        for (int i = 0; i < manager->m_activeCount; ++i)
        {
            manager->m_bots[i].PostAction(manager->m_iocp, now, manager->m_activeCount);   // 미연결/로그아웃 봇은 내부 m_sock INVALID 가드로 skip (activeCount=귓속말 대상 범위)
        }

        // churn: 기존 100ms 주기 유지(RECONNECT_LIFETIME_TICKS 의미 보존). 기본 OFF면 TickReconnect 즉시 return.
        if (now - lastChurnMs >= 100)
        {
            for (int i = 0; i < manager->m_activeCount; ++i)
            {
                manager->m_bots[i].TickReconnect(manager->m_iocp, manager->m_serverIp, manager->m_gamePort, manager->m_churnMode);   // 입력 구동 churn (재빌드 없이 토글, 슬롯 재사용)
            }
            lastChurnMs = now;
        }
    }
    return 0;
}

void StressTestManager::Run(int initialCount)
{
    RampConnect(0, initialCount);
    ::wprintf(L"[StressTest] 봇 가동 중. 명령: +N(추가) / -N(로그아웃) / s(상태) / q(종료)\n");

    wchar_t cmd[64] = { 0 };
    for (;;)
    {
        ::wprintf(L"명령> ");
        if (::fgetws(cmd, 64, stdin) == nullptr) { break; }
        if (cmd[0] == L'q' || cmd[0] == L'Q') { break; }
        else if (cmd[0] == L'+')
        {
            int n = 0;
            if (::swscanf_s(cmd + 1, L"%d", &n) == 1 && n > 0) { RampConnect(m_activeCount, n); }
        }
        else if (cmd[0] == L'-')
        {
            int n = 0;
            if (::swscanf_s(cmd + 1, L"%d", &n) == 1 && n > 0) { Logout(n); }
        }
        else if (cmd[0] == L's' || cmd[0] == L'S')
        {
            // 라이브 지표 조회 - 종료 시 리포트와 동일 포맷(PrintStats 공유). soak 중 오염(parseErr/content), 요청==응답(채널변경)을 실시간 확인.
            ::wprintf(L"[StressTest] ===== 라이브 지표 (active=%d) =====\n", m_activeCount);
            PrintStats();
        }
    }

    // 종료 시 최종 지표 리포트 (s 명령의 라이브 조회와 동일 포맷 - PrintStats 공유).
    ::wprintf(L"[StressTest] ===== 최종 지표 =====\n");
    PrintStats();
}

void StressTestManager::Logout(int count)
{
    int n = (count <= m_activeCount) ? count : m_activeCount;
    for (int i = m_activeCount - 1; i >= m_activeCount - n; --i)
    {
        m_bots[i].Close();   // closesocket -> 서버가 CLIENT_FIN 으로 슬롯 회수 (CS_LOGOUT 패킷 불요, 부하봇 표준)
    }
    m_activeCount -= n;
    ::wprintf(L"[StressTest] 로그아웃 %d  -> active=%d\n", n, m_activeCount);
}

// 누적 지표 스냅샷 - s(상태) 라이브 조회와 종료 최종 리포트가 공유(포맷 단일화 -> 두 뷰가 어긋날 수 없음).
//   오염 검사 = parseErr(framing 깨짐) + content 정합 이상(내용 오염). 요청==응답 = 채널변경 송신/회신 + 결과코드 분해.
void StressTestManager::PrintStats() const
{
    ::wprintf(L"[StressTest] 인증: 성공 %lld / 거부 %lld  (active=%d - 서버 CCU(auth)와 대조)\n",
        static_cast<long long>(DummySession::s_authSuccess),
        static_cast<long long>(DummySession::s_authFail),
        m_activeCount);
    ::wprintf(L"[StressTest] recv: 총 %lld / parseErr %lld  | mapChange %lld / moveBC %lld / spawn %lld / despawn %lld / serializeFail %lld\n",
        static_cast<long long>(DummySession::s_recvPackets),
        static_cast<long long>(DummySession::s_recvParseErrors),
        static_cast<long long>(DummySession::s_recvMapChange),
        static_cast<long long>(DummySession::s_recvMoveBroadcast),
        static_cast<long long>(DummySession::s_recvSpawn),
        static_cast<long long>(DummySession::s_recvDespawn), static_cast<long long>(DummySession::s_serializeFail));
    ::wprintf(L"[StressTest] recv: monsterSpawn %lld / monsterMove %lld / damage %lld / death %lld / respawn %lld / chat %lld / whisper %lld / whisperFail %lld\n",
        static_cast<long long>(DummySession::s_recvMonsterSpawns),
        static_cast<long long>(DummySession::s_recvMonsterMove),
        static_cast<long long>(DummySession::s_recvDamage),
        static_cast<long long>(DummySession::s_recvDeath),
        static_cast<long long>(DummySession::s_recvRespawn),
        static_cast<long long>(DummySession::s_recvChat),
        static_cast<long long>(DummySession::s_recvWhisper),
        static_cast<long long>(DummySession::s_recvWhisperFail));
    ::wprintf(L"[StressTest] sent: skill %lld / portal %lld / chat %lld / whisper %lld  | 포탈 맵이동 성공 %lld\n",
        static_cast<long long>(DummySession::s_sentSkill),
        static_cast<long long>(DummySession::s_sentPortal),
        static_cast<long long>(DummySession::s_sentChat),
        static_cast<long long>(DummySession::s_sentWhisper),
        static_cast<long long>(DummySession::s_mapChanges));
    ::wprintf(L"[StressTest] 채널변경: 송신 %lld / 회신 %lld  (OK %lld / SAME %lld / FULL %lld / INVALID %lld / NOT_IN_GAME %lld / RATE_LIMITED %lld)\n",
        static_cast<long long>(DummySession::s_sentChannelChange),
        static_cast<long long>(DummySession::s_recvChannelChange),
        static_cast<long long>(DummySession::s_ccOk),
        static_cast<long long>(DummySession::s_ccSame),
        static_cast<long long>(DummySession::s_ccFull),
        static_cast<long long>(DummySession::s_ccInvalid),
        static_cast<long long>(DummySession::s_ccNotInGame),
        static_cast<long long>(DummySession::s_ccRateLimited));
    ::wprintf(L"[StressTest] content 정합 이상(framing 정상·내용 오염): %lld  (0이어야 - 배치 교차오염/삼켜진 UAF 탐지)\n",
        static_cast<long long>(DummySession::s_recvContentErrors));
    ::wprintf(L"[StressTest] 아이템: 드랍 sighting %lld / 줍기 송신 %lld / 인벤 스냅샷 %lld  (줍기 송신 >= 인벤 스냅샷 근사, 뿌린 수 >= 주운 수)\n",
        static_cast<long long>(DummySession::s_recvGroundItemSpawn),
        static_cast<long long>(DummySession::s_sentPickup),
        static_cast<long long>(DummySession::s_recvInventory));

    // 검증 하니스 지표 (하니스 활성 시만 - 평시엔 노이즈 회피).
    if (DummySession::s_harness.hotspot || DummySession::s_harness.muteIdle || DummySession::s_harness.badCred
        || DummySession::s_harness.stopMove || DummySession::s_harness.bogusWhisper
        || DummySession::s_harness.skipChannel || DummySession::s_harness.rawInject
        || DummySession::s_harness.b3repro)
    {
        ::wprintf(L"[StressTest] 하니스: h1=%s h2=%s h3=%s h7=%s h9=%s h10=%s h12=%s\n",
            DummySession::s_harness.stopMove     ? L"ON" : L"off",
            DummySession::s_harness.hotspot      ? L"ON" : L"off",
            DummySession::s_harness.rawInject    ? L"ON" : L"off",
            DummySession::s_harness.bogusWhisper ? L"ON" : L"off",
            DummySession::s_harness.skipChannel  ? L"ON" : L"off",
            DummySession::s_harness.muteIdle     ? L"ON" : L"off",
            DummySession::s_harness.badCred      ? L"ON" : L"off");
        ::wprintf(L"[StressTest]   H1 divergence %lld (수정후 0) / H3 injected %lld / H9 skip attempt %lld -> admitted %lld (수정후 admitted 0·attempt>0 이라야 유효) | H12 badCredFail %lld / spuriousArm %lld (0이어야) / recvParseErr %lld (0이어야)\n",
            static_cast<long long>(DummySession::s_h1Divergence),
            static_cast<long long>(DummySession::s_h3Injected),
            static_cast<long long>(DummySession::s_h9SkipAttempted),
            static_cast<long long>(DummySession::s_h9SkipAdmitted),
            static_cast<long long>(DummySession::s_badCredFail),
            static_cast<long long>(DummySession::s_spuriousArm),
            static_cast<long long>(DummySession::s_recvParseErrors));
    }
}

void StressTestManager::Shutdown()
{
    // 0) 타이머 스레드 먼저 정지 (가드로 재호출 안전). 봇 배열에 PQCS 행동을 게시하는 주체를 먼저 없애야
    //    이후 워커 join -> 봇 Close/delete 중 회수 대상 봇에 PostAction(use-after-free) 이 안 일어난다.
    //    (타이머 정지 후 큐에 남은 ACTION/MOVE 완료는 워커가 drain 또는 sentinel 후 폐기 - 봇은 아직 유효.)
    if (m_timerThread != NULL)
    {
        m_timerRunning = false;
        ::WaitForSingleObject(m_timerThread, INFINITE);
        ::CloseHandle(m_timerThread);
        m_timerThread = NULL;
    }

    // 1) 워커 풀에 종료 sentinel(key=0) 게시 후 join. (가드로 재호출 안전.)
    //    워커를 먼저 모두 회수해 봇 배열을 참조하는 스레드를 없앤 뒤 소켓/배열을 정리한다 -
    //    그래서 봇 Close()/delete[] 가 워커와 동시에 일어나지 않는다(동시 Close/재사용 핸들 닫기 회피).
    if (m_workers != NULL)
    {
        if (m_iocp != NULL)
        {
            for (int i = 0; i < m_workerCount; ++i)
                ::PostQueuedCompletionStatus(m_iocp, 0, 0, NULL);
        }
        for (int i = 0; i < m_workerCount; ++i)
        {
            if (m_workers[i] != NULL)
            {
                ::WaitForSingleObject(m_workers[i], INFINITE);
                ::CloseHandle(m_workers[i]);
                m_workers[i] = NULL;
            }
        }
        delete[] m_workers;
        m_workers = NULL;
    }
    m_workerCount = 0;

    // 2) 봇 소켓 정리 + 배열 해제. 워커 join 이후라 봇을 만지는 스레드가 없다 -> closesocket/delete 안전.
    //    closesocket 이 남은 in-flight I/O 를 취소하고, 미수거 취소 완료 패킷은 아래 CloseHandle(iocp)
    //    에서 폐기된다. (취소 완료를 drain 한 뒤 해제하는 pending-IO 카운트 추적은 Stage 1 부하 도구
    //    범위 밖이라 생략 - 종료 시 한 번뿐인 무해한 윈도우.)
    if (m_bots != NULL)
    {
        for (int i = 0; i < m_maxBotCount; ++i)
            m_bots[i].Close();   // 전체 슬롯 정리 (미연결/로그아웃 봇 m_sock INVALID 는 Close no-op)
        delete[] m_bots;
        m_bots = NULL;
    }
    m_maxBotCount = 0;
    m_activeCount = 0;

    // 3) 완료 포트 + Winsock 회수 (iocp 기준 1회 - 이중 WSACleanup 방지).
    if (m_iocp != NULL)
    {
        ::CloseHandle(m_iocp);
        m_iocp = NULL;
        ::WSACleanup();
    }
}
