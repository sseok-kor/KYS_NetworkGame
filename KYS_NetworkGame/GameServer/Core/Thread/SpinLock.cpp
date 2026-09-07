#include "pch_gameserver.h"
#include "SpinLock.h"

// 락을 풀린 상태(0)로 두고 시작.
KYS::GAMESERVER::THREAD::SpinLock::SpinLock()
	:m_lock(0)
{

}

// 소멸 시 락이 안 잡혀 있는지(0) 점검.
KYS::GAMESERVER::THREAD::SpinLock::~SpinLock()
{
	_ASSERTE(m_lock == 0);
}

// 락을 잡을 때까지 반복 시도한다.
void KYS::GAMESERVER::THREAD::SpinLock::Lock()
{
	while (true)
	{
		// 다른 스레드가 쥐고 있으면 풀릴 때까지 CPU를 양보하며 대기
		while (m_lock != 0)
		{
			YieldProcessor();
		}

		// 풀린 걸 봤으면 원자적으로 1로 바꿔 잡기 시도 (직전 값이 0이면 내가 잡은 것)
		if (InterlockedExchange(&m_lock, 1) == 0)
		{
			return;
		}
	}
}

// 잡은 락을 0으로 돌려 푼다.
void KYS::GAMESERVER::THREAD::SpinLock::UnLock()
{
	InterlockedExchange(&m_lock, 0);
}

// 딱 한 번만 잡기 시도 - 성공(직전 값 0)하면 true.
bool KYS::GAMESERVER::THREAD::SpinLock::TryLock()
{
	return (InterlockedExchange(&m_lock, 1) == 0);
}

// 가드 생성과 동시에 lock을 잡는다.
//   lock : 이 가드가 잡았다 풀 대상 락
KYS::GAMESERVER::THREAD::SpinLockGuard::SpinLockGuard(SpinLock& lock)
	:m_lock(lock)
{
	m_lock.Lock();
}

// 스코프를 벗어나면 잡았던 락을 푼다.
KYS::GAMESERVER::THREAD::SpinLockGuard::~SpinLockGuard()
{
	m_lock.UnLock();
}
