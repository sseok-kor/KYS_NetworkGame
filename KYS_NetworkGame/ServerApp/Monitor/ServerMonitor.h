#pragma once
#include <Windows.h>   // LONG / UINT64 / ULONGLONG / Interlocked* 타입
#include <vector>      // 채널별 직전값 (idle/phase% delta 계산 - 채널 수 동적)

namespace KYS { namespace GAMESERVER { namespace NETWORK { class IOCPServer; } } }   // Dump 인자(송신 압력 스냅샷 진입점) - 구현부만 IOCPServer.h 포함

namespace KYS
{
    namespace SERVERAPP
    {
        class Channel;          // 전방선언 - Dump 인자(포인터). 구현은 ServerMonitor.cpp서 Channel.h include.
        class ChannelManager;   // 전방선언 - Dump가 인구(채널별/맵별 플레이어) 집계에 사용.

        class ServerMonitor
        {
        public:
            static ServerMonitor& GetInstance() { static ServerMonitor instance; return instance; }   // Meyers static

            // 저빈도 hook (Interlocked, multi-writer 안전)
            // OnConnect/OnDisconnect 폐기: socket 카운트는 SessionPool 활성 슬롯 수에서 바로 셈(Dump 인자)으로 전환
            void OnLogin();                 // 누적 로그인 ++ (totalLogin, 줄지 않고 늘기만). CCU는 ChannelManager::GetAuthenticatedCount에서 계산
            void OnReaped(int count);       // 무음 소켓 회수 누적 += count (reaper 스레드가 호출 - m_totalLogin 과 같은 push 형)
            LONG GetReapedTotal() const { return m_reapedTotal; }   // 회수 누적 (표출/export 용 무락 read)
            LONG GetTotalLogin() const { return m_totalLogin; }     // 누적 로그인 (export 용 무락 read)

            // q 입력 시 main 스레드가 1회 호출 (동기 - 별도 모니터 스레드 없음)
            void Dump(Channel** channels, int channelCount, LONG ccu, int playerPoolUsed, int gsPoolUsed, int socketCount, KYS::GAMESERVER::NETWORK::IOCPServer* server, ChannelManager* channelManager);   // ccu=GetAuthenticatedCount() / socketCount=SessionPool 활성 슬롯 수 / 풀 사용량=누수 관측(근사) / server=송신 압력 스냅샷(drop/wsaPost/wrap 총량=retired+활성 + 상위 N 느린 세션) / channelManager=채널별/맵별 플레이어 집계 + 세션 라벨

            ServerMonitor(const ServerMonitor&) = delete;
            ServerMonitor& operator=(const ServerMonitor&) = delete;
            ServerMonitor(ServerMonitor&&) = delete;
            ServerMonitor& operator=(ServerMonitor&&) = delete;

        private:
            ServerMonitor();                   // 싱글턴 - GetInstance로만 생성
            ~ServerMonitor() = default;

            // 저빈도 카운터 (전역 Interlocked) - m_socketCount 폐기(socket은 SessionPool 활성 슬롯 수에서 셈)
            volatile LONG m_totalLogin;   // 누적 로그인 (줄지 않고 늘기만)
            volatile LONG m_reapedTotal;  // 무음 소켓 회수 누적 (reaper 스레드 Interlocked - flood 방어 관측)

            // 초당 변화량 계산용 직전값 (직전 q 스냅샷 - 메인만 접근, 락 불요)
            UINT64 m_prevTotalLogin;
            UINT64 m_prevPacketCount;
            UINT64 m_prevBytesRecv;
            UINT64 m_prevBytesSent;
            UINT64 m_prevTotalTickUs;
            UINT64 m_prevTickCount;
            UINT64 m_prevOverBudgetCount;   // 예산 초과 구간 변화량 계산용
            UINT64 m_prevWarnCount;         // 경고 구간 변화량 계산용
            UINT64 m_prevWsaSendCount;      // 진단: WSASend 게시 변화량 계산용 (coalescing 확인)
            UINT64 m_prevSendWrapCount;     // 진단: RingBuffer wrap 송신 변화량 계산용 (scatter-gather 실이익)

            // 자원 변화량 계산용 직전값 (CPU% / 실제 경과시간 - FILETIME 100ns 단위)
            ULONGLONG m_prevKernel100ns;
            ULONGLONG m_prevUser100ns;
            ULONGLONG m_prevWall100ns;
            bool      m_hasBaseline;        // 첫 q는 false -> 시작 이후 누적값 출력 후 직전값 세팅

            // 채널별 직전값 (idle/phase%를 최근 구간 delta로 계산 - 누적이면 시작 직후 한가했던 시간이 영구 혼입돼 idle이 0으로 안 내려감).
            //   인덱스 = 채널 번호. 첫 q(또는 채널 수 변동) 시 채널 수에 맞춰 resize.
            std::vector<UINT64> m_prevChSleepUs;
            std::vector<UINT64> m_prevChTotalTickUs;
            std::vector<UINT64> m_prevChPhaseRecvUs;
            std::vector<UINT64> m_prevChPhaseBroadcastUs;
            std::vector<UINT64> m_prevChPhaseSendUs;
            std::vector<UINT64> m_prevChPhaseUpdateUs;
            std::vector<UINT64> m_prevChOverBudget;   // 채널별 예산 초과 tick 수 (over% delta - idle과 짝)
            std::vector<UINT64> m_prevChTickCount;     // 채널별 tick 수 (over% 분모 delta)

            // 세션별 송신 압력 창평균용 슬롯-인덱스 prev (스냅샷마다 sid 대조 - 재사용 슬롯이면 baseline 리셋해 언더플로/오평균 차단). 풀 크기로 resize.
            std::vector<UINT64> m_prevSendSlotSid;         // 슬롯의 직전 스냅샷 sid (generation 포함 - 화신 교체 감지)
            std::vector<UINT64> m_prevSendSlotSampleSum;   // 슬롯의 직전 sampleSum (창평균 분자 delta)
            std::vector<UINT64> m_prevSendSlotSampleCount; // 슬롯의 직전 sampleCount (창평균 분모 delta)
        };
    }
}
