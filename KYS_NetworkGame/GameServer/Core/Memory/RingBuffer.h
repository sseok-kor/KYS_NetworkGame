#pragma once
#include "./Core/Thread/SpinLock.h"

namespace KYS
{
	namespace GAMESERVER
	{
		namespace MEMORY
		{
			// 원형 버퍼 - 끝에 닿으면 앞으로 되감아 재사용하는 송수신 바이트 버퍼.
			class RingBuffer
			{
			public:
				explicit RingBuffer(int capacity = RECV_BUFFER_SIZE);   // capacity 바이트 확보 (기본=수신 버퍼 크기)
				~RingBuffer();

				// 복사/이동 금지 (버퍼는 자기 메모리를 단독 소유)
				RingBuffer(const RingBuffer&) = delete;
				RingBuffer& operator=(const RingBuffer&) = delete;
				RingBuffer(RingBuffer&&) = delete;
				RingBuffer& operator=(RingBuffer&&) = delete;

				bool Enqueue(const char* data, int size);   // 데이터를 뒤에 복사해 넣음 (공간 부족이면 false)
				bool Dequeue(char* dest, int size);          // 앞에서 size만큼 꺼내 복사 (꺼낸 만큼 비움)
				bool Peek(char* dest, int size);             // 꺼내지 않고 앞 데이터만 복사 (읽기 위치 안 움직임)

				char* GetWritePtr();             // 다음에 쓸 위치 (WSARecv가 직접 채울 자리)
				int GetDirectEnqueueSize();      // 되감기 없이 한 번에 이어 쓸 수 있는 바이트 수
				bool MoveWritePos(int size);     // 외부가 직접 써넣은 만큼 쓰기 위치 전진

				char* GetReadPtr();              // 다음에 읽을 위치 (WSASend가 직접 보낼 자리)
				int GetDirectDequeueSize();      // 되감기 없이 한 번에 이어 읽을 수 있는 바이트 수
				bool MoveReadPos(int size);      // 처리한 만큼 읽기 위치 전진

				int GetUsedSize() const;         // 지금 들어있는 바이트 수
				int GetFreeSize() const;         // 더 넣을 수 있는 바이트 수
				int GetCapacity() const;         // 전체 크기
				void Clear();                    // 내용 비우고 위치 초기화

			private:
				char* m_buffer;        // 실제 바이트를 담는 버퍼
				int m_capacity;        // 버퍼 전체 크기
				int m_readPos;         // 다음에 읽을 위치
				int m_writePos;        // 다음에 쓸 위치
				int m_usedSize;        // 현재 들어있는 바이트 수
				KYS::GAMESERVER::THREAD::SpinLock m_spinLock;   // 동시 Enqueue/Dequeue 보호

			};
		}
	}
}
