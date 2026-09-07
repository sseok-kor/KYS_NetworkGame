#include "pch_gameserver.h"
#include "TwoLockJobQueue.h"

namespace KYS
{
    namespace GAMESERVER
    {
        namespace THREAD
        {
            // 빈 큐 초기화. head/tail 둘 다 빈 표시용(dummy) 노드를 가리키게 함.
            //   nodePoolCapacity : 미리 잡아둘 노드 개수
            //   policy           : 풀이 바닥났을 때 버릴지(DROP) 늘릴지(GROW)
            TwoLockJobQueue::TwoLockJobQueue(UINT32 nodePoolCapacity, EOverflowPolicy policy)
                : m_head(nullptr)
                , m_tail(nullptr)
                , m_nodePool(nodePoolCapacity)
                , m_policy(policy)
            {
                // 빈 표시용 노드 하나로 head/tail 시작 - 빈 큐에서도 실데이터 노드를 안 건드리게
                Node* dummy = m_nodePool.Allocate();
                dummy->job = nullptr;
                dummy->next = nullptr;
                dummy->fromHeap = false;
                m_head = dummy;
                m_tail = dummy;
            }

            // 큐 정리. 노드만 반납한다 (남은 작업의 수명은 호출자 책임 - shutdown 전에 미리 비워야 함).
            TwoLockJobQueue::~TwoLockJobQueue()
            {
                // dummy부터 끝까지 노드를 따라가며 반납
                Node* cur = m_head;   // dummy부터
                while (cur != nullptr)
                {
                    Node* next = cur->next;
                    if (cur->fromHeap)
                    {
                        delete cur;                  // heap 노드 - 풀에 넣지 않고 직접 삭제
                    }
                    else
                    {
                        m_nodePool.Deallocate(cur);  // 풀 노드 - 풀로 반납
                    }
                    cur = next;
                }
            }

            // 작업 하나를 큐 끝에 매단다. 여러 워커가 동시에 호출 가능.
            //   job : 큐에 넣을 작업
            bool TwoLockJobQueue::Enqueue(IJob* job)
            {
                // 담을 노드 확보 시작 (풀 소진 시 정책대로 처리)
                Node* newNode = m_nodePool.Allocate();   // 풀에서 노드 하나 (락 없이)
                if (newNode == nullptr)                  // 풀 바닥: 그냥 쓰면 널 접근 크래시라 정책대로 처리
                {
                    if (m_policy == EOverflowPolicy::GROW)
                    {
                        newNode = new Node();            // heap에서 새로 만듦 - 세션 이벤트 큐는 절대 안 버림
                        newNode->fromHeap = true;        // 반납 때 delete 경로로 가도록 표시
                    }
                    else
                    {
                        delete job;                      // DROP 정책(데이터 큐): 작업 버림
                        return false;                    // 호출자가 drop 계측 가능 (movement lane shed 카운트)
                    }// 큐에 안 들어갔으니 소비자가 delete할 일 없음 - 이중 해제 안 생김
                }
                else
                {
                    newNode->fromHeap = false;           // 풀 노드
                }
                newNode->job = job;
                newNode->next = nullptr;

                // tail에 매다는 구간 시작 (생산자들끼리만 직렬화)
                SpinLockGuard guard(m_tailLock);         // 워커들 순서 맞춤 + 잠금 해제 시 메모리 반영
                m_tail->next = newNode;                  // 현재 끝(tail)에 연결
                m_tail = newNode;                        // tail 전진
                // head는 안 건드림 (dummy 노드 덕에 소비자와 안 부딪힘)
                return true;                             // 적재 성공
            }

            // 큐 앞에서 작업 하나 꺼냄. 비어 있으면 false. 소비자는 한 스레드뿐.
            //   outJob : 꺼낸 작업을 담아 돌려줄 자리
            bool TwoLockJobQueue::Dequeue(IJob*& outJob)
            {
                Node* oldHead = nullptr;   // 옛 dummy - 락 푼 뒤 반납하려고 잠시 들고 있음
                // head를 옮기는 구간 시작 (소비자 한 명이라 사실상 무경합)
                {
                    SpinLockGuard guard(m_headLock);   // 잠금 시 메모리 최신값 읽기 보장
                    oldHead = m_head;                  // 현재 dummy
                    Node* first = oldHead->next;       // 첫 실데이터 노드
                    if (first == nullptr)              // dummy만 있음 = 빈 큐
                    {
                        outJob = nullptr;
                        return false;
                    }
                    outJob = first->job;               // first가 이제 새 dummy가 됨
                    m_head = first;                    // head 전진
                }                                      // 여기서 잠금 풀림

                // 락 밖에서 옛 dummy 노드 반납
                if (oldHead->fromHeap)
                {
                    delete oldHead;                      // heap 노드 - 직접 삭제
                }
                else
                {
                    m_nodePool.Deallocate(oldHead);      // 풀 노드 - 풀로 반납
                }

                return true;
            }

            // 큐가 비었는지 확인. 호출 직후 생산자가 넣을 수 있으니 그 순간 스냅샷일 뿐.
            bool TwoLockJobQueue::IsEmpty() const
            {
                SpinLockGuard guard(m_headLock);
                return m_head->next == nullptr;        // dummy 다음이 없으면 빈 큐
            }
        }
    }
}
