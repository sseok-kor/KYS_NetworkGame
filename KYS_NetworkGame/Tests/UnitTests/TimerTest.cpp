#include "pch_tests.h"

#include "Core/Timer.h"

#include <type_traits>

using namespace KYS::GAMESERVER;

// ==========================================================================
// 컴파일 타임 계약: Timer 는 Rule of Zero 값 타입 (복사 가능 - =delete 없음)
// ==========================================================================

static_assert(std::is_copy_constructible<Timer>::value, "Timer 는 값 타입 - 복사 가능");
static_assert(std::is_copy_assignable<Timer>::value,    "Timer 는 값 타입 - 복사 대입 가능");

// ==========================================================================
// 초기화 게이트 property (미초기화 0 / Reset no-op / Initialize)
// ==========================================================================

// 미초기화 상태 - 3 단위 모두 0 (결정론, 벽시계 무관)
TEST(TimerTest, UninitializedReturnsZero)
{
	Timer t;
	EXPECT_EQ(t.GetCurrentTimeMS(), 0ULL);
	EXPECT_EQ(t.GetCurrentTimeMicro(), 0ULL);
	EXPECT_EQ(t.GetCurrentTimeNano(), 0ULL);
}

// 미초기화 Reset - no-op (m_initialized 게이트), 여전히 0
TEST(TimerTest, ResetBeforeInitializeIsNoOp)
{
	Timer t;
	t.Reset();                          // 미초기화라 무시됨
	EXPECT_EQ(t.GetCurrentTimeMS(), 0ULL);
}

// Initialize - QPF 성공 시 true
TEST(TimerTest, InitializeSucceeds)
{
	Timer t;
	EXPECT_TRUE(t.Initialize());
}

// ==========================================================================
// 측정 property (단조성 / 단위순서 / Reset 재기준 / 복사)
//   [!] 절대 시각 magnitude 는 어떤 assert 로도 고정하지 않는다 (flaky 회피).
// ==========================================================================

// 단조성 - 연속 두 측정에서 뒤가 앞보다 작지 않음 (property, 절대값 assert 안 함)
TEST(TimerTest, MonotonicNonDecreasing)
{
	Timer t;
	ASSERT_TRUE(t.Initialize());

	UINT64 a = t.GetCurrentTimeNano();
	UINT64 b = t.GetCurrentTimeNano();
	EXPECT_GE(b, a);                    // 단조 증가(같은 틱이면 동일)
}

// 단위 순서 - 같은 경과를 더 잘게 나눈 단위가 카운트가 크거나 같다 (nano >= micro >= ms)
TEST(TimerTest, UnitOrdering)
{
	Timer t;
	ASSERT_TRUE(t.Initialize());

	UINT64 ms = t.GetCurrentTimeMS();
	UINT64 micro = t.GetCurrentTimeMicro();
	UINT64 nano = t.GetCurrentTimeNano();
	// 1ms = 1000 micro = 1,000,000 nano -> 같은 시각이면 나노 카운트가 가장 크다
	EXPECT_GE(nano, micro);
	EXPECT_GE(micro, ms);
}

// 경과 하한 - Sleep(50)은 최소 50ms 이상 재운다(Windows Sleep 하한 보장) -> 40ms 하한은 flaky 없음.
// 이 하한이 "getter가 무조건 0을 반환"하는 결함(거짓 초록의 전형)을 즉사시킨다.
TEST(TimerTest, ElapsedReflectsSleepLowerBound)
{
	Timer t;
	ASSERT_TRUE(t.Initialize());

	::Sleep(50);
	UINT64 ms    = t.GetCurrentTimeMS();
	UINT64 micro = t.GetCurrentTimeMicro();
	UINT64 nano  = t.GetCurrentTimeNano();

	EXPECT_GE(ms, 40ULL);                 // Sleep 하한 - 0 반환이면 여기서 실패
	EXPECT_GE(micro, ms * 1000ULL);       // 단위 배율 관계 (뒤 측정 + 잘게 나눈 단위 = 크거나 같음)
	EXPECT_GE(nano, micro * 1000ULL);
}

// Reset 재기준 - 시간 흐른 뒤 Reset 하면 경과가 실제로 되돌아간다 (margin 30ms - no-op Reset 이면 실패)
TEST(TimerTest, ResetRebasesElapsed)
{
	Timer t;
	ASSERT_TRUE(t.Initialize());

	::Sleep(50);                             // 최소 ~50ms 확보 (Windows Sleep 은 요청 이상)
	UINT64 beforeMs = t.GetCurrentTimeMS();
	ASSERT_GE(beforeMs, 40ULL);              // 전제: 경과가 실제로 쌓였음 (0 반환 결함 차단)
	t.Reset();
	UINT64 afterMs = t.GetCurrentTimeMS();   // Reset 직후라 near-0

	// Reset 이 최소 30ms 이상 되돌려야 통과 - no-op Reset(after==before)이면 실패.
	// 절대값(after==0)은 선점/반올림으로 flaky 라 margin 방향 검사로 고정.
	EXPECT_LE(afterMs + 30, beforeMs);
}

// 복사 가능 (Rule of Zero, value type) - 복사본도 독립적으로 동작
TEST(TimerTest, CopyableValueType)
{
	Timer t;
	ASSERT_TRUE(t.Initialize());

	Timer copy = t;                     // 복사 (=delete 없음, 값 멤버 비트 복제)
	UINT64 a = copy.GetCurrentTimeMS();
	UINT64 b = copy.GetCurrentTimeMS();
	EXPECT_GE(b, a);                    // 복사본이 정상 측정
}
