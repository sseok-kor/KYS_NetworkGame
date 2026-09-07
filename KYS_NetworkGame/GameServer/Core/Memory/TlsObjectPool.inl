#pragma once

// 슬롯 메모리를 잡고 TLS 핸들을 만든 뒤, 초기 재고를 공용 반납함에 쌓아 둠.
//   capacity : 미리 확보할 슬롯 수
template<typename T>
KYS::GAMESERVER::MEMORY::TlsObjectPool<T>::TlsObjectPool(UINT32 capacity)
    : m_capacity(capacity)
    , m_usedCount(0)
    , m_memoryBlock(nullptr)
    , m_tlsIndex(TLS_OUT_OF_INDEXES)
{
    InitializeSListHead(&m_threadFree);
    m_tlsIndex = TlsAlloc();

    UINT32 blockSize = BlockSize();   // 16의 배수
    m_memoryBlock = reinterpret_cast<BYTE*>(
        _aligned_malloc(static_cast<size_t>(capacity) * blockSize,
            MEMORY_ALLOCATION_ALIGNMENT));

    // 초기 슬롯을 전부 공용 반납함에 적재 -> 첫 Allocate가 통째로 자기 목록으로 가져감
    for (UINT32 i = 0; i < capacity; ++i)
    {
        BlockHeader* block = GetBlockAt(i);
        block->magic = MAGIC_FREE;
        block->reserved = 0;
        block->owner = this;
        InterlockedPushEntrySList(&m_threadFree, &block->slistEntry);
    }
}

// 슬롯 메모리와 TLS 핸들 해제 (반납 안 된 객체가 있으면 디버그에서 누수 표시).
template <typename T>
KYS::GAMESERVER::MEMORY::TlsObjectPool<T>::~TlsObjectPool()
{
#ifdef _DEBUG
    if (m_usedCount > 0)
    {
        //오류 (누수)
    }
#endif // _DEBUG

    if (m_memoryBlock != nullptr)
    {
        _aligned_free(m_memoryBlock);
        m_memoryBlock = nullptr;
    }
    if (m_tlsIndex != TLS_OUT_OF_INDEXES)
    {
        TlsFree(m_tlsIndex);
        m_tlsIndex = TLS_OUT_OF_INDEXES;
    }
}

// 빈 슬롯 하나를 꺼내 그 자리에 T를 생성해 돌려줌 (평소엔 락 없이 내 스레드 목록에서).
//   args : T 생성자에 그대로 전달할 인자들
template <typename T>
template <typename... Args>
T* KYS::GAMESERVER::MEMORY::TlsObjectPool<T>::Allocate(Args&&... args)
{
    // 내 스레드 전용 목록에서 하나 꺼내기 (내 것이라 락 불필요)
    PSLIST_ENTRY entry = reinterpret_cast<PSLIST_ENTRY>(TlsGetValue(m_tlsIndex));

    if (entry == nullptr)
    {
        // 내 목록이 비면, 다른 스레드들이 반납해 둔 빈 슬롯을 한 번에 통째로 가져옴
        entry = InterlockedFlushSList(&m_threadFree);
    }

    if (entry == nullptr)
    {
#ifdef _DEBUG
        //오류 (풀 소진) - 학습 단계: 공유 블록 보충 or nullptr
#endif // _DEBUG
        return nullptr;
    }

    // 가져온 묶음의 첫 슬롯을 쓰고, 나머지는 내 목록으로 보관 (내 소유라 락 불필요)
    TlsSetValue(m_tlsIndex, entry->Next);

    BlockHeader* block = CONTAINING_RECORD(entry, BlockHeader, slistEntry);
    block->magic = MAGIC_USED;
    InterlockedIncrement(&m_usedCount);

    T* object = reinterpret_cast<T*>(block + 1);
    new (object) T(std::forward<Args>(args)...);   // 객체 생성은 동기화 밖

    return object;
}

// 빌린 객체를 소멸시키고, 그 슬롯을 만든 풀의 공용 반납함에 되돌림.
//   object : Allocate가 돌려준 포인터 (nullptr면 무시)
template <typename T>
void KYS::GAMESERVER::MEMORY::TlsObjectPool<T>::Deallocate(T* object)
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
#endif // _DEBUG
        return;
    }

    // 다른 스레드가 빌린 슬롯도 반납될 수 있어, 만든 풀(owner)로 되돌림
    TlsObjectPool<T>* owner = block->owner;

    object->~T();                              // 소멸은 반납 전에 (비싼 일은 동기화 밖)
    block->magic = MAGIC_FREE;

    // 공용 반납함에 락 없이 매달기 (게임 로직이 경합 락을 안 잡도록)
    InterlockedPushEntrySList(&owner->m_threadFree, &block->slistEntry);
    InterlockedDecrement(&owner->m_usedCount);
}

// 지금 빌려나간 슬롯 수 (여러 스레드 합산이라 근사값).
template <typename T>
UINT32 KYS::GAMESERVER::MEMORY::TlsObjectPool<T>::GetUsedCount() const
{
    return static_cast<UINT32>(m_usedCount);   // 여러 스레드 합산 근사값
}

// 전체 슬롯 수.
template <typename T>
UINT32 KYS::GAMESERVER::MEMORY::TlsObjectPool<T>::GetCapacity() const
{
    return m_capacity;
}

// 이 포인터가 이 풀의 메모리 범위 안에서 나온 것인지 검사.
//   object : 검사할 포인터
template <typename T>
bool KYS::GAMESERVER::MEMORY::TlsObjectPool<T>::IsFromThisPool(const T* object) const
{
    if (object == nullptr)
    {
        return false;
    }
    const BYTE* ptr = reinterpret_cast<const BYTE*>(object);
    const BYTE* start = m_memoryBlock + sizeof(BlockHeader);
    const BYTE* end = m_memoryBlock + static_cast<size_t>(m_capacity) * BlockSize();
    return (ptr >= start) && (ptr < end);
}
