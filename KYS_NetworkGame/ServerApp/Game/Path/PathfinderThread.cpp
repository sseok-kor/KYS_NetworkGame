#include "pch_serverapp.h"
#include "PathfinderThread.h"
#include "../Channel/ChannelManager.h"   // GetChannel (결과 회신 대상 조회)
#include "../Channel/IChannel.h"          // EnqueueJob / FindMonsterForPath
#include "../Monster/Monster.h"           // OnPathResult (결과 적용)

#include <process.h>                       // _beginthreadex

namespace KYS
{
    namespace SERVERAPP
    {
        // file-local 진입 함수 - ThreadLoop 이 public 이라 바로 호출 (DBThread/Channel 과 같은 방식).
        static unsigned __stdcall PathfinderThreadEntry(void* arg)
        {
            static_cast<PathfinderThread*>(arg)->ThreadLoop();
            return 0;
        }

        PathfinderThread::PathfinderThread()
            : m_head(0)
            , m_tail(0)
            , m_count(0)
            , m_pathFinder()   // 기본 grid = WalkableTable 공유 표 (프로덕션)
            , m_thread(NULL)
            , m_running(false)
            , m_reqCount(0)
            , m_okCount(0)
            , m_noPathCount(0)
            , m_dropCount(0)
            , m_resultDropCount(0)
        {
        }

        bool PathfinderThread::Start()
        {
            m_running = true;
            m_thread = reinterpret_cast<HANDLE>(
                _beginthreadex(NULL, 0, &PathfinderThreadEntry, this, 0, NULL));
            return m_thread != NULL;
        }

        void PathfinderThread::Stop()
        {
            m_running = false;
            if (m_thread != NULL)
            {
                WaitForSingleObject(m_thread, INFINITE);   // join
                CloseHandle(m_thread);
                m_thread = NULL;
            }
        }

        bool PathfinderThread::Enqueue(const PathfindRequest& req)
        {
            KYS::GAMESERVER::THREAD::SpinLockGuard guard(m_queueLock);
            if (m_count >= MAX_PATH_REQUESTS)
            {
                ::InterlockedIncrement64(&m_dropCount);   // 큐 포화 - drop (repath 재요청이 자기치유)
                return false;
            }
            m_queue[m_tail] = req;
            m_tail = (m_tail + 1) % MAX_PATH_REQUESTS;
            ++m_count;
            return true;
        }

        bool PathfinderThread::Dequeue(PathfindRequest& out)
        {
            KYS::GAMESERVER::THREAD::SpinLockGuard guard(m_queueLock);
            if (m_count == 0) { return false; }
            out = m_queue[m_head];
            m_head = (m_head + 1) % MAX_PATH_REQUESTS;
            --m_count;
            return true;
        }

        void PathfinderThread::ThreadLoop()
        {
            while (m_running)
            {
                int processed = 0;
                PathfindRequest req;
                while (processed < PATHFINDER_BATCH_MAX && Dequeue(req))
                {
                    ++processed;
                    ::InterlockedIncrement64(&m_reqCount);

                    Position waypoints[PATH_WAYPOINT_MAX];
                    const int n = m_pathFinder.FindPath(req.mapId, req.from, req.to, waypoints, PATH_WAYPOINT_MAX);
                    const bool success = (n > 0);
                    if (success) { ::InterlockedIncrement64(&m_okCount); }
                    else         { ::InterlockedIncrement64(&m_noPathCount); }

                    IChannel* ch = ChannelManager::GetInstance().GetChannel(req.channelId);
                    if (ch == nullptr) { continue; }   // 채널 범위 밖 - 폐기

                    PathResultJob* job = new PathResultJob(req.channelId, req.monsterId, req.mapId,
                                                           req.targetId, req.seq, success, waypoints, n);
                    // critical mailbox 재주입 - 포화(DROP) 시 mailbox 가 job 을 이미 delete 하므로 여기선 회수 금지
                    //   (이중 해제 방지). bool 은 drop 계측용일 뿐 - 결과 유실은 몬스터 pending TTL 이 자기치유.
                    if (!ch->EnqueueJob(job))
                    {
                        ::InterlockedIncrement64(&m_resultDropCount);
                    }
                }
                ::Sleep(1);   // 매 사이클 1ms 양보 - 큐가 비어 바쁜대기 스핀 도는 것을 막는다(DBThread 방식 답습). 잔량 있으면 다음 사이클 배치가 이어감
            }
        }

        // ---- PathResultJob ----

        PathResultJob::PathResultJob(int channelId, UINT32 monsterId, int mapId, UINT32 targetId, UINT32 seq,
                                     bool success, const Position* waypoints, int count)
            : m_channelId(channelId)
            , m_monsterId(monsterId)
            , m_mapId(mapId)
            , m_targetId(targetId)
            , m_seq(seq)
            , m_success(success)
            , m_count(0)
        {
            int n = count;
            if (n < 0) { n = 0; }
            if (n > PATH_WAYPOINT_MAX) { n = PATH_WAYPOINT_MAX; }
            m_count = n;
            for (int i = 0; i < n; ++i) { m_waypoints[i] = waypoints[i]; }
        }

        void PathResultJob::Execute()
        {
            IChannel* ch = ChannelManager::GetInstance().GetChannel(m_channelId);
            if (ch == nullptr) { return; }

            // monsterId 로 살아있는 몬스터 재조회 (존재 + !IsDead + mapId 일치 - 없으면 그새 죽음/이관, 조용히 폐기).
            Monster* m = ch->FindMonsterForPath(m_mapId, m_monsterId);
            if (m == nullptr) { return; }

            // seq/targetId/AI 상태 재검증 + waypoint 적용/폐기 + pending 해제는 몬스터가 소유한 상태라 몬스터가 판정.
            m->OnPathResult(m_seq, m_targetId, m_success, m_waypoints, m_count);
        }
    }
}
