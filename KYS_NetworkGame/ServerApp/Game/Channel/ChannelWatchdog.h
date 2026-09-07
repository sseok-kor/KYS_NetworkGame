#pragma once
#include <Windows.h>

namespace KYS
{
    namespace SERVERAPP
    {
        class Channel;

        // 채널 행(hang) 감시 스레드 - 각 채널의 tick 카운터가 임계 시간 동안 안 변하면 강제 크래시로 전환한다.
        //   행은 프로세스 종료(exit)가 아니라서 외부 복구 수단(재시작 스크립트/서비스 관리자)이 볼 수 없다.
        //   크래시로 바꾸면 기존 CrashDump가 전 스레드 스택 덤프를 남기고 비정상 exit code로 종료된다.
        //   워치독은 관측만 한다 - 채널 상태에 개입하지 않는다 (1채널 1스레드 소유권 유지).
        class ChannelWatchdog
        {
        public:
            static const UINT64 STALL_LIMIT_MS = 60000;   // 행 판정 임계 - 60초 (채널 정상 tick 33ms 대비 1800배 여유)

            ChannelWatchdog();
            ~ChannelWatchdog();

            ChannelWatchdog(const ChannelWatchdog&) = delete;             // 스레드/이벤트 핸들 단일 소유 (이동은 자동 미선언)
            ChannelWatchdog& operator=(const ChannelWatchdog&) = delete;

            bool Start(Channel* const* channels, int channelCount);   // 부팅 완료 후 호출 - 로딩 구간을 감시 밖에 둬 오탐 원천 차단
            void Stop();                                              // 종료 시퀀스 맨 앞에서 호출 - 정리 중 tick 정지를 행으로 오판하지 않게

            static UINT64 GetMaxStallMs();   // 모니터 표시용 - 시작 이후 관측된 채널 최대 무진행 ms

        private:
            static unsigned __stdcall ThreadEntry(void* arg);
            void Run();   // 1초 주기 폴링 루프 (stopEvent 대기가 타이머 겸용)

            static const int MAX_WATCH_CHANNELS = 16;   // 감시 슬롯 상한 (CHANNEL_COUNT 여유분)

            Channel* m_channels[MAX_WATCH_CHANNELS];        // 감시 대상 (비소유 - main이 수명 관리)
            UINT64   m_lastTickCount[MAX_WATCH_CHANNELS];   // 마지막으로 관측한 채널 tick 카운터
            UINT64   m_lastProgressMs[MAX_WATCH_CHANNELS];  // 그 카운터가 마지막으로 변한 시각 (GetTickCount64 기준)
            UINT64   m_lastPollMs;                          // 직전 폴링 시각 - 폴링 자체가 크게 건너뛰면(디버거/절전) 판정 대신 기준점 재설정
            int      m_channelCount;
            HANDLE   m_thread;      // 감시 스레드 (_beginthreadex)
            HANDLE   m_stopEvent;   // manual-reset - Stop()이 SetEvent로 폴링 대기를 즉시 깨움

            static volatile LONG64 s_maxStallMs;   // 관측 최대 무진행 (워치독 스레드 단일 writer - 모니터가 plain read)
        };
    }
}
