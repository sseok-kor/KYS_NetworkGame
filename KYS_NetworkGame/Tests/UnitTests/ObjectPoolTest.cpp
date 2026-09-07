#include "pch_tests.h"

#include "Core/Memory/ObjectPool.h"
#include "TestHarness/ConcurrentHammer.h"   // FailFastTestHarness

#include <type_traits>

using namespace KYS::GAMESERVER::MEMORY;

// ==========================================================================
// 컴파일 타임 계약: 풀은 자기 메모리 단독 소유 (Rule-of-Five 4줄 =delete)
// ==========================================================================

static_assert(!std::is_copy_constructible<ObjectPool<int>>::value, "풀 복사 생성 금지");
static_assert(!std::is_copy_assignable<ObjectPool<int>>::value,    "풀 복사 대입 금지");
static_assert(!std::is_move_constructible<ObjectPool<int>>::value, "풀 이동 생성 금지");
static_assert(!std::is_move_assignable<ObjectPool<int>>::value,    "풀 이동 대입 금지");

namespace
{
	// 생성 인자 전달 + 생존 수 계측 프로브 (생성 +1 / 소멸 -1).
	struct PoolProbe
	{
		int hp;
		int level;
		static LONG s_liveCount;
		PoolProbe(int hpArg, int levelArg)
			: hp(hpArg), level(levelArg)
		{
			::InterlockedIncrement(&s_liveCount);
		}
		~PoolProbe()
		{
			::InterlockedDecrement(&s_liveCount);
		}
	};
	LONG PoolProbe::s_liveCount = 0;
}

// ==========================================================================
// 정확성 (단일스레드): 생성 -> 고갈 -> 반납 -> 재사용 생명주기 + 방어
// ==========================================================================

// 생성 직후: 용량은 capacity, 사용량은 0
TEST(ObjectPoolCorrectness, StartsEmptyWithFullCapacity)
{
	ObjectPool<int> pool(4);
	EXPECT_EQ(pool.GetCapacity(), 4u);
	EXPECT_EQ(pool.GetUsedCount(), 0u);
}

// capacity개까지만 발급, 그다음은 nullptr, 실패는 사용량을 안 늘림
TEST(ObjectPoolCorrectness, ExhaustsAtCapacity)
{
	ObjectPool<int> pool(3);
	int* a = pool.Allocate(1);
	int* b = pool.Allocate(2);
	int* c = pool.Allocate(3);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	ASSERT_NE(c, nullptr);
	EXPECT_EQ(pool.GetUsedCount(), 3u);

	int* overflow = pool.Allocate(4);   // 빈 슬롯 없음
	EXPECT_EQ(overflow, nullptr);
	EXPECT_EQ(pool.GetUsedCount(), 3u); // 실패는 사용량을 늘리지 않음
}

// 생성 인자가 placement new로 전달되고, 반납 시 소멸자가 정확히 1회 호출됨
TEST(ObjectPoolCorrectness, ForwardsCtorArgsAndRunsDtorOnce)
{
	PoolProbe::s_liveCount = 0;
	ObjectPool<PoolProbe> pool(2);

	PoolProbe* p = pool.Allocate(120, 7);   // hp=120, level=7
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(p->hp, 120);
	EXPECT_EQ(p->level, 7);
	EXPECT_EQ(PoolProbe::s_liveCount, 1);   // 생성 1회

	pool.Deallocate(p);
	EXPECT_EQ(PoolProbe::s_liveCount, 0);   // 소멸 1회 (누수/이중소멸 없음)
	EXPECT_EQ(pool.GetUsedCount(), 0u);
}

// nullptr 반납과 이중 반납은 no-op (magic 가드), 사용량 언더플로 없음
TEST(ObjectPoolCorrectness, DeallocateNullAndDoubleFreeAreNoOps)
{
	ObjectPool<int> pool(2);
	pool.Deallocate(nullptr);               // 무시
	EXPECT_EQ(pool.GetUsedCount(), 0u);

	int* p = pool.Allocate(42);
	ASSERT_NE(p, nullptr);
	pool.Deallocate(p);
	EXPECT_EQ(pool.GetUsedCount(), 0u);

	pool.Deallocate(p);                     // 이중 반납 - magic이 FREE라 무시
	EXPECT_EQ(pool.GetUsedCount(), 0u);     // 언더플로로 거대값이 되지 않음
}

// 가장 최근에 반납한 슬롯이 다음 Allocate에서 먼저 재사용됨 (free list = LIFO)
TEST(ObjectPoolCorrectness, ReusesMostRecentlyFreedSlotLifo)
{
	ObjectPool<int> pool(4);
	int* first = pool.Allocate(1);
	int* second = pool.Allocate(2);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);

	pool.Deallocate(first);
	pool.Deallocate(second);                // second가 free list 맨 앞

	int* reused = pool.Allocate(3);
	EXPECT_EQ(reused, second);              // 마지막 반납 슬롯이 먼저 나옴
}

// 전부 소진 -> 전부 반납 -> 다시 소진 순환에서 슬롯 유실 없음
TEST(ObjectPoolCorrectness, DrainRefillCycleKeepsAllSlots)
{
	const UINT32 cap = 8;
	ObjectPool<int> pool(cap);
	for (int cycle = 0; cycle < 3; ++cycle)
	{
		int* borrowed[cap];
		for (UINT32 i = 0; i < cap; ++i)
		{
			borrowed[i] = pool.Allocate(static_cast<int>(i));
			ASSERT_NE(borrowed[i], nullptr);
		}
		EXPECT_EQ(pool.GetUsedCount(), cap);
		EXPECT_EQ(pool.Allocate(999), nullptr);   // 소진

		for (UINT32 i = 0; i < cap; ++i)
		{
			pool.Deallocate(borrowed[i]);
		}
		EXPECT_EQ(pool.GetUsedCount(), 0u);       // 전부 회수
	}
}

// IsFromThisPool: 발급 포인터=true, 다른 풀/스택변수/nullptr=false
TEST(ObjectPoolCorrectness, IsFromThisPoolDiscriminatesOwnership)
{
	ObjectPool<int> poolA(4);
	ObjectPool<int> poolB(4);

	int* a = poolA.Allocate(1);
	ASSERT_NE(a, nullptr);
	EXPECT_TRUE(poolA.IsFromThisPool(a));    // 이 풀에서 나온 포인터
	EXPECT_FALSE(poolB.IsFromThisPool(a));   // 다른 풀 것 아님
	EXPECT_FALSE(poolA.IsFromThisPool(nullptr));

	int stackVar = 5;
	EXPECT_FALSE(poolA.IsFromThisPool(&stackVar));   // 풀 범위 밖

	poolA.Deallocate(a);
}

// ==========================================================================
// 고경합 스트레스: free-list 무결성 (이중발급 0 / 균형 후 0 / 유실 0)
// ==========================================================================

namespace
{
	struct HammerArgs
	{
		ObjectPool<LONG>* pool;
		volatile LONG* startFlag;        // 0->1 되면 동시 출발
		volatile LONG* arrived;          // 준비된 스레드 수
		LONG myTag;                      // 이 스레드 고유 태그 (>=1)
		int iterations;
		volatile LONG* doubleIssueCount; // 슬롯 이중발급 감지
	};

	// 할당 -> 자기 태그 기록 -> 잠깐 대기 -> 태그 확인 -> 반납 반복.
	// 두 스레드가 같은 슬롯을 동시에 받으면 서로 다른 태그로 덮어써 read-back 불일치 검출.
	unsigned __stdcall HammerProc(void* param)
	{
		HammerArgs* args = reinterpret_cast<HammerArgs*>(param);

		::InterlockedIncrement(args->arrived);
		while (*args->startFlag == 0)
		{
			::YieldProcessor();          // 전 스레드 준비까지 대기 (경합 극대화)
		}

		for (int i = 0; i < args->iterations; ++i)
		{
			LONG* slot = args->pool->Allocate(args->myTag);
			if (slot == nullptr)
			{
				continue;                // 순간 소진 - 정상 (다른 스레드가 다 빌려간 상태)
			}

			*slot = args->myTag;         // 내 태그 기록
			for (int spin = 0; spin < 32; ++spin)
			{
				::YieldProcessor();      // 다른 스레드가 같은 슬롯을 만질 창을 벌림
			}
			if (*slot != args->myTag)
			{
				::InterlockedIncrement(args->doubleIssueCount);   // 이중발급 = 태그 오염
			}

			args->pool->Deallocate(slot);
		}
		return 0;
	}
}

// 고경합 free list 무결성: 이중발급 0 + 균형 후 usedCount 0 + 슬롯 유실 0
TEST(ObjectPoolStress, FreeListSurvivesConcurrentAllocFree)
{
	const UINT32 cap = 256;
	const int threadCount = 8;
	const int iterations = 20000;

	ObjectPool<LONG> pool(cap);
	volatile LONG startFlag = 0;
	volatile LONG arrived = 0;
	volatile LONG doubleIssueCount = 0;

	HANDLE handles[threadCount];
	HammerArgs args[threadCount];
	for (int t = 0; t < threadCount; ++t)
	{
		args[t].pool = &pool;
		args[t].startFlag = &startFlag;
		args[t].arrived = &arrived;
		args[t].myTag = static_cast<LONG>(t + 1);   // 0은 안 씀
		args[t].iterations = iterations;
		args[t].doubleIssueCount = &doubleIssueCount;
		handles[t] = reinterpret_cast<HANDLE>(
			::_beginthreadex(nullptr, 0, HammerProc, &args[t], 0, nullptr));
		ASSERT_NE(handles[t], nullptr);
	}

	while (arrived < threadCount)
	{
		::YieldProcessor();
	}
	::InterlockedExchange(&startFlag, 1);            // 동시 출발

	DWORD wait = ::WaitForMultipleObjects(threadCount, handles, TRUE, 30000);
	if (wait != WAIT_OBJECT_0)
	{
		// 스레드들이 지역 args/pool 을 계속 참조하므로 반환하면 UB -> 즉시 실패 종료.
		KYS::TESTS::FailFastTestHarness("ObjectPool free-list stress: threads hung", 0);
	}

	for (int t = 0; t < threadCount; ++t)
	{
		::CloseHandle(handles[t]);
	}

	LONG doubleIssues = doubleIssueCount;            // join 후 단일 스레드 읽기
	EXPECT_EQ(doubleIssues, 0) << "슬롯 이중발급 발생";
	EXPECT_EQ(pool.GetUsedCount(), 0u) << "균형 후 사용량 0 아님 (유실/누수)";

	// 유실 검증: 전 슬롯을 다시 drain -> capacity개 전부 non-null이어야 함
	LONG* drained[cap];
	UINT32 got = 0;
	for (UINT32 i = 0; i < cap; ++i)
	{
		drained[i] = pool.Allocate(0L);
		if (drained[i] != nullptr) ++got;
	}
	EXPECT_EQ(got, cap) << "drain 재고가 capacity 미만 (슬롯 유실)";
	for (UINT32 i = 0; i < cap; ++i)
	{
		pool.Deallocate(drained[i]);
	}
}

namespace
{
	struct ExhaustArgs
	{
		ObjectPool<int>* pool;
		volatile LONG* startFlag;
		volatile LONG* arrived;
		int* result;    // 이 스레드의 단 1회 Allocate 결과 저장
	};
	unsigned __stdcall ExhaustProc(void* param)
	{
		ExhaustArgs* a = reinterpret_cast<ExhaustArgs*>(param);
		::InterlockedIncrement(a->arrived);
		while (*a->startFlag == 0) ::YieldProcessor();
		a->result = a->pool->Allocate(1);   // 각 스레드 1회만
		return 0;
	}
}

// 빈 리스트 경계 경합: capacity개만 성공, 초과 스레드는 정확히 nullptr (이중발급 0)
TEST(ObjectPoolStress, ExhaustionRaceIssuesExactlyCapacity)
{
	const UINT32 cap = 32;
	const int threadCount = 64;   // capacity의 2배

	ObjectPool<int> pool(cap);
	volatile LONG startFlag = 0;
	volatile LONG arrived = 0;

	HANDLE handles[threadCount];
	ExhaustArgs args[threadCount];
	for (int t = 0; t < threadCount; ++t)
	{
		args[t].pool = &pool;
		args[t].startFlag = &startFlag;
		args[t].arrived = &arrived;
		args[t].result = nullptr;
		handles[t] = reinterpret_cast<HANDLE>(
			::_beginthreadex(nullptr, 0, ExhaustProc, &args[t], 0, nullptr));
		ASSERT_NE(handles[t], nullptr);
	}

	while (arrived < threadCount) ::YieldProcessor();
	::InterlockedExchange(&startFlag, 1);

	DWORD wait = ::WaitForMultipleObjects(threadCount, handles, TRUE, 30000);
	if (wait != WAIT_OBJECT_0)
	{
		KYS::TESTS::FailFastTestHarness("ObjectPool exhaustion race: threads hung", 0);
	}
	for (int t = 0; t < threadCount; ++t) ::CloseHandle(handles[t]);

	// 성공 횟수만 세면 "이중발급 + 유실"이 상쇄돼 숨는다 -> 포인터 유일성까지 검사.
	std::unordered_set<int*> uniqueSlots;
	int nonNull = 0;
	for (int t = 0; t < threadCount; ++t)
	{
		if (args[t].result != nullptr)
		{
			++nonNull;
			EXPECT_TRUE(uniqueSlots.insert(args[t].result).second)
				<< "슬롯 이중발급: 두 스레드가 같은 포인터를 받음";
		}
	}
	EXPECT_EQ(nonNull, static_cast<int>(cap)) << "빈 리스트 경계에서 발급 수 != capacity";
	EXPECT_EQ(pool.GetUsedCount(), cap);
}

// ==========================================================================
// 엣지, 설계검증: cross-pool 게이팅 / GetUsedCount stale-tolerant
// ==========================================================================

// cross-pool 반납은 RELEASE에서 free list를 손상시킨다 (owner 가드가 _DEBUG 전용).
// 따라서 여러 풀이 있으면 IsFromThisPool로 소유 풀을 찾아 그 풀에만 반납해야 한다.
TEST(ObjectPoolEdge, CrossPoolDeallocateMustBeGatedByOwnership)
{
	ObjectPool<int> poolA(4);
	ObjectPool<int> poolB(4);

	int* p = poolA.Allocate(7);
	ASSERT_NE(p, nullptr);

	// 올바른 라우팅: 소유 풀을 찾아 그 풀에만 반납
	ObjectPool<int>* owner = poolA.IsFromThisPool(p) ? &poolA
	                      : (poolB.IsFromThisPool(p) ? &poolB : nullptr);
	ASSERT_EQ(owner, &poolA);
	owner->Deallocate(p);
	EXPECT_EQ(poolA.GetUsedCount(), 0u);
	EXPECT_EQ(poolB.GetUsedCount(), 0u);

	// 주의: poolB.Deallocate(p)는 RELEASE에서 poolB free list를 오염시킨다 -> 절대 호출 금지.
	//       _DEBUG 빌드에서만 owner!=this 가드가 no-op으로 막아준다 (Release는 UB).
}

// GetUsedCount는 단일스레드에서만 정확값 assert (다중스레드 중간값은 stale-tolerant)
TEST(ObjectPoolEdge, GetUsedCountExactOnlyUnderSingleThread)
{
	ObjectPool<int> pool(4);
	int* a = pool.Allocate(1);
	int* b = pool.Allocate(2);
	EXPECT_EQ(pool.GetUsedCount(), 2u);   // 단일스레드라 정확
	pool.Deallocate(a);
	pool.Deallocate(b);
	EXPECT_EQ(pool.GetUsedCount(), 0u);
	// 다중스레드에서 op 도중 GetUsedCount는 무락 읽기라 stale -> 중간값 assert 금지 (스트레스는 join 후만 assert).
}
