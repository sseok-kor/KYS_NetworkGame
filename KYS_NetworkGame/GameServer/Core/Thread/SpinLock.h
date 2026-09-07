#pragma once

namespace KYS
{
	namespace GAMESERVER
	{
		namespace THREAD
		{
			// 한 번에 한 스레드만 들어가게 막는 짧은 락.
			// 락이 풀릴 때까지 CPU를 돌며 기다린다 (짧게 잡고 바로 푸는 구간용).
			class SpinLock
			{
			public:
				SpinLock();
				~SpinLock();

				SpinLock(const SpinLock&) = delete;
				SpinLock& operator=(const SpinLock&) = delete;

				void Lock();      // 락을 잡을 때까지 돈다 (잡으면 반환)
				void UnLock();    // 잡은 락을 푼다
				bool TryLock();   // 한 번만 잡기 시도, 잡았으면 true

			private:
				volatile LONG m_lock; // 0=풀림, 1=잡힘 (Lock/UnLock/TryLock이 Interlocked로 갱신)
			};

			// 스코프 동안 SpinLock을 잡았다가 빠져나갈 때 자동으로 푸는 RAII 가드.
			class SpinLockGuard
			{
			public:
				explicit SpinLockGuard(SpinLock& lock); // 생성과 동시에 lock을 잡음
				~SpinLockGuard();                       // 스코프를 벗어나면 자동으로 풂

				SpinLockGuard(const SpinLockGuard&) = delete;
				SpinLockGuard& operator=(const SpinLockGuard&) = delete;

			private:
				SpinLock& m_lock; // 잡고 있는 락 (소멸 시 UnLock 대상)
			};
		}
	}
}
