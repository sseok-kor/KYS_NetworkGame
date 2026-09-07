#include "pch_serverapp.h"
#include "Item.h"
#include <Windows.h>   // InterlockedIncrement64 / InterlockedExchange64

// 마지막으로 발급한 uid. 여러 채널 스레드가 동시에 발급해도 Interlocked 로 중복이 안 생긴다.
static volatile LONG64 g_lastIssuedItemUid = 0;

void KYS::SERVERAPP::SeedItemUid(UINT64 maxUidInDb)
{
    ::InterlockedExchange64(&g_lastIssuedItemUid, (LONG64)maxUidInDb);
}

UINT64 KYS::SERVERAPP::IssueItemUid()
{
    return (UINT64)::InterlockedIncrement64(&g_lastIssuedItemUid);
}
