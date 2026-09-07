#include "pch_serverapp.h"
#include "ChannelWatchdog.h"
#include "Channel.h"
#include "../GameServer/Core/Log/Logger.h"   // 행 확정 fatal 파일 기록 (유한 락 - abort 도달 비결박)
#include <process.h>   // _beginthreadex
#include <stdlib.h>    // abort - SIGABRT로 CrashDump 덤프 경로 진입

namespace KYS
{
    namespace SERVERAPP
    {
        static const DWORD WATCHDOG_POLL_MS = 1000;   // 폴링 주기 1초

        volatile LONG64 ChannelWatchdog::s_maxStallMs = 0;

        ChannelWatchdog::ChannelWatchdog()
            : m_lastPollMs(0)
            , m_channelCount(0)
            , m_thread(NULL)
            , m_stopEvent(NULL)
        {
            for (int i = 0; i < MAX_WATCH_CHANNELS; ++i)
            {
                m_channels[i] = nullptr;
                m_lastTickCount[i] = 0;
                m_lastProgressMs[i] = 0;
            }
        }

        ChannelWatchdog::~ChannelWatchdog()
        {
            Stop();   // 멱등 - main이 명시 Stop을 놓쳐도 좀비 스레드/핸들 누수 방지
        }

        bool ChannelWatchdog::Start(Channel* const* channels, int channelCount)
        {
            if (channels == nullptr || channelCount <= 0 || channelCount > MAX_WATCH_CHANNELS)
            {
                return false;   // 감시 대상 없음/슬롯 초과 - 워치독 없이 계속 (호출측이 경고 로그)
            }
            if (m_thread != NULL)
            {
                return false;   // 이미 가동 중 (재기동은 Stop 후)
            }

            m_stopEvent = ::CreateEventW(NULL, TRUE, FALSE, NULL);   // manual-reset
            if (m_stopEvent == NULL)
            {
                return false;
            }

            // 기준점 초기화 - 지금부터의 무진행만 잰다 (부팅/로딩 구간은 감시 밖).
            const UINT64 nowMs = ::GetTickCount64();
            for (int i = 0; i < channelCount; ++i)
            {
                m_channels[i] = channels[i];
                m_lastTickCount[i] = channels[i]->GetTickCounter();
                m_lastProgressMs[i] = nowMs;
            }
            m_lastPollMs = nowMs;
            m_channelCount = channelCount;

            m_thread = reinterpret_cast<HANDLE>(
                _beginthreadex(NULL, 0, ThreadEntry, this, 0, NULL));
            if (m_thread == NULL)
            {
                ::CloseHandle(m_stopEvent);
                m_stopEvent = NULL;
                m_channelCount = 0;
                return false;
            }
            return true;
        }

        void ChannelWatchdog::Stop()
        {
            if (m_thread == NULL)
            {
                return;   // 미기동/이미 정지 - 멱등
            }
            ::SetEvent(m_stopEvent);                    // 폴링 대기를 즉시 깨움 (최대 1초 기다릴 필요 없이 종료)
            ::WaitForSingleObject(m_thread, INFINITE);  // 감시 스레드 join
            ::CloseHandle(m_thread);
            m_thread = NULL;
            ::CloseHandle(m_stopEvent);
            m_stopEvent = NULL;
        }

        UINT64 ChannelWatchdog::GetMaxStallMs()
        {
            return static_cast<UINT64>(s_maxStallMs);   // 단일 writer(워치독 스레드) - x64 정렬 8B read 원자 근사 (모니터 관례)
        }

        unsigned __stdcall ChannelWatchdog::ThreadEntry(void* arg)
        {
            ChannelWatchdog* watchdog = static_cast<ChannelWatchdog*>(arg);
            watchdog->Run();
            return 0;
        }

        // 1초마다 채널 tick 카운터를 스냅샷해 진행 여부만 본다 (값 자체는 무의미 - "변했는가"가 전부).
        //   행 판정 시 사유를 콘솔에 남기고 abort() - CrashDump의 SIGABRT 핸들러가 전 스레드 스택 덤프를 쓰고 종료한다.
        void ChannelWatchdog::Run()
        {
            for (;;)
            {
                if (::WaitForSingleObject(m_stopEvent, WATCHDOG_POLL_MS) == WAIT_OBJECT_0)
                {
                    return;   // 종료 신호 (정상 종료 경로)
                }

                const UINT64 nowMs = ::GetTickCount64();

                // 폴링 자체가 크게 건너뛰었다 = 워치독도 같이 멈췄었다 (디버거 전체 정지/절전/스냅샷 재개).
                //   채널 행은 독립 스레드인 워치독을 못 멈추므로, 이 경우는 프로세스 전체 정지지 채널 행이 아니다.
                //   판정하지 않고 기준점을 다시 잡는다 - 브레이크포인트 재개 직후 오탐 크래시 방지.
                if (nowMs - m_lastPollMs > static_cast<UINT64>(WATCHDOG_POLL_MS) * 5)
                {
                    for (int i = 0; i < m_channelCount; ++i)
                    {
                        m_lastProgressMs[i] = nowMs;
                    }
                    m_lastPollMs = nowMs;
                    continue;
                }
                m_lastPollMs = nowMs;

                for (int i = 0; i < m_channelCount; ++i)
                {
                    const UINT64 tickCount = m_channels[i]->GetTickCounter();   // cross-thread plain read (x64 정렬 8B 원자 근사)
                    if (tickCount != m_lastTickCount[i])
                    {
                        m_lastTickCount[i] = tickCount;   // 진행 관측 - 기준 시각 갱신
                        m_lastProgressMs[i] = nowMs;
                        continue;
                    }

                    const UINT64 stallMs = nowMs - m_lastProgressMs[i];
                    if (stallMs > static_cast<UINT64>(s_maxStallMs))
                    {
                        s_maxStallMs = static_cast<LONG64>(stallMs);   // 모니터 표시용 최대치 (단일 writer plain write)
                    }
                    if (stallMs >= STALL_LIMIT_MS)
                    {
                        // 행 확정 - 어느 채널이 얼마나 멈췄는지 먼저 기록.
                        //   (덤프의 폴트 스레드는 abort()를 부른 워치독으로 찍히므로 이 로그가 행 채널 특정의 1차 단서.
                        //    행 스레드의 실제 스택은 덤프의 전 스레드 스택에 들어 있다.)
                        ::wprintf(L"[watchdog][fatal] 채널 %d tick %llums 무진행 (임계 %llums) - 강제 크래시로 덤프 남기고 종료\n",
                            m_channels[i]->GetChannelId(), stallMs, STALL_LIMIT_MS);
                        ::fflush(stdout);   // abort()->TerminateProcess 는 CRT 버퍼를 안 비운다. 로그 캡처(리다이렉트) 시 위 fatal 줄 유실 방지 - 행 채널 특정 1차 단서 보존
                        // 파일에도 best-effort 로 남긴다 (FATAL = 유한 락 시도 동기 기록 - 실패해도 아래 abort 도달을 막지 않음).
                        //   콘솔이 닫혔거나 리다이렉트가 유실된 운영에서 사후에 행 채널을 파일로 특정하는 2차 단서.
                        KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                            KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_FATAL, L"watchdog",
                            L"채널 %d tick %llums 무진행 (임계 %llums) - 강제 크래시로 덤프 남기고 종료",
                            m_channels[i]->GetChannelId(), stallMs, STALL_LIMIT_MS);
                        abort();   // SIGABRT -> CrashDump 핸들러 -> 전 스레드 스택 .dmp/.txt -> 프로세스 종료 (exit 1)
                    }
                }
            }
        }
    }
}
