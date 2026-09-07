#pragma once
#pragma warning(push)
#pragma warning(disable:4324)   // alignas 채움 안내 - head/tail 캐시라인 분리는 의도된 설계
#include "./Core/Thread/IJob.h"
#include "./Core/Memory/ObjectPool.h"

namespace KYS
{
    namespace GAMESERVER
    {
        namespace THREAD
        {

            // 락 없이 여러 스레드가 동시에 작업을 넣고 빼는 큐. 넣기/빼기를 원자적 비교-교환(CAS)으로 처리.
            //   [검증/학습용] 프로덕션 큐는 TwoLockJobQueue - 이 lock-free 큐는 CAS/ABA 방어 학습, 스트레스 검증 자산으로 보존(호출부 0).
            class LockFreeQueue
            {
            public:
                explicit LockFreeQueue(UINT32 nodePoolCapacity = 1024);   // 미리 잡아둘 노드 개수 지정
                ~LockFreeQueue();

                LockFreeQueue(const LockFreeQueue&) = delete;
                LockFreeQueue& operator=(const LockFreeQueue&) = delete;
                LockFreeQueue(LockFreeQueue&&) = delete;
                LockFreeQueue& operator=(LockFreeQueue&&) = delete;


                void Enqueue(IJob* job);        // 작업을 큐 끝에 추가 (여러 워커가 동시 호출 가능)
                bool Dequeue(IJob*& outJob);    // 큐 앞에서 작업 하나 꺼냄 (성공 시 true, outJob에 담음)
                bool IsEmpty() const;           // 비었는지 확인 (스냅샷 - 직후 바뀔 수 있음)

            private:
                struct Node
                {
                    IJob* job;
                    volatile uintptr_t next;  // 다음 노드 + 재사용 오인(ABA) 방지용 태그를 한 워드에 묶음

                    // 빈 생성자로 값-초기화를 막는다. ObjectPool이 Allocate마다 new(p)Node()로
                    // next를 0으로 밀어버리면 재활용 노드의 태그 이력이 사라져 ABA 방어가 무너진다.
                    // 이 생성자가 있으면 재활용 시 이전 next(태그 포함)가 메모리에 그대로 살아남는다.
                    Node() {}
                };

                static constexpr uintptr_t POINTER_MASK = 0x0000FFFFFFFFFFFFULL;   // 하위 48비트 = 실제 포인터
                static constexpr int TAG_SHIFT = 48;                               // 상위 16비트 = 태그 자리

                // 포인터와 태그를 한 워드로 합치고 다시 떼어내는 보조 함수들 (위 next 필드 다룰 때 사용)
                static uintptr_t MakeTagged(Node* ptr, uint16_t tag)
                {
                    return (reinterpret_cast<uintptr_t>(ptr) & POINTER_MASK) |
                        (static_cast<uintptr_t>(tag) << TAG_SHIFT);
                }

                static Node* GetPtr(uintptr_t tagged)
                {
                    return reinterpret_cast<Node*>(tagged & POINTER_MASK);
                }

                static uint16_t GetTag(uintptr_t tagged)
                {
                    return static_cast<uint16_t>(tagged >> TAG_SHIFT);
                }

                // m_head와 m_tail을 각자 다른 캐시라인에 두어 서로의 캐시를 무효화하지 않게(false sharing 회피)
                alignas(64) volatile uintptr_t m_head;
                alignas(64) volatile uintptr_t m_tail;

                KYS::GAMESERVER::MEMORY::ObjectPool<Node> m_nodePool;
            };

        }
    }
}

#pragma warning(pop)
