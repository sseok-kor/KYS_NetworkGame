#pragma once

#include "./Core/Thread/SpinLock.h"

#include <utility>


namespace KYS
{
	namespace GAMESERVER
	{
		namespace MEMORY
		{
			// 고정 개수 슬롯을 미리 잡아두고 빌려주는 객체 풀 (잦은 new/delete 비용 회피).
			template<typename T>
			class ObjectPool
			{
			public:
				explicit ObjectPool(UINT32 capacity);   // capacity개 슬롯을 한 번에 확보
				~ObjectPool();

				// 복사/이동 금지 (풀은 자기 메모리를 단독 소유)
				ObjectPool(const ObjectPool&) = delete;
				ObjectPool& operator=(const ObjectPool&) = delete;
				ObjectPool(ObjectPool&&) = delete;
				ObjectPool& operator=(ObjectPool&&) = delete;

				// 빈 슬롯 하나를 빌려 그 자리에 T를 생성, 포인터 반환 (없으면 nullptr)
				template <typename... Args>
				T* Allocate(Args&&... args);
				// 빌린 객체를 소멸시키고 슬롯을 빈 목록으로 반납
				void Deallocate(T* object);

				UINT32 GetUsedCount() const;                 // 지금 빌려나간 슬롯 수
				UINT32 GetCapacity() const;                  // 전체 슬롯 수
				bool IsFromThisPool(const T* object) const;  // 이 포인터가 이 풀 메모리 범위에서 나온 것인지


			private:
				// 각 슬롯 앞에 붙는 관리 머리말 (한 슬롯 = BlockHeader + T)
				struct BlockHeader
				{
					UINT32 magic;            // 사용중/반납됨 표식 (이중 해제 감지)
					UINT32 reserved;
					ObjectPool<T>* owner;    // 이 슬롯이 속한 풀 (반납 검증용)
					BlockHeader* nextFree;   // 다음 빈 슬롯 (빈 목록 연결용)
				};
				BYTE* m_memoryBlock;         // 전체 슬롯을 담은 통짜 버퍼
				BlockHeader* m_freeHead;     // 빈 슬롯 목록의 맨 앞 (Allocate가 여기서 꺼냄)
				UINT32 m_usedCount;          // 빌려나간 슬롯 수
				UINT32 m_capacity;           // 전체 슬롯 수
				KYS::GAMESERVER::THREAD::SpinLock m_spinLock;   // Allocate/Deallocate 동시 호출 보호

				static const UINT32 MAGIC_USED = 0xDEADBEEF;   // 슬롯 머리말 표식: 사용중
				static const UINT32 MAGIC_FREE = 0xFEEEFEEE;   // 슬롯 머리말 표식: 반납됨

				// index번째 슬롯의 머리말 주소 계산 (슬롯 접근 헬퍼)
				BlockHeader* GetBlockAt(UINT32 index)
				{
					return reinterpret_cast<BlockHeader*>(
						m_memoryBlock + index * (sizeof(BlockHeader) + sizeof(T))
						);
				}


			};
		}
	}
}



// 템플릿 구현 포함
#include "ObjectPool.inl"
