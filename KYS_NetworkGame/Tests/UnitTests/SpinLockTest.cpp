#include "pch_tests.h"

#include "Core/Thread/SpinLock.h"
#include "TestHarness/ConcurrentHammer.h"

#include <type_traits>

using namespace KYS::GAMESERVER::THREAD;

// ==========================================================================
// 컴파일 타임 계약: 깨지면 빌드 실패(런타임 비용 0)
// ==========================================================================

// SpinLock: 복사 2줄 =delete -> 이동은 암시적 미선언으로 함께 차단(4개 전부 false)
static_assert(!std::is_copy_constructible<SpinLock>::value, "SpinLock 복사 생성 금지");
static_assert(!std::is_copy_assignable<SpinLock>::value,    "SpinLock 복사 대입 금지");
static_assert(!std::is_move_constructible<SpinLock>::value, "SpinLock 이동 생성 금지(복사 삭제로 폴백 차단)");
static_assert(!std::is_move_assignable<SpinLock>::value,    "SpinLock 이동 대입 금지(복사 삭제로 폴백 차단)");

// SpinLockGuard: 복사/이동 금지 + SpinLock& 로만 생성 + explicit(암시 변환 불가)
static_assert(!std::is_copy_constructible<SpinLockGuard>::value, "Guard 복사 금지");
static_assert(!std::is_move_constructible<SpinLockGuard>::value, "Guard 이동 금지");
static_assert(std::is_constructible<SpinLockGuard, SpinLock&>::value, "Guard 는 SpinLock& 로 생성 가능해야");
static_assert(!std::is_convertible<SpinLock&, SpinLockGuard>::value, "Guard 생성자는 explicit(암시 변환 불가)");

// ---- 위 계약을 Test Explorer 에서도 보이게 미러 ----
TEST(SpinLockContract, RuleOfFiveTwoLineAndExplicit)
{
	// 복사 2줄이 이동까지 막는다는 [class.copy.ctor] 규칙의 런타임 가시 증거
	EXPECT_FALSE(std::is_copy_constructible<SpinLock>::value);
	EXPECT_FALSE(std::is_move_constructible<SpinLock>::value);
	EXPECT_FALSE(std::is_copy_constructible<SpinLockGuard>::value);
	EXPECT_FALSE(std::is_move_constructible<SpinLockGuard>::value);
	EXPECT_TRUE((std::is_constructible<SpinLockGuard, SpinLock&>::value));
	EXPECT_FALSE((std::is_convertible<SpinLock&, SpinLockGuard>::value));
}

// ==========================================================================
// 단일스레드 정확성: TryLock 상태기계 / Lock 라운드트립 / Guard RAII
// ==========================================================================

TEST(SpinLockCorrectness, TryLockStateMachine)
{
	SpinLock lock;
	EXPECT_TRUE(lock.TryLock());    // free -> 잡힘(직전 0)
	EXPECT_FALSE(lock.TryLock());   // 이미 잡힘 -> 실패(직전 1)
	lock.UnLock();
	EXPECT_TRUE(lock.TryLock());    // 풀린 뒤 -> 다시 잡힘
	lock.UnLock();                  // [!] 잠긴 채 소멸하지 않게 반드시 해제
}

TEST(SpinLockCorrectness, LockUnlockRoundTrip)
{
	SpinLock lock;
	lock.Lock();                    // 비경합이라 즉시 획득
	EXPECT_FALSE(lock.TryLock());   // 보유 중 -> 재획득 실패
	lock.UnLock();
	EXPECT_TRUE(lock.TryLock());    // 해제 후 획득 가능
	lock.UnLock();
}

TEST(SpinLockCorrectness, GuardLocksInScopeAndReleasesAfter)
{
	SpinLock lock;
	{
		SpinLockGuard guard(lock);      // 진입과 동시에 Lock
		EXPECT_FALSE(lock.TryLock());   // 스코프 안: 이미 잡혀 실패
	}                                   // 스코프 종료 -> 자동 UnLock
	EXPECT_TRUE(lock.TryLock());        // 스코프 밖: 풀려서 성공
	lock.UnLock();
}

// ==========================================================================
// 고경합 스트레스: 상호배제 (비원자 카운터 K x M)
// ==========================================================================

TEST(SpinLockStress, MutualExclusionProtectsNonAtomicCounter)
{
	SpinLock  lock;
	long long protectedCounter = 0;   // [!] 반드시 비원자. 락이 진짜 배타를 주는지 검증할 대상.
	long      insideNow        = 0;   // 지금 임계구역 안에 있는 스레드 수(락 아래라 plain OK)
	long      maxInsideAtOnce  = 0;   // 관측된 최대 동시 진입(락 정상이면 1)

	const DWORD  kThreads   = 16;
	const UINT32 kPerThread = 100000;

	auto work = [&](DWORD, UINT32, UINT64)
	{
		SpinLockGuard guard(lock);            // 임계구역 진입
		++insideNow;
		if (insideNow > maxInsideAtOnce)
		{
			maxInsideAtOnce = insideNow;
		}
		++protectedCounter;                   // 비원자 증가 - 락이 없으면 lost update
		--insideNow;
	};                                        // 스코프 끝 -> 자동 UnLock

	KYS::TESTS::HammerResult r =
		KYS::TESTS::ConcurrentHammer::Run(kThreads, kPerThread, work);

	ASSERT_FALSE(r.timedOut) << "hang-timeout 초과: 데드락 의심 (seed=" << r.seed << ")";
	EXPECT_EQ(protectedCounter,
	          static_cast<long long>(kThreads) * kPerThread) << "lost update: 상호배제 실패";
	EXPECT_EQ(maxInsideAtOnce, 1) << "임계구역에 2개 이상 동시 진입: 배타 위반";
}

// ==========================================================================
// 엣지: non-recursive 관측 / 소멸 불변식 happy-path
// ==========================================================================

TEST(SpinLockEdge, NonRecursiveReLockObservedAsFalse)
{
	SpinLock lock;
	lock.Lock();
	// 같은 스레드가 다시 시도해도 실패 - 재진입 불가를 관측
	EXPECT_FALSE(lock.TryLock());
	lock.UnLock();
	// [!] 같은 스레드가 lock.Lock() 을 재호출하면 자기 자신을 무한 대기한다.
	//     실행하면 테스트가 hang 하므로 실행하지 않는다(non-recursive 문서 경계).
}

TEST(SpinLockEdge, DestructionHappyPathHoldsInvariant)
{
	// Guard 가 잡았다 스코프 끝에서 풀고 나면, SpinLock 소멸 시 m_lock==0 이라
	// 잠긴 채 소멸이 아니어서 안전하다(happy-path 불변식).
	{
		SpinLock      lock;
		{
			SpinLockGuard guard(lock);
		}                              // 자동 UnLock -> m_lock==0
	}                                  // 여기서 SpinLock 소멸
	SUCCEED();
	// [!] 잠긴 채로 소멸하는 케이스는 Debug=abort / Release=UB 라 테스트로 실행하지 않는다.
}
