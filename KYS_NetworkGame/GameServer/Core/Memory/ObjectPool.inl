#pragma once


// capacity개 슬롯을 통짜로 잡고, 전부 빈 목록에 연결해 둠.
//   capacity : 미리 확보할 슬롯 수
template<typename T>
KYS::GAMESERVER::MEMORY::ObjectPool<T>::ObjectPool(UINT32 capacity)
	: m_capacity(capacity)
	, m_usedCount(0)
	, m_memoryBlock(nullptr)
	, m_freeHead(nullptr)
{
	UINT32 blockSize = sizeof(BlockHeader) + sizeof(T);
	// 뒤의 ()로 슬롯 메모리를 한 번 0으로 채운다. 재활용 시 이전 값을 읽는 타입
	// (예: LockFreeQueue의 Node - next에 ABA 태그를 보존)은 첫 사용 때 그 값이
	// 미초기화면 쓰레기를 읽게 되므로, 처음부터 결정론적인 0에서 시작하게 한다.
	m_memoryBlock = new BYTE[capacity * blockSize]();

	// 뒤 슬롯부터 앞으로 빈 목록에 끼워 0번이 맨 앞에 오게 함
	for (INT32 i = capacity - 1; i >= 0; i--)
	{
		BlockHeader* block = GetBlockAt(i);
		block->magic = MAGIC_FREE;
		block->reserved = 0;
		block->owner = this;
		block->nextFree = m_freeHead;
		m_freeHead = block;
	}
}

// 풀 전체 메모리 해제 (반납 안 된 객체가 있으면 디버그에서 누수 표시).
template <typename T>
KYS::GAMESERVER::MEMORY::ObjectPool<T>::~ObjectPool()
{
	#ifdef _DEBUG
	if (m_usedCount > 0)
	{
		//오류 (누수)
	}
	#endif // _DEBUG

	delete[] m_memoryBlock;
	m_memoryBlock = nullptr;
	m_freeHead = nullptr;

}

// 빈 슬롯 하나를 꺼내 그 자리에 T를 생성해 돌려줌.
//   args : T 생성자에 그대로 전달할 인자들
template <typename T>
template <typename... Args>
T* KYS::GAMESERVER::MEMORY::ObjectPool<T>::Allocate(Args&&... args)
{
	BlockHeader* tempHeader = nullptr;

	{
		KYS::GAMESERVER::THREAD::SpinLockGuard guard(m_spinLock);

		if (m_freeHead == nullptr)
		{
		#ifdef _DEBUG
			//오류 (빈 슬롯 없음)
		#endif // _DEBUG

			return nullptr;
		}

		// 빈 목록 맨 앞 슬롯을 떼어내 사용중으로 표시
		tempHeader = m_freeHead;
		m_freeHead = tempHeader->nextFree;

		tempHeader->magic = MAGIC_USED;
		tempHeader->nextFree = nullptr;
		m_usedCount++;
	}

	// 머리말 바로 뒤 자리에 객체 생성 (락 밖 - 생성 비용은 동기화 밖으로)
	T* object = reinterpret_cast<T*>(tempHeader + 1);
	new (object) T(std::forward<Args>(args)...);



	return object;

}

// 빌린 객체를 소멸시키고 그 슬롯을 빈 목록으로 되돌림.
//   object : Allocate가 돌려준 포인터 (nullptr면 무시)
template <typename T>
void KYS::GAMESERVER::MEMORY::ObjectPool<T>::Deallocate(T* object)
{
	if (object == nullptr)
	{
		return;
	}

	// 객체 바로 앞의 머리말을 찾아 사용중인 슬롯이 맞는지 확인
	BlockHeader* block = reinterpret_cast<BlockHeader*>(object) - 1;

	if (block->magic != MAGIC_USED)
	{
		// 이중 해제
		#ifdef _DEBUG
		//오류
		#endif // DEBUG

		return;
	}

	#ifdef _DEBUG
	if (block->owner != this)
	{
		// 다른 풀의 슬롯
		//오류

		return;
	}
	#endif // DEBUG



	object->~T();

	// 소멸 뒤 슬롯을 빈 목록 맨 앞에 다시 매닮
	{
		KYS::GAMESERVER::THREAD::SpinLockGuard guard(m_spinLock);

		block->nextFree = m_freeHead;
		block->magic = MAGIC_FREE;
		m_freeHead = block;

		m_usedCount--;
	}
}

// 지금 빌려나간 슬롯 수.
template <typename T>
UINT32 KYS::GAMESERVER::MEMORY::ObjectPool<T>::GetUsedCount() const
{
	return m_usedCount;
}

// 전체 슬롯 수.
template <typename T>
UINT32 KYS::GAMESERVER::MEMORY::ObjectPool<T>::GetCapacity() const
{
	return m_capacity;
}

// 이 포인터가 이 풀의 메모리 범위 안에서 나온 것인지 검사.
//   object : 검사할 포인터
template <typename T>
bool KYS::GAMESERVER::MEMORY::ObjectPool<T>::IsFromThisPool(const T* object) const
{
	if (object == nullptr)
	{
		return false;
	}

	const BYTE* ptr = reinterpret_cast<const BYTE*>(object);
	const BYTE* start = m_memoryBlock + sizeof(BlockHeader);
	const BYTE* end = m_memoryBlock + m_capacity * (sizeof(BlockHeader) + sizeof(T));

	return (ptr >= start) && (ptr < end);
}
