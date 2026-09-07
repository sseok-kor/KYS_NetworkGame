#pragma once

namespace KYS
{
	namespace GAMESERVER
	{
		namespace THREAD
		{
			// 읽기는 여러 스레드가 동시에, 쓰기는 혼자만 잡는 락.
			// 읽기는 잦고 쓰기는 드문 곳에 쓴다 (Windows SRWLOCK 래퍼).
			class SRWLockWrapper
			{
			public:
				SRWLockWrapper();
				~SRWLockWrapper();

				SRWLockWrapper(const SRWLockWrapper&) = delete;
				SRWLockWrapper& operator=(const SRWLockWrapper&) = delete;

				void ReadLock();     // 읽기용으로 잡기 (다른 읽기와 공유 가능)
				void ReadUnLock();   // 읽기 잠금 풀기
				void WriteLock();    // 쓰기용으로 혼자 잡기 (읽기/쓰기 전부 대기시킴)
				void WriteUnLock();  // 쓰기 잠금 풀기

			private:
				SRWLOCK m_srwLock; // Windows SRWLOCK 핸들 (Read/Write Lock/UnLock 대상)
			};

			// 스코프 동안 읽기 잠금을 잡았다가 빠져나갈 때 자동으로 푸는 RAII 가드.
			class SRWReadGuard
			{
			public:
				explicit SRWReadGuard(SRWLockWrapper& lock); // 생성과 동시에 읽기 잠금을 잡음
				~SRWReadGuard();                             // 스코프를 벗어나면 자동으로 풂

				SRWReadGuard(const SRWReadGuard&) = delete;
				SRWReadGuard& operator=(const SRWReadGuard&) = delete;

			private:
				SRWLockWrapper& m_lock; // 잡고 있는 락 (소멸 시 ReadUnLock 대상)
			};

			// 스코프 동안 쓰기 잠금을 잡았다가 빠져나갈 때 자동으로 푸는 RAII 가드.
			class SRWWriteGuard
			{
			public:
				explicit SRWWriteGuard(SRWLockWrapper& lock); // 생성과 동시에 쓰기 잠금을 잡음
				~SRWWriteGuard();                             // 스코프를 벗어나면 자동으로 풂

				SRWWriteGuard(const SRWWriteGuard&) = delete;
				SRWWriteGuard& operator=(const SRWWriteGuard&) = delete;

			private:
				SRWLockWrapper& m_lock; // 잡고 있는 락 (소멸 시 WriteUnLock 대상)
			};
		}
	}
}
