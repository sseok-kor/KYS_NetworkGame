#include "pch_tests.h"

#include "Core/Thread/IJob.h"
#include "TestHarness/MockJob.h"

#include <type_traits>

using KYS::GAMESERVER::THREAD::IJob;
using KYS::TESTS::MockJob;

// ==========================================================================
// 컴파일 타임 계약: IJob = Polymorphic Base (virtual ~ + Rule-of-Five 4줄)
// ==========================================================================

static_assert(std::is_abstract<IJob>::value,            "IJob 은 순수가상 인터페이스");
static_assert(std::has_virtual_destructor<IJob>::value, "다형 delete 안전(virtual ~IJob)");

// base 4줄 =delete -> 파생(MockJob)도 멤버를 추가했어도 자동으로 비복사/비이동
static_assert(!std::is_copy_constructible<MockJob>::value, "파생 복사 생성 금지(base 삭제 상속)");
static_assert(!std::is_copy_assignable<MockJob>::value,    "파생 복사 대입 금지(base 삭제 상속)");
static_assert(!std::is_move_constructible<MockJob>::value, "파생 이동 생성 금지(base 삭제 상속)");
static_assert(!std::is_move_assignable<MockJob>::value,    "파생 이동 대입 금지(base 삭제 상속)");

TEST(IJobContract, PolymorphicBaseTraitsMirror)
{
	EXPECT_TRUE(std::has_virtual_destructor<IJob>::value);
	EXPECT_TRUE(std::is_abstract<IJob>::value);
	EXPECT_FALSE(std::is_copy_constructible<MockJob>::value);
	EXPECT_FALSE(std::is_move_constructible<MockJob>::value);
}

// ==========================================================================
// 런타임 최소 증명: 다형 delete 가 파생 소멸자를 실행한다
// ==========================================================================

TEST(IJobContract, PolymorphicDeleteRunsDerivedDestructor)
{
	MockJob::ResetCounters();

	IJob* job = new MockJob(1);
	job->Execute();
	delete job;   // base 포인터로 delete -> virtual ~IJob 이라 ~MockJob 실행

	EXPECT_EQ(MockJob::GetExecuteCount(), 1);   // Execute 정확히 1회
	EXPECT_EQ(MockJob::GetDestroyCount(), 1);   // 파생 소멸자 실행 = virtual ~ 증거
}
