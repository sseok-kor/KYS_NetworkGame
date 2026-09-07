#pragma once
#pragma warning(push)
#pragma warning(disable:4324)
#include "./Core/Thread/SpinLock.h"
#include "./Core/Thread/IJob.h"
#include "./Core/Memory/ObjectPool.h"

namespace KYS
{
    namespace GAMESERVER
    {
        namespace THREAD
        {
            // 노드 풀이 바닥났을 때 어떻게 할지 정하는 정책.
            //   DROP = 작업을 버림 (데이터 큐 - 밀리면 버려도 되는 것)
            //   GROW = heap에서 노드를 새로 만들어 이어감 (세션 이벤트 큐 - 절대 버리면 안 되는 것)
            enum class EOverflowPolicy
            {
                DROP,
                GROW,
            };

            // 넣는 쪽(tail)과 빼는 쪽(head) 락을 따로 둔 작업 큐. 넣기는 여러 워커, 빼기는 한 스레드.
            class TwoLockJobQueue
            {
            public:
                explicit TwoLockJobQueue(UINT32 nodePoolCapacity = 1024,
                                         EOverflowPolicy policy = EOverflowPolicy::DROP);   // 노드 풀 크기와 소진 정책 지정
                ~TwoLockJobQueue();


                TwoLockJobQueue(const TwoLockJobQueue&) = delete;
                TwoLockJobQueue& operator=(const TwoLockJobQueue&) = delete;
                TwoLockJobQueue(TwoLockJobQueue&&) = delete;
                TwoLockJobQueue& operator=(TwoLockJobQueue&&) = delete;

                bool Enqueue(IJob* job);          // 작업을 큐 끝에 추가 (여러 워커 동시 - tail 락). 반환: true=적재 / false=DROP(풀 소진, 정책 DROP)
                bool Dequeue(IJob*& outJob);       // 큐 앞에서 작업 하나 꺼냄 (한 소비자 전용 - head 락)
                bool IsEmpty() const;              // 비었는지 확인 (스냅샷 - head 락)

            private:
                struct Node
                {
                    IJob* job;
                    Node* next;                   // 다음 노드 (락이 지켜주므로 일반 포인터로 충분)
                    bool  fromHeap;   // true=heap에서 만든 노드, false=풀 노드. 반납 경로 구분용
                };

                // 빼는 쪽(소비자, Channel) - 넣는 쪽과 캐시라인을 갈라 서로 간섭 안 하게
                alignas(64) Node* m_head;         // 현재 dummy 노드
                mutable SpinLock m_headLock;      // const IsEmpty()에서도 잠가야 해서 mutable

                // 넣는 쪽(생산자, 워커들) - 다른 캐시라인
                alignas(64) Node* m_tail;         // 마지막 노드
                SpinLock m_tailLock;

                KYS::GAMESERVER::MEMORY::ObjectPool<Node> m_nodePool;  // 노드 풀 (dummy 포함, 여러 스레드 공용)
                EOverflowPolicy m_policy;   // 풀 소진 시 정책 (DROP/GROW)
            };
        }
    }
}

#pragma warning(pop)
