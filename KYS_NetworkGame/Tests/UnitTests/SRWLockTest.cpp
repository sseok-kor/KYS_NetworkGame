#include "pch_tests.h"

#include "Core/Thread/SRWLockWrapper.h"
#include "TestHarness/ConcurrentHammer.h"

#include <type_traits>

using namespace KYS::GAMESERVER::THREAD;

// ==========================================================================
// 컴파일 타임 계약: Rule-of-Five + explicit + Read/Write 타입 분리
// ==========================================================================

// ---- SRWLockWrapper 본체: 복사 2줄 -> 이동 폴백 차단 ----
static_assert(!std::is_copy_constructible<SRWLockWrapper>::value, "Wrapper 복사 생성 금지");
static_assert(!std::is_copy_assignable<SRWLockWrapper>::value,    "Wrapper 복사 대입 금지");
static_assert(!std::is_move_constructible<SRWLockWrapper>::value, "Wrapper 이동 생성 금지");
static_assert(!std::is_move_assignable<SRWLockWrapper>::value,    "Wrapper 이동 대입 금지");

// ---- 두 가드: 복사/이동 금지 + explicit ----
static_assert(!std::is_copy_constructible<SRWReadGuard>::value,  "ReadGuard 복사 금지");
static_assert(!std::is_move_constructible<SRWReadGuard>::value,  "ReadGuard 이동 금지");
static_assert(!std::is_copy_constructible<SRWWriteGuard>::value, "WriteGuard 복사 금지");
static_assert(!std::is_move_constructible<SRWWriteGuard>::value, "WriteGuard 이동 금지");
static_assert(std::is_constructible<SRWReadGuard,  SRWLockWrapper&>::value,  "ReadGuard 생성 가능");
static_assert(std::is_constructible<SRWWriteGuard, SRWLockWrapper&>::value,  "WriteGuard 생성 가능");
static_assert(!std::is_convertible<SRWLockWrapper&, SRWReadGuard>::value,   "ReadGuard explicit");
static_assert(!std::is_convertible<SRWLockWrapper&, SRWWriteGuard>::value,  "WriteGuard explicit");

// ---- 타입 분리: Read/Write 가드는 서로 대체 불가 ----
static_assert(!std::is_same<SRWReadGuard, SRWWriteGuard>::value, "Read/Write 는 별개 타입");
static_assert(!std::is_convertible<SRWWriteGuard&, SRWReadGuard&>::value, "WriteGuard 를 ReadGuard 자리에 넘길 수 없음");
static_assert(!std::is_convertible<SRWReadGuard&, SRWWriteGuard&>::value, "ReadGuard 를 WriteGuard 자리에 넘길 수 없음");

TEST(SRWLockContract, RuleOfFiveExplicitAndTypeSeparation)
{
	EXPECT_FALSE(std::is_move_constructible<SRWLockWrapper>::value);
	EXPECT_FALSE(std::is_copy_constructible<SRWReadGuard>::value);
	EXPECT_FALSE(std::is_copy_constructible<SRWWriteGuard>::value);
	EXPECT_TRUE((std::is_constructible<SRWReadGuard, SRWLockWrapper&>::value));
	EXPECT_FALSE((std::is_convertible<SRWLockWrapper&, SRWWriteGuard>::value));
	// Read/Write 상호 대체 불가(타입 안전의 핵심)
	EXPECT_FALSE((std::is_convertible<SRWWriteGuard&, SRWReadGuard&>::value));
}

// ==========================================================================
// 정확성: reader 는 절대 절반만 갱신된 쌍(torn)을 보지 않는다
// ==========================================================================

TEST(SRWLockCorrectness, ReaderNeverSeesTornPair)
{
	SRWLockWrapper lock;
	volatile LONG64 a = 0;
	volatile LONG64 b = 0;
	volatile LONG   tornSeen = 0;   // reader 가 a!=b 를 한 번이라도 보면 1
	volatile LONG   stop     = 0;   // writer 가 끝나면 reader 루프 종료 신호

	const DWORD kReaders = 8;

	auto work = [&](DWORD threadIndex, UINT32, UINT64)
	{
		if (threadIndex == 0)
		{
			// writer: (a,b) 를 항상 같은 값으로 짝 갱신
			for (LONG64 i = 1; i <= 200000; ++i)
			{
				SRWWriteGuard guard(lock);
				a = i;
				b = i;
			}
			::InterlockedExchange(&stop, 1);
		}
		else
		{
			// reader: 완결 상태라면 a==b. 다르면 torn(배타 실패).
			while (::InterlockedCompareExchange(&stop, 0, 0) == 0)
			{
				SRWReadGuard guard(lock);
				if (a != b)
				{
					::InterlockedExchange(&tornSeen, 1);
				}
			}
		}
	};

	KYS::TESTS::HammerResult r =
		KYS::TESTS::ConcurrentHammer::Run(kReaders + 1, 1, work);

	ASSERT_FALSE(r.timedOut) << "hang-timeout 초과 (seed=" << r.seed << ")";
	LONG tornObserved = tornSeen;   // join 후 단일 스레드 읽기
	EXPECT_EQ(tornObserved, 0) << "reader 가 절반만 갱신된 쌍(torn)을 관측: 배타 실패";
}

// ==========================================================================
// 고경합 스트레스: 동시 reader 증명 / writer 배타 증명
// ==========================================================================

// max = max(max, sample) 를 원자적으로 갱신
static void UpdateMax(volatile LONG* maxSlot, LONG sample)
{
	for (;;)
	{
		LONG observed = *maxSlot;
		if (sample <= observed)
		{
			break;
		}
		if (::InterlockedCompareExchange(maxSlot, sample, observed) == observed)
		{
			break;
		}
	}
}

TEST(SRWLockStress, MultipleReadersHoldSimultaneously)
{
	SRWLockWrapper lock;
	volatile LONG  currentReaders = 0;
	volatile LONG  maxReaders     = 0;

	const DWORD kReaders = 8;

	auto readerWork = [&](DWORD, UINT32, UINT64)
	{
		SRWReadGuard guard(lock);                           // 공유 잠금(여럿 동시 가능)
		LONG now = ::InterlockedIncrement(&currentReaders);
		UpdateMax(&maxReaders, now);
		::Sleep(2);                                         // 다른 reader 들이 겹칠 시간 확보
		::InterlockedDecrement(&currentReaders);
	};

	KYS::TESTS::HammerResult r =
		KYS::TESTS::ConcurrentHammer::Run(kReaders, 50, readerWork);

	ASSERT_FALSE(r.timedOut) << "hang-timeout 초과 (seed=" << r.seed << ")";
	LONG maxObserved = maxReaders;   // join 후 단일 스레드 읽기
	EXPECT_GT(maxObserved, 1) << "동시에 여러 reader 가 락을 보유 못 함: 공유 잠금 실패";
}

TEST(SRWLockStress, WritersAreExclusive)
{
	SRWLockWrapper lock;
	volatile LONG  currentWriters = 0;
	volatile LONG  maxWriters     = 0;

	auto writerWork = [&](DWORD, UINT32, UINT64)
	{
		SRWWriteGuard guard(lock);                          // 배타 잠금(하나만)
		LONG now = ::InterlockedIncrement(&currentWriters);
		UpdateMax(&maxWriters, now);
		::Sleep(1);
		::InterlockedDecrement(&currentWriters);
	};

	KYS::TESTS::HammerResult r =
		KYS::TESTS::ConcurrentHammer::Run(8, 50, writerWork);

	ASSERT_FALSE(r.timedOut) << "hang-timeout 초과 (seed=" << r.seed << ")";
	LONG maxObserved = maxWriters;   // join 후 단일 스레드 읽기
	EXPECT_EQ(maxObserved, 1) << "writer 배타 위반: 2명 이상 동시 진입";
}

// ==========================================================================
// 엣지: happy-path 사이클 (재진입/업그레이드/오소유 해제는 실행 금지 경계)
// ==========================================================================

TEST(SRWLockEdge, HappyPathReadWriteCycles)
{
	SRWLockWrapper lock;
	for (int i = 0; i < 1000; ++i)
	{
		{
			SRWReadGuard rg(lock);    // 공유 잠금 사이클
		}
		{
			SRWWriteGuard wg(lock);   // 배타 잠금 사이클
		}
	}
	SUCCEED();
	// [!] 아래는 SRWLOCK 이 non-recursive/업그레이드 미지원이라 실행하지 않는다(데드락/UB):
	//   (1) 같은 스레드가 ReadLock 안에서 WriteLock 재획득(업그레이드) -> 자기 무한 대기
	//   (2) 같은 스레드가 WriteLock 중첩(재진입) -> 무한 대기
	//   (3) 잡지 않은 락 UnLock / 다른 모드로 해제(오소유) -> UB
	// 위 오용은 App Verifier(SRWLock 레이어)로 런타임 포착한다.
}
