#pragma once
#pragma warning(push)
#pragma warning(disable:4324)

#include <Windows.h>
#include <utility>

namespace KYS
{
    namespace GAMESERVER
    {
        namespace MEMORY
        {
            // 스레드별 빈 슬롯 목록을 둬서 평소엔 락 없이 빌려주는 객체 풀 (Worker 경합 회피).
            template<typename T>
            class TlsObjectPool
            {
            public:
                explicit TlsObjectPool(UINT32 capacity);   // capacity개 슬롯을 한 번에 확보
                ~TlsObjectPool();

                // 복사/이동 금지 (풀은 자기 메모리를 단독 소유)
                TlsObjectPool(const TlsObjectPool&) = delete;
                TlsObjectPool& operator=(const TlsObjectPool&) = delete;
                TlsObjectPool(TlsObjectPool&&) = delete;
                TlsObjectPool& operator=(TlsObjectPool&&) = delete;

                // 내 스레드 목록에서 빈 슬롯을 꺼내 T 생성, 포인터 반환 (없으면 nullptr)
                template <typename... Args>
                T* Allocate(Args&&... args);
                // 빌린 객체를 소멸시키고, 그 슬롯을 만든 풀에 반납
                void Deallocate(T* object);

                UINT32 GetUsedCount() const;                 // 지금 빌려나간 슬롯 수 (스레드 합산 근사값)
                UINT32 GetCapacity() const;                  // 전체 슬롯 수
                bool IsFromThisPool(const T* object) const;  // 이 포인터가 이 풀 메모리 범위에서 나온 것인지

            private:
                struct BlockHeader
                {
                    SLIST_ENTRY slistEntry;     // 맨 앞: 16B 정렬 기준점 (블록주소 == entry주소)
                    UINT32 magic;               // 사용중/반납됨 표식 (이중 해제 감지)
                    UINT32 reserved;
                    TlsObjectPool<T>* owner;    // 이 슬롯을 만든 풀 (반납 라우팅용)
                };

                BYTE* m_memoryBlock;            // 전체 슬롯을 담은 통짜 버퍼 (16B 정렬 _aligned_malloc)
                SLIST_HEADER m_threadFree;      // 다른 스레드가 반납한 빈 슬롯 모음 (owner가 통째로 가져감)
                DWORD m_tlsIndex;               // 각 스레드가 자기 빈 슬롯 목록을 찾는 핸들
                UINT32 m_capacity;              // 전체 슬롯 수
                volatile LONG m_usedCount;      // 빌려나간 슬롯 수 (Interlocked로 증감)

                static const UINT32 MAGIC_USED = 0xDEADBEEF;   // 슬롯 표식: 사용중
                static const UINT32 MAGIC_FREE = 0xFEEEFEEE;   // 슬롯 표식: 반납됨

                // 한 슬롯의 바이트 크기 (16의 배수로 올림 - SLIST가 16B 정렬 요구)
                static UINT32 BlockSize()
                {
                    UINT32 raw = sizeof(BlockHeader) + sizeof(T);
                    return (raw + (MEMORY_ALLOCATION_ALIGNMENT - 1))
                        & ~(MEMORY_ALLOCATION_ALIGNMENT - 1);
                }

                // index번째 슬롯의 주소 계산 (슬롯 접근 헬퍼)
                BlockHeader* GetBlockAt(UINT32 index)
                {
                    return reinterpret_cast<BlockHeader*>(
                        m_memoryBlock + index * BlockSize()
                        );
                }
            };
        }
    }
}

#pragma warning(pop)

#include "TlsObjectPool.inl"
