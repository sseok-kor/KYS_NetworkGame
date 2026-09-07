#include "pch_tests.h"

#include "Core/Thread/SpinLock.h"

// (1) 프레임워크와 러너가 살아있다는 최소 증명.
TEST(SmokeTest, RunnerWorks)
{
	SUCCEED();   // 이 테스트가 리포트에 뜨면 InitGoogleTest/RUN_ALL_TESTS가 동작
}

// (2) GameServer.lib 심볼에 실제로 링크됐다는 증명.
//     SpinLock을 한 번 잡았다 풀어 라이브러리 코드가 링크되게 강제한다.
TEST(SmokeTest, GameServerLibLinks)
{
	KYS::GAMESERVER::THREAD::SpinLock lock;
	lock.Lock();
	lock.UnLock();
	SUCCEED();
}
