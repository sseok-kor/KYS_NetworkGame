#include "pch_gameserver.h"
#include "RingBuffer.h"

// capacity 바이트 버퍼를 확보한 빈 원형 버퍼 생성.
//   capacity : 버퍼 크기(바이트)
KYS::GAMESERVER::MEMORY::RingBuffer::RingBuffer(int capacity)
	:m_capacity(capacity)
	,m_readPos(0)
	,m_writePos(0)
	,m_usedSize(0)
	,m_buffer(nullptr)
{
	m_buffer = new char[capacity];
}

// 버퍼 메모리 해제.
KYS::GAMESERVER::MEMORY::RingBuffer::~RingBuffer()
{
	delete[] m_buffer;
	m_buffer = nullptr;
}

// 데이터를 버퍼 뒤쪽에 복사해 넣음 (남은 공간보다 크면 실패).
//   data : 넣을 바이트 시작 주소
//   size : 넣을 바이트 수
bool KYS::GAMESERVER::MEMORY::RingBuffer::Enqueue(const char* data, int size)
{
	KYS::GAMESERVER::THREAD::SpinLockGuard guard(m_spinLock);

	if (size > GetFreeSize())
	{
		return false;
	}

	// 쓰기 위치부터 버퍼 끝까지 먼저 채움
	int firstSize = min(size,(m_capacity - m_writePos));
	memcpy(m_buffer + m_writePos, data, firstSize);

	// 끝을 넘긴 나머지는 버퍼 맨 앞으로 되감아 채움
	int secondSize = size - firstSize;
	if (secondSize > 0)
	{
		memcpy(m_buffer, data + firstSize, secondSize);
	}

	m_writePos = ((m_writePos + size) % m_capacity);
	m_usedSize += size;

	return true;

}

// 앞에서 size만큼 꺼내 dest로 복사하고 그만큼 비움 (들어있는 양보다 크면 실패).
//   dest : 꺼낸 바이트를 받을 주소
//   size : 꺼낼 바이트 수
bool KYS::GAMESERVER::MEMORY::RingBuffer::Dequeue(char* dest, int size)
{
	KYS::GAMESERVER::THREAD::SpinLockGuard guard(m_spinLock);

	if (size > m_usedSize)
	{
		return false;
	}

	// 읽기 위치부터 버퍼 끝까지 먼저 복사
	int firstSize = min(size, m_capacity - m_readPos);
	memcpy(dest, m_buffer + m_readPos, firstSize);

	// 끝을 넘긴 나머지는 버퍼 맨 앞에서 이어 복사
	int secondSize = size - firstSize;
	if (secondSize > 0)
	{
		memcpy(dest + firstSize, m_buffer, secondSize);
	}

	m_readPos = (m_readPos + size) % m_capacity;
	m_usedSize -= size;
	return true;

}

// Dequeue처럼 복사하지만 읽기 위치를 옮기지 않음 (미리 들여다보기).
//   dest : 복사해 받을 주소
//   size : 들여다볼 바이트 수
bool KYS::GAMESERVER::MEMORY::RingBuffer::Peek(char* dest, int size)
{
	KYS::GAMESERVER::THREAD::SpinLockGuard guard(m_spinLock);

	if (size > m_usedSize)
	{
		return false;
	}

	int firstSize = min(size, m_capacity - m_readPos);
	memcpy(dest, m_buffer + m_readPos, firstSize);

	int secondSize = size - firstSize;
	if (secondSize > 0)
	{
		memcpy(dest + firstSize, m_buffer, secondSize);
	}

	return true;
}

// 다음에 쓸 위치의 포인터 (WSARecv가 여기 직접 채움).
char* KYS::GAMESERVER::MEMORY::RingBuffer::GetWritePtr()
{
	return m_buffer + m_writePos;
}

// 되감기 없이 쓰기 위치부터 한 번에 이어 쓸 수 있는 바이트 수.
int KYS::GAMESERVER::MEMORY::RingBuffer::GetDirectEnqueueSize()
{
	int freeSize = GetFreeSize();
	if (freeSize == 0)
	{
		return 0;
	}

	if (m_writePos < m_readPos)
	{
		return m_readPos - m_writePos;
	}

	else
	{
		return m_capacity - m_writePos;
	}
}

// 외부가 GetWritePtr 자리에 직접 써넣은 뒤, 그만큼 쓰기 위치를 전진.
//   size : 직접 써넣은 바이트 수
bool KYS::GAMESERVER::MEMORY::RingBuffer::MoveWritePos(int size)
{
	if (size > GetFreeSize())
	{
		return false;
	}

	m_writePos = (m_writePos + size) % m_capacity;
	m_usedSize += size;

	return true;
}

// 다음에 읽을 위치의 포인터 (WSASend가 여기서 직접 보냄).
char* KYS::GAMESERVER::MEMORY::RingBuffer::GetReadPtr()
{
	return m_buffer + m_readPos;
}

// 되감기 없이 읽기 위치부터 한 번에 이어 읽을 수 있는 바이트 수.
int KYS::GAMESERVER::MEMORY::RingBuffer::GetDirectDequeueSize()
{
	int usedSize = GetUsedSize();
	if (usedSize == 0)
	{
		return 0;
	}

	if (m_readPos < m_writePos)
	{
		return m_writePos - m_readPos;
	}
	else
	{
		return m_capacity - m_readPos;
	}
}

// 외부가 GetReadPtr 자리에서 직접 보낸 뒤, 그만큼 읽기 위치를 전진.
//   size : 보낸(처리한) 바이트 수
bool KYS::GAMESERVER::MEMORY::RingBuffer::MoveReadPos(int size)
{
	if (size > GetUsedSize())
	{
		return false;
	}

	m_readPos = (m_readPos + size) % m_capacity;
	m_usedSize -= size;
	return true;
}

// 지금 들어있는 바이트 수.
int KYS::GAMESERVER::MEMORY::RingBuffer::GetUsedSize() const
{
	return m_usedSize;
}

// 더 넣을 수 있는 바이트 수.
int KYS::GAMESERVER::MEMORY::RingBuffer::GetFreeSize() const
{
	return (m_capacity - m_usedSize);
}

// 버퍼 전체 크기.
int KYS::GAMESERVER::MEMORY::RingBuffer::GetCapacity() const
{
	return m_capacity;
}

// 내용을 비우고 읽기/쓰기 위치를 처음으로 되돌림.
void KYS::GAMESERVER::MEMORY::RingBuffer::Clear()
{
	KYS::GAMESERVER::THREAD::SpinLockGuard guard(m_spinLock);
	m_readPos = 0;
	m_writePos = 0;
	m_usedSize = 0;
}
