#include "pch_gameserver.h"
#include "SRWLockWrapper.h"

// SRWLOCK을 사용 가능한 상태로 초기화.
KYS::GAMESERVER::THREAD::SRWLockWrapper::SRWLockWrapper()
{
	InitializeSRWLock(&m_srwLock);
}

// SRWLOCK은 따로 해제할 자원이 없다 (Windows가 관리).
KYS::GAMESERVER::THREAD::SRWLockWrapper::~SRWLockWrapper()
{

}

// 읽기용으로 잠근다 (다른 읽기 스레드와 공유).
void KYS::GAMESERVER::THREAD::SRWLockWrapper::ReadLock()
{
	AcquireSRWLockShared(&m_srwLock);
}

// 읽기 잠금을 푼다.
void KYS::GAMESERVER::THREAD::SRWLockWrapper::ReadUnLock()
{
	ReleaseSRWLockShared(&m_srwLock);
}

// 쓰기용으로 혼자 잠근다 (읽기/쓰기 모두 대기시킴).
void KYS::GAMESERVER::THREAD::SRWLockWrapper::WriteLock()
{
	AcquireSRWLockExclusive(&m_srwLock);
}

// 쓰기 잠금을 푼다.
void KYS::GAMESERVER::THREAD::SRWLockWrapper::WriteUnLock()
{
	ReleaseSRWLockExclusive(&m_srwLock);
}

// 가드 생성과 동시에 읽기 잠금을 잡는다.
//   lock : 이 가드가 잡았다 풀 대상 락
KYS::GAMESERVER::THREAD::SRWReadGuard::SRWReadGuard(SRWLockWrapper& lock)
	:m_lock(lock)
{
	m_lock.ReadLock();
}

// 스코프를 벗어나면 읽기 잠금을 푼다.
KYS::GAMESERVER::THREAD::SRWReadGuard::~SRWReadGuard()
{
	m_lock.ReadUnLock();
}

// 가드 생성과 동시에 쓰기 잠금을 잡는다.
//   lock : 이 가드가 잡았다 풀 대상 락
KYS::GAMESERVER::THREAD::SRWWriteGuard::SRWWriteGuard(SRWLockWrapper& lock)
	:m_lock(lock)
{
	m_lock.WriteLock();
}

// 스코프를 벗어나면 쓰기 잠금을 푼다.
KYS::GAMESERVER::THREAD::SRWWriteGuard::~SRWWriteGuard()
{
	m_lock.WriteUnLock();
}
