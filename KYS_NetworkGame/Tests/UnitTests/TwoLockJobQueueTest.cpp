#include "pch_tests.h"

#include "Core/Thread/TwoLockJobQueue.h"
#include "TestHarness/ConcurrentHammer.h"
#include "TestHarness/JobProbe.h"

#include <type_traits>

using KYS::GAMESERVER::THREAD::IJob;
using KYS::GAMESERVER::THREAD::TwoLockJobQueue;
using KYS::GAMESERVER::THREAD::EOverflowPolicy;
using KYS::TESTS::JobProbe;
using KYS::TESTS::ProbeJob;
using KYS::TESTS::ResetProbe;

// [!] 라이브 recv/job 경로의 메일박스(Channel m_mailbox/m_moveMailbox)가 바로 이 큐다.
//     MPSC 계약: 넣기는 여러 워커(tail 락), 빼기는 채널 스레드 하나(head 락).

// ==========================================================================
// 컴파일 타임 계약: Rule-of-Five 4줄 =delete
// ==========================================================================

static_assert(!std::is_copy_constructible<TwoLockJobQueue>::value, "큐 복사 생성 금지");
static_assert(!std::is_copy_assignable<TwoLockJobQueue>::value,    "큐 복사 대입 금지");
static_assert(!std::is_move_constructible<TwoLockJobQueue>::value, "큐 이동 생성 금지");
static_assert(!std::is_move_assignable<TwoLockJobQueue>::value,    "큐 이동 대입 금지");

// ==========================================================================
// 엣지: DROP 정책 - 초과 job은 큐가 내부에서 정확히 1회 delete + false 반환
// ==========================================================================

TEST(TwoLockJobQueueEdge, DropPolicyDeletesOverflowJobExactlyOnce)
{
	JobProbe probe;
	LONG seen[2];
	ResetProbe(probe, seen, 2);

	TwoLockJobQueue q(2, EOverflowPolicy::DROP);   // dummy 1 + 여유 1

	EXPECT_TRUE(q.Enqueue(new ProbeJob(0, &probe)));   // 여유 사용
	IJob* extra = new ProbeJob(1, &probe);
	EXPECT_FALSE(q.Enqueue(extra));                    // 풀 고갈 -> DROP
	LONG destroyedAfterDrop = probe.destroyCount;
	EXPECT_EQ(1, destroyedAfterDrop);                  // 버린 job만 delete(호출자 delete 금지)

	IJob* out = nullptr;
	ASSERT_TRUE(q.Dequeue(out));
	EXPECT_EQ(0, static_cast<ProbeJob*>(out)->GetId());
	out->Execute();
	delete out;

	EXPECT_FALSE(q.Dequeue(out));                      // #1은 애초에 안 들어감
	EXPECT_EQ(nullptr, out);
	LONG destroyedFinal = probe.destroyCount;
	EXPECT_EQ(2, destroyedFinal);                      // #0까지 소멸
}

// ==========================================================================
// 엣지: GROW 정책 - 풀을 넘겨도 전부 적재(heap 성장), 전부 회수, 누수 0
// ==========================================================================

TEST(TwoLockJobQueueEdge, GrowPolicyRetainsAllNoLoss)
{
	const int N = 500;   // 풀(=2) 훨씬 초과 -> 대부분 heap 노드
	JobProbe probe;
	std::vector<LONG> seen(N);
	ResetProbe(probe, seen.data(), N);

	TwoLockJobQueue q(2, EOverflowPolicy::GROW);
	for (int i = 0; i < N; ++i)
	{
		EXPECT_TRUE(q.Enqueue(new ProbeJob(i, &probe)));   // 절대 버리지 않음
	}

	int count = 0;
	IJob* out = nullptr;
	while (q.Dequeue(out)) { out->Execute(); delete out; ++count; }

	LONG executed = probe.executeCount;
	LONG destroyed = probe.destroyCount;
	EXPECT_EQ(N, count);
	EXPECT_EQ(N, executed);
	EXPECT_EQ(N, destroyed);                               // heap 노드/Job 회수 정확
	for (int i = 0; i < N; ++i) EXPECT_EQ(1, probe.seen[i]);
}

// ==========================================================================
// 정확성: fromHeap 혼합(풀+heap 노드 혼재) Dequeue/소멸 경로 + 빈 큐
// ==========================================================================

// 풀 노드 + heap 노드 혼재 상태에서 Dequeue 경로/소멸자 경로가 fromHeap 분기를 모두 타도 크래시 0.
TEST(TwoLockJobQueueCorrectness, MixedHeapAndPoolNodes)
{
	const int N = 6;
	JobProbe probe;
	LONG seen[6];
	ResetProbe(probe, seen, N);

	{
		TwoLockJobQueue q(3, EOverflowPolicy::GROW);   // dummy 1 + 풀 2 + 나머지 heap
		for (int i = 0; i < N; ++i) q.Enqueue(new ProbeJob(i, &probe));

		IJob* out = nullptr;
		for (int i = 0; i < 2; ++i)                    // 일부만 Dequeue(풀/heap 섞여 나옴)
		{
			ASSERT_TRUE(q.Dequeue(out));
			out->Execute();
			delete out;
		}
		// 소멸 전 잔여 Job은 호출자 drain 의무(큐는 Job delete 안 함).
		while (q.Dequeue(out)) { out->Execute(); delete out; }
	}   // 큐 소멸 -> 남은 노드(풀+heap 혼합)를 fromHeap 분기로 정확 반환

	LONG executed = probe.executeCount;
	LONG destroyed = probe.destroyCount;
	EXPECT_EQ(N, executed);
	EXPECT_EQ(N, destroyed);
}

// 빈 큐 Dequeue.
TEST(TwoLockJobQueueCorrectness, EmptyDequeueReturnsFalse)
{
	TwoLockJobQueue q;
	IJob* out = reinterpret_cast<IJob*>(static_cast<uintptr_t>(0xDEAD));
	EXPECT_FALSE(q.Dequeue(out));
	EXPECT_EQ(nullptr, out);
	EXPECT_TRUE(q.IsEmpty());
}

// ==========================================================================
// 스트레스: 다른 락 같은 워드 경계 (다생산자 + 단소비자 = MPSC 계약)
//   dummy->next는 생산자(tail 락 아래 write)와 소비자(head 락 아래 read)가
//   다른 락으로 접근하는 유일한 공유 워드 - 빈 큐 경계에서만 진짜 교차한다.
// ==========================================================================

namespace
{
	struct MpscCtx
	{
		TwoLockJobQueue* q;
		JobProbe*        probe;
		volatile LONG    consumed;
		LONG             total;         // 소비 목표(고정 상수) - 이동 목표 race 회피
	};

	// MPSC 계약: 소비자는 단 하나(이 프로시저를 도는 스레드 1개만).
	unsigned __stdcall MpscConsumerProc(void* raw)
	{
		MpscCtx* c = static_cast<MpscCtx*>(raw);
		// 종료 = 총량(고정 상수)을 다 소비할 때까지. produced 같은 이동 목표를 안 봐서 race 창 0.
		while (::InterlockedCompareExchange(&c->consumed, 0, 0) < c->total)
		{
			IJob* out = nullptr;
			if (c->q->Dequeue(out))
			{
				out->Execute();
				delete out;
				::InterlockedIncrement(&c->consumed);
			}
			else
			{
				::YieldProcessor();
			}
		}
		return 0;
	}
}

TEST(TwoLockJobQueueStress, BoundaryConcurrentMpsc)
{
	const DWORD  P = 8;
	const UINT32 K = 20000;
	const UINT32 TOTAL = P * K;

	JobProbe probe;
	std::vector<LONG> seen(TOTAL);
	ResetProbe(probe, seen.data(), TOTAL);

	TwoLockJobQueue q(1024, EOverflowPolicy::GROW);
	MpscCtx ctx{ &q, &probe, 0, static_cast<LONG>(TOTAL) };   // total = 소비 목표(고정)

	// 단일 소비자를 먼저 띄우고(빈 큐 경계에서 대기), 생산자 P개를 배리어 동시 출발.
	HANDLE consumer = reinterpret_cast<HANDLE>(
		::_beginthreadex(nullptr, 0, MpscConsumerProc, &ctx, 0, nullptr));
	ASSERT_NE(consumer, nullptr);

	auto producer = [&](DWORD threadIndex, UINT32 m, UINT64)
	{
		int id = static_cast<int>(threadIndex * K + m);
		q.Enqueue(new ProbeJob(id, &probe));   // GROW라 유실 없음. 종료는 consumed==total 로만 판정
	};

	KYS::TESTS::HammerResult r =
		KYS::TESTS::ConcurrentHammer::Run(P, K, producer);
	ASSERT_FALSE(r.timedOut) << "producers hung (seed=" << r.seed << " P=" << P << " K=" << K << ")";

	DWORD wait = ::WaitForSingleObject(consumer, 30000);
	if (wait != WAIT_OBJECT_0)
	{
		// 소비자가 지역 큐/컨텍스트를 계속 참조하므로 반환하면 UB -> 즉시 실패 종료.
		KYS::TESTS::FailFastTestHarness("TwoLockJobQueue MPSC: consumer hung", r.seed);
	}
	::CloseHandle(consumer);

	LONG executed = probe.executeCount;
	LONG destroyed = probe.destroyCount;
	EXPECT_EQ(static_cast<LONG>(TOTAL), executed);
	EXPECT_EQ(static_cast<LONG>(TOTAL), destroyed);
	for (UINT32 i = 0; i < TOTAL; ++i)
	{
		ASSERT_EQ(1, probe.seen[i]) << "id " << i << " 유실/중복(경계 torn 의심)";
	}
}

// [!] 소비자가 둘이면 이 큐는 계약 위반이다 - two-lock 큐는 MPSC이지 MPMC가 아니다.
//     그런 테스트는 작성하지 않는다(부재가 의도된 경계).
