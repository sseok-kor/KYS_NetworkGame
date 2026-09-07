#pragma once
#include "../GameServer/Network/IOCP/Session.h"   // SessionSendMetrics / RetiredSendTotals (스냅샷 버퍼 타입)
#include <Windows.h>
#include <vector>

namespace KYS { namespace GAMESERVER { namespace NETWORK { class IOCPServer; } } }

namespace KYS
{
	namespace SERVERAPP
	{
		class Channel;
		class ChannelManager;

		// 지표 시계열 export - 전용 스레드가 10초마다 지표를 read-only 수집해 JSONL 파일에 1행씩 남긴다.
		//   'q' 콘솔 모니터(사람 눈용 순간값)와 독립: 자기 prev 만 쓰고 채널/세션 상태에 write 0 (관측자 효과 회피).
		//   튜닝 전후 비교/사후 분석("새벽에 무슨 일")이 목적 - 20초 tick 표본 창 밖의 역사를 디스크에 영속한다.
		//   로거 스레드와 분리(관측 수단끼리 운명 비공유) - 로거가 멈춰도 메트릭은 남고, 그 역도 성립.
		class MetricExporter
		{
		public:
			static const DWORD EXPORT_INTERVAL_MS = 10000;   // 시작값 10s (StatsD flush 기본과 동일) - 측정 후 조정 가능한 숫자

			MetricExporter();
			~MetricExporter();

			MetricExporter(const MetricExporter&) = delete;
			MetricExporter& operator=(const MetricExporter&) = delete;

			// Logs\<appName>_<pid>_metrics_YYYYMMDD_HHMMSS.jsonl 을 열고 수집 스레드를 시작한다.
			//   부팅 완료 후(채널/서버 기동 뒤) 호출 - 실패해도 서비스는 정상(관측만 없음).
			bool Start(const wchar_t* appName, Channel** channels, int channelCount,
				ChannelManager* channelManager, KYS::GAMESERVER::NETWORK::IOCPServer* server);

			// 마지막 스냅샷 1행을 쓰고 스레드를 정리한다. 채널 Stop 이전에 불러야 마지막 행이 살아있는 값을 읽는다.
			void Stop();

		private:
			static unsigned __stdcall ThreadEntry(void* self);
			void Loop();
			void WriteSnapshotLine(bool firstWindow);   // 수집(read-only) -> JSONL 1행 append
			void AppendUtf8Line(const wchar_t* line, int len);

			HANDLE m_thread;
			HANDLE m_stopEvent;    // 수동 리셋 - Stop 신호
			HANDLE m_file;

			Channel** m_channels;
			int m_channelCount;
			ChannelManager* m_channelManager;
			KYS::GAMESERVER::NETWORK::IOCPServer* m_server;

			// 수집/직렬화 버퍼 (수집 스레드 전용 - Start 에서 확보)
			std::vector<KYS::GAMESERVER::NETWORK::SessionSendMetrics> m_sendSnap;   // 전 슬롯 스냅샷 (활성 먼저 -> 은퇴 나중 순서 고정)
			wchar_t* m_line;
			char* m_utf8;

			// rate 계산용 직전값 (수집 스레드 전용 - 'q' 모니터 prev 와 완전 분리)
			UINT64 m_prevWallMs;          // 실측 wall 분모 (명목 10s 아님 - 지연/절전에도 왜곡 없는 rate)
			UINT64 m_prevPacketSum;
			UINT64 m_prevRecvBytesSum;
			UINT64 m_prevSentBytesSum;
			UINT64 m_prevWsaPostTotal;
			ULONGLONG m_prevKernel100ns;  // CPU(proc)% 분자 직전값
			ULONGLONG m_prevUser100ns;
			ULONGLONG m_prevWall100ns;
			std::vector<UINT64> m_prevChTickUs;      // 채널별 창 계산 (idle/over/phase%)
			std::vector<UINT64> m_prevChTickCount;
			std::vector<UINT64> m_prevChSleepUs;
			std::vector<UINT64> m_prevChOver;
			std::vector<UINT64> m_prevChPhaseRecv;
			std::vector<UINT64> m_prevChPhaseBcast;
			std::vector<UINT64> m_prevChPhaseSend;
			std::vector<UINT64> m_prevChPhaseUpdate;

			// 누적 필드 emit 측 단조 클램프 - 활성/은퇴 fold 레이스의 일시 감소가 시계열에 비단조로 남지 않게 (직전 emit 값 미만이면 유지)
			UINT64 m_lastDropTotal;
			UINT64 m_lastDropBytes;
			UINT64 m_lastWrapTotal;
			UINT64 m_lastWsaPostTotal;
			UINT64 m_lastWmHitsTotal;

			bool m_overflowWarned;   // 행 조립 절단 1회 경고 게이트 (반복 경고 스팸 방지)
		};
	}
}
