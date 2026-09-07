#pragma once
#include "../../../GameServer/Core/Thread/SpinLock.h"    // 요청 큐 보호 (producer=채널 스레드들 / consumer=이 스레드)
#include "../../../GameServer/Core/Thread/IJob.h"         // PathResultJob base
#include "../../../GameServer/Types/Defines.h"            // UINT32/UINT64
#include "../../../GameCommon/Pathfinding/PathFinder.h"               // PathFinder (스레드가 1 인스턴스 소유) + PATH_WAYPOINT_MAX
#include "../../../GameCommon/CommonStructs.h"            // Position
#include <Windows.h>                                      // HANDLE

namespace KYS
{
    namespace SERVERAPP
    {
        // 요청 큐 상한 - 포화 시 drop (경로는 소모품, 다음 tick repath 재요청이 자기치유). 몬스터 총량 ~1080 + 여유.
        static const int MAX_PATH_REQUESTS    = 2048;
        // 한 루프 사이클당 처리 상한 (스레드가 큐를 독점하지 않게 - dtPathQueue maxIters 동형).
        static const int PATHFINDER_BATCH_MAX = 16;

        // 길찾기 요청 - 값 스냅샷(가변 상태 참조 0). 발행 시점 정보만 담고, 결과 도착 시 재조회로 재검증한다.
        struct PathfindRequest
        {
            int      channelId;   // 결과 회신 대상 채널
            UINT32   monsterId;   // 요청 몬스터 wire id (값 - dangling 불가, 재조회 키)
            int      mapId;       // 탐색 맵
            Position from;        // 출발 (몬스터 위치)
            Position to;          // 목적 (타깃 위치)
            UINT32   targetId;    // 발행 시점 교전 대상 (도착 시 대상 바뀌었으면 stale)
            UINT32   seq;         // per-몬스터 단조 요청 번호 (늦게 온 옛 결과 폐기)
        };

        // 전용 길찾기 스레드 (DBThread 형틀) - A*/JPS 를 별 스레드에서 돌려 30Hz 채널 tick 을 막지 않는다.
        //   요청 = 값 스냅샷 큐(고정 배열·포화 drop), 결과 = 대상 채널 mailbox 재주입(PathResultJob).
        //   수명 계약: WalkableTable 로드 후 Start / 채널 스레드 join "전" Stop - 채널이 살아있는 동안 in-flight 결과가
        //   mailbox 로 회신되게 한다. 역순(채널 join 후 Stop)이면 이미 죽은 채널 큐로 결과를 부치게 된다.
        //   잔여 요청은 undrained, 잔여 결과는 mailbox 잔류 - DROP 레인이고 프로세스 종료라 무해.
        class PathfinderThread
        {
        public:
            static PathfinderThread& GetInstance() { static PathfinderThread instance; return instance; }

            bool Start();   // 스레드 기동 (WalkableTable 로드 뒤). 실패 = false (fail-fast)
            void Stop();    // m_running=false + join

            // 요청 발행 (몬스터측·채널 스레드). 큐 포화면 false - 호출자가 pending 을 설정하지 않는다(D-H1).
            bool Enqueue(const PathfindRequest& req);

            void ThreadLoop();   // 스레드 본체 (public - file-local 진입 함수가 바로 호출)

            // 관측 (Interlocked 카운터 - 모니터 스레드 근사 read)
            long long GetRequestCount() const { return m_reqCount; }
            long long GetSuccessCount() const { return m_okCount; }
            long long GetNoPathCount() const  { return m_noPathCount; }
            long long GetDropCount() const    { return m_dropCount + m_resultDropCount; }

            PathfinderThread(const PathfinderThread&) = delete;
            PathfinderThread& operator=(const PathfinderThread&) = delete;

        private:
            PathfinderThread();

            bool Dequeue(PathfindRequest& out);   // 큐에서 하나 꺼냄 (비었으면 false)

            KYS::GAMESERVER::THREAD::SpinLock m_queueLock;    // 요청 큐 보호
            PathfindRequest m_queue[MAX_PATH_REQUESTS];       // 고정 배열 링 (힙 0)
            int m_head;    // 다음 꺼낼 위치
            int m_tail;    // 다음 넣을 위치
            int m_count;   // 현재 적재 수

            PathFinder m_pathFinder;   // 이 스레드 전용 인스턴스 1개 (무락 - WalkableTable 공유 표 조회)

            HANDLE        m_thread;
            volatile bool m_running;

            volatile long long m_reqCount;         // 처리한 요청 수
            volatile long long m_okCount;          // 경로 찾음
            volatile long long m_noPathCount;      // 도달불가/예산초과
            volatile long long m_dropCount;        // 요청 큐 포화 drop
            volatile long long m_resultDropCount;  // 결과 mailbox 포화 drop
        };

        // 길찾기 결과를 대상 채널 스레드에서 몬스터에 적용하는 Job.
        //   Execute: monsterId 로 몬스터 재조회(맵 순회) + liveness/AI/seq 재검증 후 waypoint 적용 (실패 = 폐기).
        //   waypoints 는 값 복사로 담는다 (포인터 결과 금지 - LIFO ABA / dangling 봉인).
        class PathResultJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            PathResultJob(int channelId, UINT32 monsterId, int mapId, UINT32 targetId, UINT32 seq,
                          bool success, const Position* waypoints, int count);
            ~PathResultJob() override = default;
            void Execute() override;

        private:
            int      m_channelId;
            UINT32   m_monsterId;
            int      m_mapId;
            UINT32   m_targetId;
            UINT32   m_seq;
            bool     m_success;
            int      m_count;
            Position m_waypoints[PATH_WAYPOINT_MAX];   // 값 복사 (count 개만 유효)
        };
    }
}
