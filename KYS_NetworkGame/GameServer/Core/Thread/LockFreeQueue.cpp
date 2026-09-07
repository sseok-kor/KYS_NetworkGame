#include "pch_gameserver.h"
#include "LockFreeQueue.h"
#include "MemoryOrder.h"

namespace KYS
{
    namespace GAMESERVER
    {
        namespace THREAD
        {

            // 빈 큐 초기화. 처음엔 head/tail 둘 다 빈 표시용(dummy) 노드를 가리킴.
            //   nodePoolCapacity : 미리 잡아둘 노드 개수 (Enqueue마다 노드 하나 사용)
            LockFreeQueue::LockFreeQueue(UINT32 nodePoolCapacity)
                : m_nodePool(nodePoolCapacity)
            {
                // 빈 표시용 노드 하나 만들어 head/tail 둘 다 거기 걸어둠
                Node* dummy = m_nodePool.Allocate();
                dummy->job = nullptr;
                dummy->next = 0;  // 다음 노드 없음

                m_head = MakeTagged(dummy, 0);
                m_tail = MakeTagged(dummy, 0);
            }

            // 큐 정리. 남은 노드를 모두 꺼내 반납하고 마지막 dummy까지 회수.
            LockFreeQueue::~LockFreeQueue()
            {
                // 남은 작업 노드 전부 빼내기
                IJob* dummy;
                while (Dequeue(dummy)) {}

                // 마지막에 남은 dummy 노드 반납
                Node* last = GetPtr(m_head);
                if (last != nullptr)
                {
                    m_nodePool.Deallocate(last);
                }
            }

            // 작업 하나를 큐 끝에 매단다. 여러 워커가 동시에 호출해도 안전.
            //   job : 큐에 넣을 작업 (소유권은 꺼내가는 쪽으로 넘어감)
            void LockFreeQueue::Enqueue(IJob* job)
            {
                // 담을 노드 확보 시작 (풀이 비면 잠깐 양보하고 재시도)
                Node* newNode = nullptr;
                while (newNode == nullptr)
                {
                    newNode = m_nodePool.Allocate();
                    if (newNode == nullptr)
                    {
                        YieldProcessor();
                    }
                }
                newNode->job = job;
                // 끝에 붙을 거라 가리킬 다음 노드는 없지만(포인터=null), 태그는 0으로 리셋하지 않고
                // 이 노드에 살아남아 있던 이전 태그에서 +1 승계한다. 리셋하면 재활용될 때마다
                // 같은 (null, 0)이 재출현해 뒤처진 스레드의 CAS가 거짓 성공(ABA)하며 노드를 소거한다.
                // 승계하면 노드별 태그가 단조 증가해 그 오인이 사라진다(Michael-Scott 원논문 전제 복원).
                newNode->next = MakeTagged(nullptr,
                    static_cast<uint16_t>(GetTag(newNode->next) + 1));

                // tail 뒤에 새 노드를 거는 시도 반복 (충돌 시 재시도)
                while (true)
                {
                    uintptr_t lastTagged = LoadAcquire(&m_tail);
                    Node* last = GetPtr(lastTagged);
                    uintptr_t nextTagged = LoadAcquire(&(last->next));
                    Node* next = GetPtr(nextTagged);

                    // 읽는 사이 tail이 바뀌지 않았는지 확인
                    if (lastTagged == LoadAcquire(&m_tail))
                    {
                        if (next == nullptr)
                        {
                            // tail이 진짜 마지막 노드를 가리킴 -> 그 뒤에 새 노드 붙이기 시도
                            uintptr_t newNextTagged = MakeTagged(newNode, GetTag(nextTagged) + 1);

                            LONG64 prevValue = CompareExchangeAcqRel(
                                reinterpret_cast<LONG64 volatile*>(&last->next),
                                static_cast<LONG64>(newNextTagged),
                                static_cast<LONG64>(nextTagged));

                            if (static_cast<uintptr_t>(prevValue) == nextTagged)
                            {
                                // 연결 성공 -> tail도 새 노드로 전진 시도 (실패해도 다른 스레드가 마저 해줌)
                                uintptr_t newTailTagged = MakeTagged(newNode, GetTag(lastTagged) + 1);
                                CompareExchangeAcqRel(
                                    reinterpret_cast<LONG64 volatile*>(&m_tail),
                                    static_cast<LONG64>(newTailTagged),
                                    static_cast<LONG64>(lastTagged));
                                return;
                            }
                        }
                        else
                        {
                            // tail이 한 칸 뒤처짐 (다른 스레드가 연결만 하고 tail 전진 전) -> 대신 전진시켜 줌
                            uintptr_t newTailTagged = MakeTagged(next, GetTag(lastTagged) + 1);
                            CompareExchangeAcqRel(
                                reinterpret_cast<LONG64 volatile*>(&m_tail),
                                static_cast<LONG64>(newTailTagged),
                                static_cast<LONG64>(lastTagged));
                        }
                    }

                }
            }

            // 큐 앞에서 작업 하나를 꺼낸다. 비어 있으면 false.
            //   outJob : 꺼낸 작업을 담아 돌려줄 자리 (성공 시에만 유효)
            bool LockFreeQueue::Dequeue(IJob*& outJob)
            {
                while (true)
                {
                    uintptr_t firstTagged = LoadAcquire(&m_head);
                    Node* first = GetPtr(firstTagged);
                    uintptr_t lastTagged = LoadAcquire(&m_tail);
                    Node* last = GetPtr(lastTagged);
                    uintptr_t nextTagged = LoadAcquire(&first->next);
                    Node* next = GetPtr(nextTagged);

                    // 읽는 사이 head가 바뀌지 않았는지 확인
                    if (firstTagged == LoadAcquire(&m_head))
                    {
                        if (first == last)
                        {
                            // 큐가 비었거나 tail이 뒤처진 상태
                            if (next == nullptr)
                            {
                                // 진짜 빈 큐
                                outJob = nullptr;
                                return false;
                            }
                            // tail이 뒤처짐 -> 대신 전진시켜 줌
                            uintptr_t newTailTagged = MakeTagged(next, GetTag(lastTagged) + 1);
                            CompareExchangeAcqRel(
                                reinterpret_cast<LONG64 volatile*>(&m_tail),
                                static_cast<LONG64>(newTailTagged),
                                static_cast<LONG64>(lastTagged));
                        }
                        else
                        {
                            // 꺼낼 게 있음. 실제 데이터는 dummy(first) 다음 노드에 들어 있음
                            IJob* job = next->job;


                            // head를 다음 노드로 전진 시도
                            uintptr_t newHeadTagged = MakeTagged(next, GetTag(firstTagged) + 1);
                            LONG64 prevValue = CompareExchangeAcqRel(
                                reinterpret_cast<LONG64 volatile*>(&m_head),
                                static_cast<LONG64>(newHeadTagged),
                                static_cast<LONG64>(firstTagged));

                            if (static_cast<uintptr_t>(prevValue) == firstTagged)
                            {
                                outJob = job;
                                m_nodePool.Deallocate(first);  // 옛 dummy(첫 노드) 반납
                                return true;
                            }
                        }
                    }
                    // 충돌 -> 재시도
                }
            }

            // 큐가 비었는지 확인. 호출 직후 다른 스레드가 넣을 수 있어 그 순간의 스냅샷일 뿐.
            bool LockFreeQueue::IsEmpty() const
            {
                // 종료 정리나 모니터링 용도로만 - 정확한 동기화 보장은 아님
                uintptr_t headTagged = LoadAcquire(&m_head);
                Node* head = GetPtr(headTagged);
                return head->next == 0;  // dummy 다음이 없으면 빈 큐
            }

        }
    }
}
