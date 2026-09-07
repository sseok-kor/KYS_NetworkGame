#include "pch_serverapp.h"
#include "ServerMonitor.h"
#include "Game/Channel/Channel.h"          // ChannelStatsSnapshot + Channel::GetMonitorStats
#include "Game/Channel/ChannelManager.h"   // CollectPopulation (채널별/맵별 플레이어 집계) + GetSession (세션 라벨)
#include "Game/Session/GameSession.h"              // GameSession(accountId/channelId/midFlush) 라벨
#include "DataBase/MySqlDBProvider.h"      // DB 재접속/실패 카운터 (static 읽기)
#include "Game/Channel/ChannelWatchdog.h"  // 워치독 최대 무진행 관측치 (static 읽기)
#include "Network/LoginLinkThread.h"       // 인터서버 링크 상태 + verify 결과 분포 (write-only 지표 소생)
#include "Network/MainServer.h"                    // per-IP 연결 거부 누적 (flood 방어 관측)
#include "../GameServer/Core/Log/Logger.h" // 로거 자체 건강 지표 (drop/절단)
#include "../GameServer/Network/IOCP/IOCPServer.h"   // SnapshotSendMetrics / GetRetiredSendTotals (송신 압력 스냅샷)
#include "../GameServer/Types/Defines.h"             // DEFAULT_SESSION_POOL_CAPACITY (스냅샷 버퍼 크기)
#include "../GameCommon/MapData/MapTable.h"                  // MapTableCount (맵별 표시 - 쓰는 맵 수만큼)
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")   // GetProcessMemoryInfo (vcxproj 무수정 링크)
#include <vector>
#include <algorithm>   // std::sort (상위 N 느린 세션 정렬)
#include <cwchar>      // swprintf_s / wcscpy_s (상위 N 표 라벨 포맷)

namespace KYS
{
    namespace SERVERAPP
    {
        // FILETIME(100ns) -> 64bit 누적값
        static ULONGLONG FileTimeToU64(const FILETIME& ft)
        {
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            return u.QuadPart;
        }

        ServerMonitor::ServerMonitor()
            : m_totalLogin(0)
            , m_reapedTotal(0)
            , m_prevTotalLogin(0)
            , m_prevPacketCount(0)
            , m_prevBytesRecv(0)
            , m_prevBytesSent(0)
            , m_prevTotalTickUs(0)
            , m_prevTickCount(0)
            , m_prevOverBudgetCount(0)
            , m_prevWarnCount(0)
            , m_prevWsaSendCount(0)
            , m_prevSendWrapCount(0)
            , m_prevKernel100ns(0)
            , m_prevUser100ns(0)
            , m_prevWall100ns(0)
            , m_hasBaseline(false)
        {
        }

        // 저빈도 hook (multi-writer -> Interlocked)
        // OnConnect/OnDisconnect 폐기: socket 카운트는 SessionPool 활성 슬롯 수(집합에서 바로 셈)로 전환 - 별도 카운터가 어긋나거나 음수 되는 문제 제거
        void ServerMonitor::OnLogin() { InterlockedIncrement(&m_totalLogin); }   // totalLogin만 (CCU는 GetAuthenticatedCount에서 계산)

        void ServerMonitor::OnReaped(int count) { InterlockedAdd(&m_reapedTotal, static_cast<LONG>(count)); }   // 무음 소켓 회수 누적 (reaper 스레드)

        void ServerMonitor::Dump(Channel** channels, int channelCount, LONG ccu, int playerPoolUsed, int gsPoolUsed, int socketCount, KYS::GAMESERVER::NETWORK::IOCPServer* server, ChannelManager* channelManager)
        {
            // (a) 고빈도 Channel 카운터 합산 (단순 읽기 - 다른 스레드 값이라 근사)
            UINT64 packetCount = 0, bytesRecv = 0, bytesSent = 0;
            UINT64 totalTickUs = 0, maxTickUs = 0, tickCount = 0;
            UINT64 overBudget = 0, warnCount = 0;
            for (int i = 0; i < channelCount; ++i)
            {
                const ChannelStatsSnapshot s = channels[i]->GetMonitorStats();
                packetCount += s.packetCount;
                bytesRecv += s.bytesRecv;
                bytesSent += s.bytesSent;
                totalTickUs += s.totalTickUs;
                tickCount += s.tickCount;
                overBudget += s.overBudgetCount;
                warnCount += s.warnCount;
                if (s.maxTickUs > maxTickUs) { maxTickUs = s.maxTickUs; }
            }

            // (b) 저빈도 카운터 - ccu/sockets 모두 인자(집합에서 셈): ccu=인증세션 map.size, sockets=SessionPool 활성 슬롯
            const LONG   sockets = socketCount;
            const UINT64 totalLogin = static_cast<UINT64>(m_totalLogin);

            // (a-2) 세션별 송신 압력 스냅샷 (라이브러리 raw per-slot) - 총량/상위 N/창평균 공용, 무락 근사(1Hz pull).
            //   총량 = 은퇴 누적(retired) + 활성 슬롯 합 => 세션 이탈로 카운터가 0화 증발해도 총량이 단조 복원(delta 언더플로 봉인).
            //   읽기 순서 = 활성 먼저 -> 은퇴 나중: Release 의 fold 레이스가 겹쳐도 같은 세션이 양쪽에 잠깐 이중 계상(spike) 쪽으로만 기울고, 그 뒤 감소분은 아래 wsaSend/s delta 가드가 0 처리한다(언더플로 봉인 주체 = 그 가드).
            std::vector<KYS::GAMESERVER::NETWORK::SessionSendMetrics> sendSnap(static_cast<size_t>(DEFAULT_SESSION_POOL_CAPACITY));
            int sendSlotCount = 0;
            if (server != nullptr)
            {
                sendSlotCount = server->SnapshotSendMetrics(sendSnap.data(), static_cast<int>(sendSnap.size()));
            }

            // 슬롯-인덱스 prev 준비(첫 q 또는 풀 크기 변동 시 0으로). 0 이면 첫 창평균은 '-'(baseline 없음).
            if (static_cast<int>(m_prevSendSlotSid.size()) != sendSlotCount)
            {
                m_prevSendSlotSid.assign(sendSlotCount, 0);
                m_prevSendSlotSampleSum.assign(sendSlotCount, 0);
                m_prevSendSlotSampleCount.assign(sendSlotCount, 0);
            }

            // 활성 슬롯 합 + 창평균(옛 prev 기준) + prev 갱신을 한 번에. 창평균 = -1.0 이면 '-'(이번 창 표본 0 / 화신 교체).
            std::vector<double> slotWindowAvg(static_cast<size_t>(sendSlotCount), -1.0);
            UINT64 activeDrop = 0, activeWsaPost = 0, activeWrap = 0;
            for (int i = 0; i < sendSlotCount; ++i)
            {
                const KYS::GAMESERVER::NETWORK::SessionSendMetrics& m = sendSnap[static_cast<size_t>(i)];
                activeDrop    += m.dropCount;
                activeWsaPost += m.wsaPostCount;
                activeWrap    += m.wrapCount;

                // 창평균 = (sampleSum - prev)/(sampleCount - prev). 같은 화신(sid 일치)일 때만 - 재사용/자유 슬롯(sid 불일치/0)은 baseline 리셋.
                //   0-나눗셈 가드 = deltaCount==0 (이번 창 미송신). 같은 화신이면 sampleCount 는 감소 안 하므로 delta 언더플로 없음.
                if (m.sid != 0 && m.sid == m_prevSendSlotSid[static_cast<size_t>(i)])
                {
                    const UINT64 dCount = m.sampleCount - m_prevSendSlotSampleCount[static_cast<size_t>(i)];
                    if (dCount != 0)
                    {
                        const UINT64 dSum = m.sampleSum - m_prevSendSlotSampleSum[static_cast<size_t>(i)];
                        slotWindowAvg[static_cast<size_t>(i)] = static_cast<double>(dSum) / static_cast<double>(dCount);
                    }
                }
                m_prevSendSlotSid[static_cast<size_t>(i)]         = m.sid;
                m_prevSendSlotSampleSum[static_cast<size_t>(i)]   = m.sampleSum;
                m_prevSendSlotSampleCount[static_cast<size_t>(i)] = m.sampleCount;
            }

            KYS::GAMESERVER::NETWORK::RetiredSendTotals retired = { 0, 0, 0, 0 };
            if (server != nullptr) { server->GetRetiredSendTotals(retired); }   // 활성 읽은 뒤 은퇴 읽기(단조 방향)

            const UINT64 sendDropTotal = retired.dropCount + activeDrop;        // 단조 복원 총량 (은퇴+활성 합 - 세션 이탈로 0화 증발해도 감소 안 함)
            const UINT64 wsaPostTotal  = retired.wsaPostCount + activeWsaPost;
            const UINT64 wrapTotal     = retired.wrapCount + activeWrap;

            // (c) 자원 스냅샷
            FILETIME ct, et, kt, ut;
            GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut);
            const ULONGLONG kernel100ns = FileTimeToU64(kt);
            const ULONGLONG user100ns = FileTimeToU64(ut);

            FILETIME nowFt;
            GetSystemTimeAsFileTime(&nowFt);
            const ULONGLONG wall100ns = FileTimeToU64(nowFt);

            PROCESS_MEMORY_COUNTERS pmc;
            pmc.cb = static_cast<DWORD>(sizeof(pmc));
            GetProcessMemoryInfo(GetCurrentProcess(), &pmc, static_cast<DWORD>(sizeof(pmc)));
            const UINT64 rssKB = static_cast<UINT64>(pmc.WorkingSetSize / 1024);       // RSS = 물리 RAM 점유(WorkingSetSize) - 업계 표준어
            const UINT64 peakRssKB = static_cast<UINT64>(pmc.PeakWorkingSetSize / 1024);

            SYSTEM_INFO si;
            GetSystemInfo(&si);
            const DWORD cores = si.dwNumberOfProcessors;

            // (d) 출력
            if (!m_hasBaseline)
            {
                // 첫 q - 시작 이후 누적 (생성 시각 ct 기준 CPU%)
                const ULONGLONG createWall = FileTimeToU64(ct);
                const ULONGLONG wallDelta = (wall100ns > createWall) ? (wall100ns - createWall) : 0;
                double cpuPct = 0.0;
                if (wallDelta > 0 && cores > 0)
                {
                    cpuPct = static_cast<double>(kernel100ns + user100ns)
                        / static_cast<double>(wallDelta)
                        / static_cast<double>(cores) * 100.0;
                }
                const UINT64 avgTickUs = (tickCount > 0) ? (totalTickUs / tickCount) : 0;

                wprintf(L"===== SERVER MONITOR (since start) =====\n");
                wprintf(L" CCU(auth)=%ld  sockets=%ld  totalLogin=%llu  playerPool=%d  gsPool=%d\n", ccu, sockets, totalLogin, playerPoolUsed, gsPoolUsed);
                wprintf(L" packets=%llu  recvBytes=%llu  sentBytes=%llu  (cumulative)\n",
                    packetCount, bytesRecv, bytesSent);
                wprintf(L" tick avg=%lluus  max=%lluus  over(>=33333)=%llu  warn(>=26666)=%llu  ticks=%llu\n",
                    avgTickUs, maxTickUs, overBudget, warnCount, tickCount);
                wprintf(L" CPU(proc)=%.1f%%  rss=%lluKB  peakRss=%lluKB  cores=%lu\n",
                    cpuPct, rssKB, peakRssKB, cores);
            }
            else
            {
                const UINT64 dLogin = totalLogin - m_prevTotalLogin;
                const UINT64 dPackets = packetCount - m_prevPacketCount;
                const UINT64 dRecv = bytesRecv - m_prevBytesRecv;
                const UINT64 dSent = bytesSent - m_prevBytesSent;
                const UINT64 dTickUs = totalTickUs - m_prevTotalTickUs;
                const UINT64 dTicks = tickCount - m_prevTickCount;
                const UINT64 dOver = overBudget - m_prevOverBudgetCount;
                const UINT64 dWarn = warnCount - m_prevWarnCount;
                const UINT64 dWsa = (wsaPostTotal >= m_prevWsaSendCount) ? (wsaPostTotal - m_prevWsaSendCount) : 0;   // 단조 총량이라 정상 증가, 가드는 fold 레이스 방어(dWall 패턴)

                const ULONGLONG dKernel = kernel100ns - m_prevKernel100ns;
                const ULONGLONG dUser = user100ns - m_prevUser100ns;
                const ULONGLONG dWall = (wall100ns > m_prevWall100ns) ? (wall100ns - m_prevWall100ns) : 0;
                const double intervalSec = static_cast<double>(dWall) / 10000000.0;   // 100ns -> sec

                double cpuPct = 0.0;
                if (dWall > 0 && cores > 0)
                {
                    cpuPct = static_cast<double>(dKernel + dUser)
                        / static_cast<double>(dWall)
                        / static_cast<double>(cores) * 100.0;
                }
                const double loginPerSec = (intervalSec > 0.0) ? (static_cast<double>(dLogin) / intervalSec) : 0.0;
                const double pps = (intervalSec > 0.0) ? (static_cast<double>(dPackets) / intervalSec) : 0.0;   // PPS = 초당 처리 패킷 수(게임서버 TPS=Ticks/s와 구분)
                const double recvKBs = (intervalSec > 0.0) ? (static_cast<double>(dRecv) / 1024.0 / intervalSec) : 0.0;
                const double sentKBs = (intervalSec > 0.0) ? (static_cast<double>(dSent) / 1024.0 / intervalSec) : 0.0;
                const UINT64 avgTickUs = (dTicks > 0) ? (dTickUs / dTicks) : 0;
                const double overPct = (dTicks > 0) ? (static_cast<double>(dOver) / static_cast<double>(dTicks) * 100.0) : 0.0;

                wprintf(L"===== SERVER MONITOR (delta %.1fs) =====\n", intervalSec);
                wprintf(L" CCU(auth)=%ld  sockets=%ld  totalLogin=%llu  login/s=%.1f  playerPool=%d  gsPool=%d\n",
                    ccu, sockets, totalLogin, loginPerSec, playerPoolUsed, gsPoolUsed);
                const double wsaPerSec = (intervalSec > 0.0) ? (static_cast<double>(dWsa) / intervalSec) : 0.0;
                wprintf(L" PPS=%.1f  recv=%.1fKB/s  sent=%.1fKB/s  wsaSend=%.0f/s  sendDrop=%llu(누적)\n",
                    pps, recvKBs, sentKBs, wsaPerSec, sendDropTotal);   // PPS=초당 처리 패킷 수(Ticks/s 아님). sendDrop/wsaSend=은퇴 누적+활성 슬롯 합(단조 복원). sendDrop=헤드룸 부족 배치 drop(0이 정상)
                wprintf(L" tick avg=%lluus  max(all)=%lluus  over=%.1f%%  warn=%llu  ticks=%llu\n",
                    avgTickUs, maxTickUs, overPct, dWarn, dTicks);   // max(all) = 시작 이후 최대치(구간 변화량 아님)
                wprintf(L" CPU(proc)=%.1f%%  rss=%lluKB  peakRss=%lluKB  cores=%lu\n",
                    cpuPct, rssKB, peakRssKB, cores);
            }

            // (d-1.5) DB 복원력 지표 (since-start/delta 공통) - 둘 다 0 이 정상.
            //   reconnect = 연결 끊김에서 복구한 횟수 / queryFail = 재실행까지 실패한 작업 누적.
            wprintf(L" db: reconnect=%u  queryFail=%u  (누적)\n",
                MySqlDBProvider::GetReconnectCount(), MySqlDBProvider::GetQueryFailCount());

            // (d-1.6) 워치독 지표 - 채널 tick 최대 무진행 관측치. 임계 도달 = 강제 크래시라 여기 값은 항상 임계 미만.
            wprintf(L" watchdog: maxStall=%llums / 임계 %llums\n",
                ChannelWatchdog::GetMaxStallMs(), ChannelWatchdog::STALL_LIMIT_MS);

            // (d-2) POPULATION - 총/채널별/맵별 플레이어 + 맵별 몬스터 (since-start/delta 공통 출력)
            //   플레이어 = ChannelManager m_sessionMap read-락 1회 순회(메인 안전) / 몬스터 = MapManager 카운터(근사 read).
            std::vector<int> perChannel(static_cast<size_t>(channelCount), 0);
            std::vector<int> perChannelMap(static_cast<size_t>(channelCount) * MAX_MAP_COUNT, 0);   // 할당 = 인덱싱(CollectPopulation)과 같은 상한 상수
            if (channelManager != nullptr)
            {
                channelManager->CollectPopulation(channelCount, perChannel.data(), perChannelMap.data());
            }
            int totalPlayers = 0;
            for (int ch = 0; ch < channelCount; ++ch) { totalPlayers += perChannel[ch]; }

            wprintf(L"----- POPULATION (총 플레이어=%d · CCU(auth)=%ld) -----\n", totalPlayers, ccu);
            for (int ch = 0; ch < channelCount; ++ch)
            {
                const ChannelStatsSnapshot cs = channels[ch]->GetMonitorStats();
                wprintf(L" [ch%d] players=%d  map players=", ch, perChannel[ch]);
                for (int m = 0; m < MapTableCount(); ++m)   // 표시도 쓰는 맵 수만큼 (미사용 슬롯 0 나열 방지)
                {
                    if (m > 0) { wprintf(L"/"); }
                    wprintf(L"%d", perChannelMap[static_cast<size_t>(ch) * MAX_MAP_COUNT + m]);   // 보폭 = 할당과 같은 상한 상수
                }
                wprintf(L"  monsters=");
                for (int m = 0; m < MapTableCount(); ++m)
                {
                    if (m > 0) { wprintf(L"/"); }
                    wprintf(L"%d", cs.monsterPerMap[m]);
                }
                wprintf(L"\n");
            }

            // (d-2b) 채널 회계 불변식 (관측 계측) - cap 카운터(m_channelPlayerCount) drift/음수/over-cap + player-less 를 한 스냅샷으로 직접 검사.
            //   sum(cap)==CCU : cap 은 admission ++/종료 --/이관 net-0 이라 항상 CCU 와 같아야 한다(어긋나면 이중감소/이중증가 drift).
            //   derived+playerLess==CCU : 인게임(Player 부착) + 로딩 중 = 전 admitted 세션. 위 POPULATION display 가 로딩 세션을 빼서 CCU 와 안 맞는 것을 설명.
            //   위 (d-2) POPULATION 은 사람 눈용 display, 여기는 무결성 자동 판정(부하 중 상시 [OK] 여야 정상).
            if (channelManager != nullptr)
            {
                std::vector<int> capCounts(static_cast<size_t>(channelCount), 0);
                int derivedTotal = 0;
                int playerLess = 0;
                long ccuSnap = 0;
                channelManager->CollectPopulationDiagnostics(channelCount, capCounts.data(), &derivedTotal, &playerLess, &ccuSnap);
                long sumCap = 0;
                bool capRangeBad = false;
                for (int ch = 0; ch < channelCount; ++ch)
                {
                    sumCap += capCounts[ch];
                    if (capCounts[ch] < 0 || capCounts[ch] > MAX_PLAYERS_PER_CHANNEL) { capRangeBad = true; }
                }
                const bool driftOk = (sumCap == ccuSnap);
                const bool acctOk = (static_cast<long>(derivedTotal) + static_cast<long>(playerLess) == ccuSnap);
                wprintf(L"----- 채널 회계 (cap 카운터 검사) %ls -----\n",
                    (driftOk && acctOk && !capRangeBad) ? L"[OK]" : L"[!! ALERT !!]");
                wprintf(L" sum(cap)=%ld  CCU=%ld  drift=%ld(0이어야) | derived=%d + playerLess=%d = %d (==CCU?)\n",
                    sumCap, ccuSnap, sumCap - ccuSnap, derivedTotal, playerLess, derivedTotal + playerLess);
                wprintf(L" cap[]=");
                for (int ch = 0; ch < channelCount; ++ch) { if (ch > 0) { wprintf(L"/"); } wprintf(L"%d", capCounts[ch]); }
                wprintf(L"%ls\n", capRangeBad ? L"  [!] 음수 또는 cap 초과 발견" : L"");
            }

            // (d-3) TICK 정밀 - 채널별 trimmed mean(대표) + p50/p95/p99/windowMax(tail) + idle% (최근 window 표본 정렬 산출).
            //   1Ch=1Thread라 tick 성능은 채널 독립 -> 채널별 표시. 위 (d) avg/max(누적)와 보완 - 여기는 분포(spike 분리).
            wprintf(L"----- TICK 정밀 (채널별·최근 window 표본 정렬) -----\n");
            // idle/phase%는 최근 구간 delta로 계산 - 누적이면 시작 직후 한가했던 시간이 영구 혼입돼 부하가 꽉 차도 idle이 0으로 안 내려감.
            //   채널 수에 맞춰 직전값 버퍼 준비(첫 q 또는 채널 수 변동 시). resize는 0으로 채워 첫 q는 since-start와 동일.
            if (static_cast<int>(m_prevChSleepUs.size()) != channelCount)
            {
                m_prevChSleepUs.assign(channelCount, 0);
                m_prevChTotalTickUs.assign(channelCount, 0);
                m_prevChPhaseRecvUs.assign(channelCount, 0);
                m_prevChPhaseBroadcastUs.assign(channelCount, 0);
                m_prevChPhaseSendUs.assign(channelCount, 0);
                m_prevChPhaseUpdateUs.assign(channelCount, 0);
                m_prevChOverBudget.assign(channelCount, 0);
                m_prevChTickCount.assign(channelCount, 0);
            }
            for (int ch = 0; ch < channelCount; ++ch)
            {
                const ChannelStatsSnapshot ts = channels[ch]->GetMonitorStats();

                // 최근 구간 증분 (현재 누적 - 직전 누적). 누적은 줄지 않으므로 음수 방어는 안전상.
                const UINT64 dSleep    = (ts.sleepUs        >= m_prevChSleepUs[ch])         ? (ts.sleepUs        - m_prevChSleepUs[ch])         : 0;
                const UINT64 dTick     = (ts.totalTickUs    >= m_prevChTotalTickUs[ch])     ? (ts.totalTickUs    - m_prevChTotalTickUs[ch])     : 0;
                const UINT64 dRecvP    = (ts.phaseRecvUs    >= m_prevChPhaseRecvUs[ch])     ? (ts.phaseRecvUs    - m_prevChPhaseRecvUs[ch])     : 0;
                const UINT64 dBcastP   = (ts.phaseBroadcastUs >= m_prevChPhaseBroadcastUs[ch]) ? (ts.phaseBroadcastUs - m_prevChPhaseBroadcastUs[ch]) : 0;
                const UINT64 dSendP    = (ts.phaseSendUs    >= m_prevChPhaseSendUs[ch])     ? (ts.phaseSendUs    - m_prevChPhaseSendUs[ch])     : 0;
                const UINT64 dUpdateP  = (ts.phaseUpdateUs  >= m_prevChPhaseUpdateUs[ch])   ? (ts.phaseUpdateUs  - m_prevChPhaseUpdateUs[ch])   : 0;
                const UINT64 dOverCh   = (ts.overBudgetCount >= m_prevChOverBudget[ch])     ? (ts.overBudgetCount - m_prevChOverBudget[ch])     : 0;
                const UINT64 dTickCh   = (ts.tickCount      >= m_prevChTickCount[ch])       ? (ts.tickCount      - m_prevChTickCount[ch])       : 0;

                // idle%: 최근 구간 실제 Sleep / 전체(sleep+처리) - trimmedAvg 추정과 달리 bimodal/선점에서도 정확(진짜 여유).
                double idlePct = 0.0;
                const double totalUs = static_cast<double>(dSleep + dTick);
                if (totalUs > 0.0)
                {
                    idlePct = static_cast<double>(dSleep) / totalUs * 100.0;
                }
                // over%: 최근 구간 예산(33.33ms) 초과 tick 비율 - 채널별이라 idle과 짝(over 100% = 모든 tick 초과 = idle 0%).
                const double overPctCh = (dTickCh > 0) ? (static_cast<double>(dOverCh) / static_cast<double>(dTickCh) * 100.0) : 0.0;
                wprintf(L" [ch%d] trimAvg=%lluus  p50=%llu  p95=%llu  p99=%llu  wMax=%llu  idle=%.0f%%  over=%.0f%%  (n=%d)\n",
                    ch, ts.trimmedAvgUs, ts.p50Us, ts.p95Us, ts.p99Us, ts.windowMaxUs, idlePct, overPctCh, ts.tickSampleCount);
                wprintf(L"        input/tick: p50=%llu  p95=%llu  max=%llu  moveDrop=%llu  critDrop=%llu  (moveDrop=CS_MOVE shed·critDrop>0=critical 손실 경고)\n",
                    ts.jobP50, ts.jobP95, ts.jobMax, ts.moveDropped, ts.criticalDropped);
                // per-phase 비율 (최근 구간 처리시간 합 대비) - tick이 어느 phase서 나오는지. send=broadcast의 부분집합.
                const UINT64 ttu = (dTick > 0) ? dTick : 1;
                wprintf(L"        phase%%: recv=%.0f%%  bcast=%.0f%%(send=%.0f%%)  update=%.0f%%\n",
                    static_cast<double>(dRecvP) * 100.0 / static_cast<double>(ttu),
                    static_cast<double>(dBcastP) * 100.0 / static_cast<double>(ttu),
                    static_cast<double>(dSendP) * 100.0 / static_cast<double>(ttu),
                    static_cast<double>(dUpdateP) * 100.0 / static_cast<double>(ttu));

                // 직전값 갱신 (다음 q의 비교 기준).
                m_prevChSleepUs[ch]          = ts.sleepUs;
                m_prevChTotalTickUs[ch]      = ts.totalTickUs;
                m_prevChPhaseRecvUs[ch]      = ts.phaseRecvUs;
                m_prevChPhaseBroadcastUs[ch] = ts.phaseBroadcastUs;
                m_prevChPhaseSendUs[ch]      = ts.phaseSendUs;
                m_prevChPhaseUpdateUs[ch]    = ts.phaseUpdateUs;
                m_prevChOverBudget[ch]       = ts.overBudgetCount;
                m_prevChTickCount[ch]        = ts.tickCount;
            }

            // (d-4) 송신 백프레셔 - 축출 누적 + 상위 N 느린 세션(peak% 내림차순, 2차키 dropBytes). 라이브러리 raw 를 게임측서 정렬/라벨(도메인 무관 유지).
            //   drop 이 나면 이미 큐 100% 포화(사후)라, peak%(사전 게이지) + drop(사후 카운터)를 함께 봐야 만성 배압 vs 일시 spike 를 가른다.
            const int kSlowSessionTopN = 8;   // percentile 히스토그램 인프라 없이 tail(느린 소비자) 저비용 식별
            wprintf(L"----- 송신 백프레셔 (상위 %d 느린 세션·peak%% 내림차순) -----\n", kSlowSessionTopN);
            wprintf(L" sendDrop누적=%llu  wsaPost누적=%llu  wrap누적=%llu  축출(SEND_TIMEOUT)=%llu\n",
                sendDropTotal, wsaPostTotal, wrapTotal, (server != nullptr ? server->GetSendDropEvictTotal() : 0ull));

            // 인터서버 링크 / flood 방어 / 로거 자체 건강 - 쓰기만 되던 지표를 표출에 연결 (0 이 정상인 경보성 값들)
            {
                LoginLinkThread& link = LoginLinkThread::GetInstance();
                wprintf(L" 인터서버: link=%s  verifyOk=%ld  notFound=%ld  alreadyOnline=%ld  error=%ld\n",
                    link.IsLinkUp() ? L"UP" : L"DOWN",
                    LoginLinkThread::s_verifyOk, LoginLinkThread::s_verifyNotFound,
                    LoginLinkThread::s_verifyAlreadyOnline, LoginLinkThread::s_verifyError);
                wprintf(L" flood: ipReject=%llu  reaped=%ld  preAuthBlock=%ld  linkQDrop=%ld   |   log: drop=%llu  truncated=%llu  writeFail=%llu\n",
                    MainServer::GetInstance().GetIpRejectCount(), GetReapedTotal(),
                    ChannelManager::GetInstance().GetPreAuthBlockCount(), LoginLinkThread::s_linkQueueDrop,
                    KYS::GAMESERVER::LOG::Logger::GetInstance().GetDropCount(),
                    KYS::GAMESERVER::LOG::Logger::GetInstance().GetTruncatedCount(),
                    KYS::GAMESERVER::LOG::Logger::GetInstance().GetWriteFailCount());
            }

            // 큐 점유(peak>0)한 라이브 세션만 후보로 모아 peak% 내림차순 정렬(2차키 dropBytes). capacity 교차곱으로 부동소수 없이 비교.
            std::vector<int> slowIdx;
            slowIdx.reserve(64);
            for (int i = 0; i < sendSlotCount; ++i)
            {
                const KYS::GAMESERVER::NETWORK::SessionSendMetrics& m = sendSnap[static_cast<size_t>(i)];
                if (m.sid != 0 && m.peakUsed > 0) { slowIdx.push_back(i); }
            }
            std::sort(slowIdx.begin(), slowIdx.end(),
                [&sendSnap](int a, int b) -> bool
                {
                    const KYS::GAMESERVER::NETWORK::SessionSendMetrics& ma = sendSnap[static_cast<size_t>(a)];
                    const KYS::GAMESERVER::NETWORK::SessionSendMetrics& mb = sendSnap[static_cast<size_t>(b)];
                    const UINT64 lhs = static_cast<UINT64>(ma.peakUsed) * (mb.capacity ? mb.capacity : 1);   // ma.peak/ma.cap vs mb.peak/mb.cap 를 분모 교차곱으로
                    const UINT64 rhs = static_cast<UINT64>(mb.peakUsed) * (ma.capacity ? ma.capacity : 1);
                    if (lhs != rhs) { return lhs > rhs; }
                    return ma.dropBytes > mb.dropBytes;   // 2차키 = drop 바이트 큰 순
                });

            if (slowIdx.empty())
            {
                wprintf(L"   (백프레셔 없음 - 큐 점유 세션 0)\n");
            }
            else
            {
                const int shown = (static_cast<int>(slowIdx.size()) < kSlowSessionTopN) ? static_cast<int>(slowIdx.size()) : kSlowSessionTopN;
                wprintf(L"   (peak=현재 연결 생애 최대 - 장수 세션 편향 / winAvg=send-사건가중 창평균)\n");
                for (int r = 0; r < shown; ++r)
                {
                    const int idx = slowIdx[static_cast<size_t>(r)];
                    const KYS::GAMESERVER::NETWORK::SessionSendMetrics& m = sendSnap[static_cast<size_t>(idx)];
                    const double peakPct = (m.capacity > 0) ? (static_cast<double>(m.peakUsed) / static_cast<double>(m.capacity) * 100.0) : 0.0;

                    // 라벨 병합: sid -> GameSession(accountId/channelId/midFlush). pre-auth(게임세션 없음)면 'pre-auth'.
                    //   락 밖 근사 read(평문 스칼라만) - Dump 도중 그 세션이 끊기면 라벨이 stale(acct 0/ch -) 1프레임 가능. 진단 표시라 허용(런타임 무해).
                    long acct = -1;
                    int  ch = -1;
                    UINT32 midFlush = 0;
                    if (channelManager != nullptr)
                    {
                        GameSession* gs = channelManager->GetSession(m.sid);
                        if (gs != nullptr)
                        {
                            acct = static_cast<long>(gs->GetAccountId());
                            ch = gs->GetChannelId();
                            midFlush = gs->GetMidTickFlushCount();
                        }
                    }
                    wchar_t acctBuf[16];
                    if (acct >= 0) { swprintf_s(acctBuf, L"%ld", acct); } else { wcscpy_s(acctBuf, L"pre-auth"); }
                    wchar_t chBuf[8];
                    if (ch >= 0) { swprintf_s(chBuf, L"%d", ch); } else { wcscpy_s(chBuf, L"-"); }
                    wchar_t winAvgBuf[32];
                    const double wavg = slotWindowAvg[static_cast<size_t>(idx)];
                    // 정상 창평균 <= capacity(16384). 무락 근사 read 가 Reset 과 겹친 torn 조합에서 비정상 거대값이 나오면 '-' 로 봉인
                    //   (좁은 버퍼 + 거대 수치는 secure CRT invalid-parameter 로 프로세스 종료를 유발할 수 있어 표시 계층에서 유한값으로 자름).
                    if (wavg >= 0.0 && wavg <= 65536.0) { swprintf_s(winAvgBuf, L"%.0fB", wavg); } else { wcscpy_s(winAvgBuf, L"-"); }   // 정상 = 값+단위(예: 82B) / 이번 창 미송신, 재사용 슬롯 = '-' (단위 없이 - 'B' 오독 방지)

                    wprintf(L"   sid=%llu acct=%ls ch=%ls drop=%u/%lluB peak=%.1f%% wrap=%u wmHit=%u midFlush=%u winAvg=%ls\n",
                        m.sid, acctBuf, chBuf, m.dropCount, m.dropBytes, peakPct, m.wrapCount, m.watermarkHits, midFlush, winAvgBuf);
                }
            }

            // (e) 직전값 갱신 (다음 q의 비교 기준)
            m_prevTotalLogin = totalLogin;
            m_prevPacketCount = packetCount;
            m_prevBytesRecv = bytesRecv;
            m_prevBytesSent = bytesSent;
            m_prevTotalTickUs = totalTickUs;
            m_prevTickCount = tickCount;
            m_prevOverBudgetCount = overBudget;
            m_prevWarnCount = warnCount;
            m_prevWsaSendCount = wsaPostTotal;    // 단조 복원 총량 기준(다음 q의 wsaSend/s delta)
            m_prevSendWrapCount = wrapTotal;
            m_prevKernel100ns = kernel100ns;
            m_prevUser100ns = user100ns;
            m_prevWall100ns = wall100ns;
            m_hasBaseline = true;
        }
    }
}
