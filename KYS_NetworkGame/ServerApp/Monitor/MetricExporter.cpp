#include "pch_serverapp.h"
#include "MetricExporter.h"
#include "ServerMonitor.h"                 // totalLogin / reaped (push 훅 누적)
#include "Game/Channel/Channel.h"          // ChannelStatsSnapshot + GetMonitorStats
#include "Game/Channel/ChannelManager.h"   // CCU / 인구 / 풀 사용량
#include "Game/Channel/ChannelWatchdog.h"  // 최대 무진행 관측치 (static)
#include "Game/Session/GameSession.h"              // GameSession (kickTotal 은 라이브러리 GetSendDropEvictTotal 로 옮겨 감)
#include "DataBase/MySqlDBProvider.h"      // DB 재접속/실패 카운터
#include "Network/LoginLinkThread.h"       // 링크 상태 + verify 분포
#include "Network/MainServer.h"                    // per-IP 거부 누적
#include "../GameServer/Core/Log/Logger.h" // 로거 자체 건강 (drop/절단) + 파일 폴더 규약 공유
#include "../GameServer/Network/IOCP/IOCPServer.h"
#include "../GameServer/Types/Defines.h"   // DEFAULT_SESSION_POOL_CAPACITY
#include "../GameCommon/GameDefines.h"     // MAX_MAP_COUNT
#include "../GameCommon/MapData/MapTable.h"        // MapTableCount (맵별 합산 - 쓰는 맵 수만큼)
#include <process.h>   // _beginthreadex
#include <psapi.h>     // GetProcessMemoryInfo (rss)

namespace KYS
{
	namespace SERVERAPP
	{
		namespace
		{
			const wchar_t METRIC_FOLDER[] = L"Logs";           // 로그 파일과 같은 폴더 (Logger 와 규약 공유)
			const int METRIC_LINE_MAX = 32768;                 // JSONL 1행 wchar 상한 (채널 6 x 이탈분포 12 포함 여유)

			ULONGLONG FileTimeToU64(const FILETIME& ft)
			{
				ULARGE_INTEGER u;
				u.LowPart = ft.dwLowDateTime;
				u.HighPart = ft.dwHighDateTime;
				return u.QuadPart;
			}

			// 누적 카운터의 창 delta - 활성/은퇴 fold 레이스로 순간 감소가 보여도 0 으로 (음수/언더플로 봉인)
			UINT64 DeltaOrZero(UINT64 now, UINT64 prev)
			{
				return (now >= prev) ? (now - prev) : 0;
			}
		}

		MetricExporter::MetricExporter()
			: m_thread(nullptr)
			, m_stopEvent(nullptr)
			, m_file(INVALID_HANDLE_VALUE)
			, m_channels(nullptr)
			, m_channelCount(0)
			, m_channelManager(nullptr)
			, m_server(nullptr)
			, m_line(nullptr)
			, m_utf8(nullptr)
			, m_prevWallMs(0)
			, m_prevPacketSum(0)
			, m_prevRecvBytesSum(0)
			, m_prevSentBytesSum(0)
			, m_prevWsaPostTotal(0)
			, m_prevKernel100ns(0)
			, m_prevUser100ns(0)
			, m_prevWall100ns(0)
			, m_lastDropTotal(0)
			, m_lastDropBytes(0)
			, m_lastWrapTotal(0)
			, m_lastWsaPostTotal(0)
			, m_lastWmHitsTotal(0)
			, m_overflowWarned(false)
		{
		}

		MetricExporter::~MetricExporter()
		{
			Stop();   // 명시 Stop 을 잊어도 안전 (이미 정리됐으면 no-op)
		}

		bool MetricExporter::Start(const wchar_t* appName, Channel** channels, int channelCount,
			ChannelManager* channelManager, KYS::GAMESERVER::NETWORK::IOCPServer* server)
		{
			if (m_thread != nullptr) { return true; }
			if (channels == nullptr || channelCount <= 0 || channelManager == nullptr || server == nullptr)
			{
				return false;
			}

			m_channels = channels;
			m_channelCount = channelCount;
			m_channelManager = channelManager;
			m_server = server;

			::CreateDirectoryW(METRIC_FOLDER, nullptr);   // 이미 있으면 무해 (Logger 와 같은 폴더)

			SYSTEMTIME now;
			::GetLocalTime(&now);
			wchar_t path[MAX_PATH];
			::_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s_%u_metrics_%04u%02u%02u_%02u%02u%02u.jsonl",
				METRIC_FOLDER, (appName != nullptr) ? appName : L"App", ::GetCurrentProcessId(),
				now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

			// CREATE_NEW - 동명 파일 무경고 truncate 방지 (실패 시 아래 강등 경로가 그대로 받는다).
			m_file = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
				CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (m_file == INVALID_HANDLE_VALUE)
			{
				::wprintf(L"[metric][warn] metrics 파일 열기 실패 - export 없이 가동 (서비스는 정상)\n");
				return false;
			}
			// BOM 없이 순수 UTF-8 - JSONL 소비 도구(jq 등)가 BOM 을 파싱 오류로 볼 수 있어 뺀다.

			m_sendSnap.resize(static_cast<size_t>(DEFAULT_SESSION_POOL_CAPACITY));
			m_line = new wchar_t[METRIC_LINE_MAX];
			m_utf8 = new char[METRIC_LINE_MAX * 3 + 2];
			m_prevChTickUs.assign(static_cast<size_t>(channelCount), 0);
			m_prevChTickCount.assign(static_cast<size_t>(channelCount), 0);
			m_prevChSleepUs.assign(static_cast<size_t>(channelCount), 0);
			m_prevChOver.assign(static_cast<size_t>(channelCount), 0);
			m_prevChPhaseRecv.assign(static_cast<size_t>(channelCount), 0);
			m_prevChPhaseBcast.assign(static_cast<size_t>(channelCount), 0);
			m_prevChPhaseSend.assign(static_cast<size_t>(channelCount), 0);
			m_prevChPhaseUpdate.assign(static_cast<size_t>(channelCount), 0);

			m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);   // 수동 리셋
			if (m_stopEvent == nullptr)
			{
				::CloseHandle(m_file);
				m_file = INVALID_HANDLE_VALUE;
				return false;
			}

			// rate 기준선 - 지금 시점 값을 prev 로 잡아 첫 행부터 온전한 10s 창이 되게 한다.
			WriteSnapshotLine(true);

			unsigned threadId = 0;
			m_thread = reinterpret_cast<HANDLE>(::_beginthreadex(nullptr, 0, &ThreadEntry, this, 0, &threadId));
			if (m_thread == nullptr)
			{
				::CloseHandle(m_stopEvent);
				m_stopEvent = nullptr;
				::CloseHandle(m_file);
				m_file = INVALID_HANDLE_VALUE;
				return false;
			}
			return true;
		}

		void MetricExporter::Stop()
		{
			if (m_thread != nullptr)
			{
				::SetEvent(m_stopEvent);
				::WaitForSingleObject(m_thread, INFINITE);
				::CloseHandle(m_thread);
				m_thread = nullptr;
			}
			if (m_stopEvent != nullptr)
			{
				::CloseHandle(m_stopEvent);
				m_stopEvent = nullptr;
			}
			if (m_file != INVALID_HANDLE_VALUE)
			{
				::FlushFileBuffers(m_file);
				::CloseHandle(m_file);
				m_file = INVALID_HANDLE_VALUE;
			}
			delete[] m_line;  m_line = nullptr;
			delete[] m_utf8;  m_utf8 = nullptr;
		}

		unsigned __stdcall MetricExporter::ThreadEntry(void* self)
		{
			static_cast<MetricExporter*>(self)->Loop();
			return 0;
		}

		void MetricExporter::Loop()
		{
			for (;;)
			{
				const bool stopping = (::WaitForSingleObject(m_stopEvent, EXPORT_INTERVAL_MS) == WAIT_OBJECT_0);
				WriteSnapshotLine(false);   // stop 신호여도 마지막 1행을 남기고 나간다 (종료 직전 상태 보존)
				if (stopping) { break; }
			}
		}

		// 지표를 read-only 로 모아 JSONL 1행을 만든다. 첫 호출(firstWindow)은 rate 기준선만 잡고 파일에는 쓰지 않는다.
		void MetricExporter::WriteSnapshotLine(bool firstWindow)
		{
			// ---- 수집 (전부 read-only - 채널/세션 상태에 write 0) ----
			const UINT64 nowWallMs = ::GetTickCount64();
			const UINT64 wallDeltaMs = DeltaOrZero(nowWallMs, m_prevWallMs);
			const double wallSec = (wallDeltaMs > 0) ? (static_cast<double>(wallDeltaMs) / 1000.0) : 1.0;

			// 채널 스냅샷 (채널당 1회)
			std::vector<ChannelStatsSnapshot> chSnap(static_cast<size_t>(m_channelCount));
			UINT64 packetSum = 0, recvSum = 0, sentSum = 0;
			for (int i = 0; i < m_channelCount; ++i)
			{
				chSnap[static_cast<size_t>(i)] = m_channels[i]->GetMonitorStats();
				packetSum += chSnap[static_cast<size_t>(i)].packetCount;
				recvSum += chSnap[static_cast<size_t>(i)].bytesRecv;
				sentSum += chSnap[static_cast<size_t>(i)].bytesSent;
			}

			// 송신 압력 - 활성 슬롯 먼저, 은퇴 fold 나중 (모니터와 같은 읽기 순서 - 이중계상 spike 쪽으로 기울여 단조 유지)
			const int slotCount = m_server->SnapshotSendMetrics(m_sendSnap.data(), static_cast<int>(m_sendSnap.size()));
			UINT64 dropTotal = 0, dropBytes = 0, wrapTotal = 0, wsaPostTotal = 0, wmHitsTotal = 0;
			int peakPctMax = 0;
			for (int i = 0; i < slotCount; ++i)
			{
				const KYS::GAMESERVER::NETWORK::SessionSendMetrics& m = m_sendSnap[static_cast<size_t>(i)];
				dropTotal += m.dropCount;
				dropBytes += m.dropBytes;
				wrapTotal += m.wrapCount;
				wsaPostTotal += m.wsaPostCount;
				if (m.sid != 0)
				{
					wmHitsTotal += m.watermarkHits;
					if (m.capacity > 0)
					{
						const int pct = static_cast<int>((static_cast<UINT64>(m.peakUsed) * 100) / m.capacity);
						if (pct > peakPctMax) { peakPctMax = pct; }
					}
				}
			}
			KYS::GAMESERVER::NETWORK::RetiredSendTotals retired;
			m_server->GetRetiredSendTotals(retired);
			dropTotal += retired.dropCount;
			dropBytes += retired.dropBytes;
			wrapTotal += retired.wrapCount;
			wsaPostTotal += retired.wsaPostCount;
			wmHitsTotal += retired.watermarkHits;   // 이탈 세션 분 접힘 가산 - Total 이름값(단조 누적) 유지

			// 누적 필드 emit 측 단조 클램프 (fold 레이스의 일시 감소가 시계열에 남지 않게)
			if (dropTotal < m_lastDropTotal) { dropTotal = m_lastDropTotal; }        m_lastDropTotal = dropTotal;
			if (dropBytes < m_lastDropBytes) { dropBytes = m_lastDropBytes; }        m_lastDropBytes = dropBytes;
			if (wrapTotal < m_lastWrapTotal) { wrapTotal = m_lastWrapTotal; }        m_lastWrapTotal = wrapTotal;
			if (wsaPostTotal < m_lastWsaPostTotal) { wsaPostTotal = m_lastWsaPostTotal; }  m_lastWsaPostTotal = wsaPostTotal;
			if (wmHitsTotal < m_lastWmHitsTotal) { wmHitsTotal = m_lastWmHitsTotal; }      m_lastWmHitsTotal = wmHitsTotal;

			// 인구 (read 락 1회 순회)
			std::vector<int> perChannel(static_cast<size_t>(m_channelCount), 0);
			std::vector<int> perChannelMap(static_cast<size_t>(m_channelCount) * MAX_MAP_COUNT, 0);   // 할당 = 인덱싱(CollectPopulation)과 같은 상한 상수
			m_channelManager->CollectPopulation(m_channelCount, perChannel.data(), perChannelMap.data());

			// CPU(proc)% / rss
			FILETIME creation, exit, kernel, user, wallFt;
			::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user);
			::GetSystemTimeAsFileTime(&wallFt);
			const ULONGLONG k = FileTimeToU64(kernel), u = FileTimeToU64(user), w = FileTimeToU64(wallFt);
			double cpuPct = 0.0;
			if (m_prevWall100ns != 0 && w > m_prevWall100ns)
			{
				cpuPct = static_cast<double>((k - m_prevKernel100ns) + (u - m_prevUser100ns)) * 100.0
					/ static_cast<double>(w - m_prevWall100ns);
			}
			PROCESS_MEMORY_COUNTERS pmc = {};
			pmc.cb = sizeof(pmc);
			::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc));
			const UINT64 rssMB = static_cast<UINT64>(pmc.WorkingSetSize) / (1024 * 1024);

			// ---- 직렬화 + prev 갱신 ----
			if (firstWindow == false && m_file != INVALID_HANDLE_VALUE && m_line != nullptr)
			{
				SYSTEMTIME ts;
				::GetLocalTime(&ts);

				int n = 0;
				bool lineOk = true;   // 조립 중 절단(-1) 발생 시 행 전체를 포기 (반쪽 JSON 을 기록하지 않는다)
				int part = 0;
				// 필드 유형 규약: rate(초당) = 실측 wall 분모 / total = 프로세스 수명 누적(단조) / 게이지 = 순간값 / pct = 이번 창 비율
				part = ::_snwprintf_s(m_line + n, METRIC_LINE_MAX - n, _TRUNCATE,
					L"{\"ts\":\"%04u-%02u-%02u %02u:%02u:%02u\",\"windowMs\":%llu,"
					L"\"ccu\":%d,\"sockets\":%d,\"playerPool\":%d,\"gsPool\":%d,\"loginTotal\":%ld,"
					L"\"pps\":%.1f,\"recvKBs\":%.1f,\"sentKBs\":%.1f,\"wsaSendS\":%.1f,",
					ts.wYear, ts.wMonth, ts.wDay, ts.wHour, ts.wMinute, ts.wSecond, wallDeltaMs,
					m_channelManager->GetAuthenticatedCount(),
					m_server->GetSessionPoolUsedCount(),
					m_channelManager->GetPlayerPoolUsed(), m_channelManager->GetGameSessionPoolUsed(),
					ServerMonitor::GetInstance().GetTotalLogin(),
					static_cast<double>(DeltaOrZero(packetSum, m_prevPacketSum)) / wallSec,
					static_cast<double>(DeltaOrZero(recvSum, m_prevRecvBytesSum)) / 1024.0 / wallSec,
					static_cast<double>(DeltaOrZero(sentSum, m_prevSentBytesSum)) / 1024.0 / wallSec,
					static_cast<double>(DeltaOrZero(wsaPostTotal, m_prevWsaPostTotal)) / wallSec);
				if (part < 0) { lineOk = false; } else { n += part; }

				if (lineOk)
				{
					part = ::_snwprintf_s(m_line + n, METRIC_LINE_MAX - n, _TRUNCATE,
						L"\"send\":{\"dropTotal\":%llu,\"dropBytes\":%llu,\"wrapTotal\":%llu,\"wsaPostTotal\":%llu,"
						L"\"kickTotal\":%llu,\"peakPctMax\":%d,\"wmHitsTotal\":%llu},"
						L"\"log\":{\"drop\":%llu,\"truncated\":%llu,\"writeFail\":%llu},"
						L"\"db\":{\"reconnect\":%u,\"qfail\":%u},"
						L"\"watchdogMaxStallMs\":%llu,"
						L"\"link\":{\"up\":%d,\"verifyOk\":%ld,\"notFound\":%ld,\"alreadyOnline\":%ld,\"error\":%ld},"
						L"\"flood\":{\"ipReject\":%llu,\"reaped\":%ld},"
						L"\"cpuProc\":%.1f,\"rssMB\":%llu,\"ch\":[",
						dropTotal, dropBytes, wrapTotal, wsaPostTotal,
						(m_server != nullptr ? m_server->GetSendDropEvictTotal() : 0ull), peakPctMax, wmHitsTotal,
						KYS::GAMESERVER::LOG::Logger::GetInstance().GetDropCount(),
						KYS::GAMESERVER::LOG::Logger::GetInstance().GetTruncatedCount(),
						KYS::GAMESERVER::LOG::Logger::GetInstance().GetWriteFailCount(),
						MySqlDBProvider::GetReconnectCount(), MySqlDBProvider::GetQueryFailCount(),
						ChannelWatchdog::GetMaxStallMs(),
						LoginLinkThread::GetInstance().IsLinkUp() ? 1 : 0,
						LoginLinkThread::s_verifyOk, LoginLinkThread::s_verifyNotFound,
						LoginLinkThread::s_verifyAlreadyOnline, LoginLinkThread::s_verifyError,
						MainServer::GetInstance().GetIpRejectCount(), ServerMonitor::GetInstance().GetReapedTotal(),
						cpuPct, rssMB);
					if (part < 0) { lineOk = false; } else { n += part; }
				}

				for (int i = 0; lineOk && i < m_channelCount; ++i)
				{
					const ChannelStatsSnapshot& s = chSnap[static_cast<size_t>(i)];
					const size_t ci = static_cast<size_t>(i);

					int monsters = 0;
					for (int mp = 0; mp < MapTableCount(); ++mp) { monsters += s.monsterPerMap[mp]; }   // 쓰는 맵 수만큼 합산

					// 이번 창(10s) 채널 delta - idle/over/phase% 는 창 기준 (누적이면 부팅 초기 한가함이 영구 혼입)
					const UINT64 dTick = DeltaOrZero(s.totalTickUs, m_prevChTickUs[ci]);
					const UINT64 dTickCount = DeltaOrZero(s.tickCount, m_prevChTickCount[ci]);
					const UINT64 dSleep = DeltaOrZero(s.sleepUs, m_prevChSleepUs[ci]);
					const UINT64 dOver = DeltaOrZero(s.overBudgetCount, m_prevChOver[ci]);
					const UINT64 dRecvP = DeltaOrZero(s.phaseRecvUs, m_prevChPhaseRecv[ci]);
					const UINT64 dBcastP = DeltaOrZero(s.phaseBroadcastUs, m_prevChPhaseBcast[ci]);
					const UINT64 dSendP = DeltaOrZero(s.phaseSendUs, m_prevChPhaseSend[ci]);
					const UINT64 dUpdateP = DeltaOrZero(s.phaseUpdateUs, m_prevChPhaseUpdate[ci]);

					const double idlePct = (dSleep + dTick > 0)
						? (static_cast<double>(dSleep) * 100.0 / static_cast<double>(dSleep + dTick)) : 0.0;
					const double overPct = (dTickCount > 0)
						? (static_cast<double>(dOver) * 100.0 / static_cast<double>(dTickCount)) : 0.0;
					const double avgTickUs = (dTickCount > 0)
						? (static_cast<double>(dTick) / static_cast<double>(dTickCount)) : 0.0;
					// phase.send 는 bcast 의 부분집합 - 네 값의 합이 100% 를 넘을 수 있다 (중복 아님, 포함 관계)
					const double phRecv = (dTick > 0) ? (static_cast<double>(dRecvP) * 100.0 / static_cast<double>(dTick)) : 0.0;
					const double phBcast = (dTick > 0) ? (static_cast<double>(dBcastP) * 100.0 / static_cast<double>(dTick)) : 0.0;
					const double phSend = (dTick > 0) ? (static_cast<double>(dSendP) * 100.0 / static_cast<double>(dTick)) : 0.0;
					const double phUpdate = (dTick > 0) ? (static_cast<double>(dUpdateP) * 100.0 / static_cast<double>(dTick)) : 0.0;

					part = ::_snwprintf_s(m_line + n, METRIC_LINE_MAX - n, _TRUNCATE,
						L"%s{\"id\":%d,\"players\":%d,\"monsters\":%d,"
						L"\"tickAvgUs\":%.0f,\"p50\":%llu,\"p95\":%llu,\"p99\":%llu,\"wMax\":%llu,"
						L"\"idlePct\":%.1f,\"overPct\":%.1f,"
						L"\"phase\":{\"recv\":%.1f,\"bcast\":%.1f,\"send\":%.1f,\"update\":%.1f},"
						L"\"moveDrop\":%llu,\"critDrop\":%llu,\"jobsP50\":%llu,\"jobsP95\":%llu,"
						L"\"leave\":[%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld]}",
						(i > 0) ? L"," : L"",
						i, perChannel[ci], monsters,
						avgTickUs, s.p50Us, s.p95Us, s.p99Us, s.windowMaxUs,
						idlePct, overPct,
						phRecv, phBcast, phSend, phUpdate,
						s.moveDropped, s.criticalDropped, s.jobP50, s.jobP95,
						s.leaveReasonCount[0], s.leaveReasonCount[1], s.leaveReasonCount[2], s.leaveReasonCount[3],
						s.leaveReasonCount[4], s.leaveReasonCount[5], s.leaveReasonCount[6], s.leaveReasonCount[7],
						s.leaveReasonCount[8], s.leaveReasonCount[9], s.leaveReasonCount[10], s.leaveReasonCount[11]);
					if (part < 0) { lineOk = false; } else { n += part; }
				}

				if (lineOk)
				{
					part = ::_snwprintf_s(m_line + n, METRIC_LINE_MAX - n, _TRUNCATE, L"]}");
					if (part < 0) { lineOk = false; } else { n += part; }
				}

				if (lineOk && n > 0)
				{
					AppendUtf8Line(m_line, n);
				}
				else if (m_overflowWarned == false)
				{
					// 조립 절단 - 반쪽 JSON 대신 행을 통째 생략하고 1회만 알린다 (버퍼 상한 산정 재검토 신호).
					m_overflowWarned = true;
					KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
						KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_WARN, L"metric",
						L"JSONL 행 조립 절단 - 해당 스냅샷 기록 생략 (버퍼 상한 %d wchar)", METRIC_LINE_MAX);
				}
			}

			// prev 갱신 (첫 호출 = rate 기준선)
			m_prevWallMs = nowWallMs;
			m_prevPacketSum = packetSum;
			m_prevRecvBytesSum = recvSum;
			m_prevSentBytesSum = sentSum;
			m_prevWsaPostTotal = wsaPostTotal;
			m_prevKernel100ns = k;
			m_prevUser100ns = u;
			m_prevWall100ns = w;
			for (int i = 0; i < m_channelCount; ++i)
			{
				const ChannelStatsSnapshot& s = chSnap[static_cast<size_t>(i)];
				const size_t ci = static_cast<size_t>(i);
				m_prevChTickUs[ci] = s.totalTickUs;
				m_prevChTickCount[ci] = s.tickCount;
				m_prevChSleepUs[ci] = s.sleepUs;
				m_prevChOver[ci] = s.overBudgetCount;
				m_prevChPhaseRecv[ci] = s.phaseRecvUs;
				m_prevChPhaseBcast[ci] = s.phaseBroadcastUs;
				m_prevChPhaseSend[ci] = s.phaseSendUs;
				m_prevChPhaseUpdate[ci] = s.phaseUpdateUs;
			}
		}

		void MetricExporter::AppendUtf8Line(const wchar_t* line, int len)
		{
			const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line, len, m_utf8, METRIC_LINE_MAX * 3, nullptr, nullptr);
			if (bytes <= 0) { return; }
			m_utf8[bytes] = '\r';
			m_utf8[bytes + 1] = '\n';
			DWORD written = 0;
			::WriteFile(m_file, m_utf8, static_cast<DWORD>(bytes) + 2, &written, nullptr);
		}
	}
}
