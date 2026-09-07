#include "pch_tests.h"

#include "Core/Thread/MemoryOrder.h"
#include "TestHarness/ConcurrentHammer.h"

#include <climits>

using namespace KYS::GAMESERVER::THREAD;

// ==========================================================================
// 정확성 (단일스레드): CAS 계약 / Exchange / Load-Store 값 충실성
//   비고: 단일 스레드 검증이라 대상 변수를 비-volatile 로 두고
//         volatile* 파라미터로의 암시 변환에 맡긴다 (EXPECT_EQ 참조 바인딩 문제 회피).
// ==========================================================================

TEST(MemoryOrderCorrectness, CompareExchangeContract)
{
	LONG64 v = 100;
	// 일치: comparand==현재값 -> exchange 로 교체 + 직전 값 반환
	EXPECT_EQ(CompareExchangeAcqRel(&v, 200, 100), 100);
	EXPECT_EQ(v, 200);
	// 불일치: comparand!=현재값 -> 불변 + 현재값 반환
	EXPECT_EQ(CompareExchangeAcqRel(&v, 999, 111), 200);
	EXPECT_EQ(v, 200);
}

TEST(MemoryOrderCorrectness, ExchangeReturnsPreviousValue)
{
	LONG64 v = 7;
	EXPECT_EQ(ExchangeAcqRel(&v, 42), 7);   // 반환 = 이전 값
	EXPECT_EQ(v, 42);                        // dest = 신규 값
}

TEST(MemoryOrderCorrectness, LoadStoreValueFidelity)
{
	LONG64 v = 0;
	StoreRelease<LONG64>(&v, -123);          // 음수
	EXPECT_EQ(LoadAcquire<LONG64>(&v), -123);
	StoreRelease<LONG64>(&v, 0);             // 0
	EXPECT_EQ(LoadAcquire<LONG64>(&v), 0);
	StoreRelease<LONG64>(&v, LLONG_MAX);     // 최대
	EXPECT_EQ(LoadAcquire<LONG64>(&v), LLONG_MAX);
}

TEST(MemoryOrderCorrectness, LoadStorePointerInstantiation)
{
	// 템플릿을 포인터 타입으로도 인스턴스화해 커버
	int  target = 5;
	int* p      = nullptr;
	StoreRelease<int*>(&p, &target);
	EXPECT_EQ(LoadAcquire<int*>(&p), &target);
}

// ==========================================================================
// 고경합 스트레스: CAS-increment 원자성 (K x M, lost update 0)
// ==========================================================================

TEST(MemoryOrderStress, CasIncrementIsAtomic)
{
	volatile LONG64 shared = 0;

	const DWORD  kThreads   = 16;
	const UINT32 kPerThread = 100000;

	auto work = [&](DWORD, UINT32, UINT64)
	{
		for (;;)
		{
			LONG64 current = LoadAcquire(&shared);
			LONG64 next    = current + 1;
			// 내가 본 값이 그대로였으면 교체 성공, 아니면 재시도(lock-free 낙관적 재시도)
			if (CompareExchangeAcqRel(&shared, next, current) == current)
			{
				break;
			}
		}
	};

	KYS::TESTS::HammerResult r =
		KYS::TESTS::ConcurrentHammer::Run(kThreads, kPerThread, work);

	ASSERT_FALSE(r.timedOut) << "hang-timeout 초과 (seed=" << r.seed << ")";
	LONG64 finalValue = LoadAcquire(&shared);   // join 후 최종값 읽기
	EXPECT_EQ(finalValue, static_cast<LONG64>(kThreads) * kPerThread)
		<< "CAS-increment 유실: 원자성 실패";
}

// ==========================================================================
// 엣지, 한계: 메시지 패싱 (x64/TSO 한계 명시)
// ==========================================================================

// 메시지 패싱: producer 가 payload 를 채운 뒤 ready 를 StoreRelease.
// consumer 는 ready 를 LoadAcquire 로 보고 나서 payload 를 읽는다.
//
// [!] 이 테스트가 증명하는 것: (1) 값 충실성 (2) 원자적 관측.
// [!] 이 테스트가 증명하지 못하는 것: acquire/release 의 "순서 강제".
//     x64 는 TSO 라 여기 필요한 재배치(Store-Store / Load-Load)를 하드웨어가 애초에 안 한다.
//     즉 래퍼가 순서를 강제하지 않아도 x64 에선 mismatch 가 안 뜬다.
//     순서 계약의 진짜 검증은 ARM64(약한 순서) 또는 인터리빙 전수 탐색(모델 체커)이 필요하다.
TEST(MemoryOrderEdge, MessagePassingValueFidelity_x64OrderingLimitDocumented)
{
	volatile LONG payload  = 0;
	volatile LONG ready    = 0;
	volatile LONG mismatch = 0;

	const DWORD kConsumers = 4;

	auto work = [&](DWORD threadIndex, UINT32, UINT64)
	{
		if (threadIndex == 0)
		{
			payload = 42;
			StoreRelease(&ready, 1L);                 // 신호를 릴리즈로 발행
		}
		else
		{
			while (LoadAcquire(&ready) == 0)          // 신호를 어콰이어로 대기
			{
				::YieldProcessor();
			}
			if (payload != 42)                        // 신호를 봤으면 payload 는 완성돼 있어야
			{
				::InterlockedExchange(&mismatch, 1);
			}
		}
	};

	KYS::TESTS::HammerResult r =
		KYS::TESTS::ConcurrentHammer::Run(kConsumers + 1, 1, work);

	ASSERT_FALSE(r.timedOut) << "hang-timeout 초과 (seed=" << r.seed << ")";
	LONG mismatchObserved = mismatch;   // join 후 단일 스레드 읽기
	EXPECT_EQ(mismatchObserved, 0) << "consumer 가 payload 완성 전에 신호를 관측(값/원자성 문제)";
}
