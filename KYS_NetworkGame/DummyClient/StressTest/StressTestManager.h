#pragma once
#include <WinSock2.h>
#include "DummySession.h"

// 다수 봇(DummySession)을 동적 1회 할당하고, 소수 워커가 GQCS 로 완료를 디스패치한다.
//   Stage 1 골격: IOCP async + ramp-up connect + 로그인/drain. (타이머/주기 행동은 Stage 2.)
class StressTestManager
{
public:
    StressTestManager();
    ~StressTestManager();

    StressTestManager(const StressTestManager&) = delete;
    StressTestManager& operator=(const StressTestManager&) = delete;
    StressTestManager(StressTestManager&&) = delete;
    StressTestManager& operator=(StressTestManager&&) = delete;

    bool Init(int maxBotCount, const wchar_t* serverIp, unsigned short gamePort);   // Winsock + IOCP + 워커 풀 + 봇 배열(maxBotCount 사전 할당, 런타임 추가 상한) + 게임 서버 포트 보관
    void Run(int initialCount);   // initialCount 먼저 연결 후 명령 루프 (+N 추가 / -N 로그아웃 / q 종료)
    void Shutdown();          // 워커 종료(sentinel) + 자원 회수 (idempotent)
    void SetTestMode(bool churnMode, bool dupLoginMode, int acctStart, int acctEnd);   // 입력 구동 모드 설정 (재빌드 없이 churn/중복로그인 토글, Init 후 Run 전 호출)

private:
    static unsigned __stdcall WorkerProc(void* arg);   // GQCS 루프 (RECV/SEND/ACTION/ACTION_SEND 디스패치)
    static unsigned __stdcall TimerProc(void* arg);    // 16ms마다 연결된 봇 sweep -> 위상 시각이 된 봇에만 PQCS 행동 게시 (Stage 2)
    void RampConnect(int fromIndex, int count);        // [fromIndex, fromIndex+count) 봇을 배치 단위 점진 연결
    void Logout(int count);                            // 끝 count 봇 closesocket (런타임 로그아웃 - 서버가 FIN 회수)
    void PrintStats() const;                           // 누적 지표 스냅샷 출력 - s(상태) 라이브 조회 + 종료 최종 리포트 공유

    HANDLE        m_iocp;
    HANDLE*       m_workers;
    int           m_workerCount;
    DummySession* m_bots;
    int           m_maxBotCount;    // 봇 배열 크기 (Init 고정, 런타임 추가 상한)
    volatile int  m_activeCount;    // 현재 연결 시도된 봇 수 (RampConnect/Logout 변동, main 갱신, timer read)
    wchar_t       m_serverIp[64];   // 서버 IP (runtime 입력, RampConnect 가 사용)
    unsigned short m_gamePort;      // 게임 서버 포트 (설정 파일에서 읽어 Init 인자로 들어온다, RampConnect/TimerProc 가 사용)
    HANDLE        m_timerThread;    // Stage 2: 주기 행동 타이머 (1개 - 봇당 스레드 금지)
    volatile bool m_timerRunning;   // 타이머 루프 종료 플래그

    // 입력 구동 테스트 모드 (재빌드 없이 main 입력으로 설정, SetTestMode).
    bool m_churnMode;       // 봇 수명 후 재연결 반복 (TimerProc->TickReconnect 게이트)
    bool m_dupLoginMode;    // 봇이 좁은 계정 범위를 공유 -> 중복 로그인 -> kick (kick 부하)
    int  m_acctStart;       // 공유 계정 인덱스 시작 (dupLoginMode 일 때만)
    int  m_acctEnd;         // 공유 계정 인덱스 끝 (포함)
};
