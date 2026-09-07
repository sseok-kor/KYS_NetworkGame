#pragma once
#include "Session.h"

namespace KYS
{
	namespace GAMESERVER
	{
		namespace NETWORK
		{
            class INetEventHandler;

            // 고정 크기 Session 배열을 free-list로 굴리는 풀. 핸들에 세대를 실어 옛 핸들 재사용을 막는다.
            class SessionPool
            {
            public:
                SessionPool();  // 빈 상태로 만든다 (실제 배열은 Init에서 할당)
                ~SessionPool();  // 배열 메모리 해제


                SessionPool(const SessionPool&) = delete;
                SessionPool& operator=(const SessionPool&) = delete;
                SessionPool(SessionPool&&) = delete;
                SessionPool& operator=(SessionPool&&) = delete;

                bool Init(int maxSessionCount, INetEventHandler* netHandler);       // 슬롯 배열/free-list 할당 + 각 세션에 통지통로/풀 주입 (idx가 32비트 범위인지 검증)

                UINT64   Allocate();                  // 빈 슬롯 하나 꺼내 식별자 세팅 후 핸들(idx<<32 | generation) 반환 (없으면 0)
                void     Release(UINT64 sessionId);   // 슬롯 초기화(generation++)하고 free-list에 되돌림
                Session* Find(UINT64 sessionId);      // 세대가 맞으면 Session* 반환, 옛 핸들이면 nullptr (liveness 체크용, IO deref 금지)
                Session* FindAndLease(UINT64 sessionId);   // 게임 스레드 IO용 - 락 안에서 세대 검증 + IO lease(+1) 원자화 (반환!=null이면 SessionLeaseGuard로 반납). teardown 중이면 nullptr
                Session* FindLockFree(UINT64 sessionId) const;   // worker 전용 - 락 없이 세대만 acquire 읽기로 검증 (옛 슬롯이면 nullptr)
                int      GetUsedCount();              // 지금 쓰이는 슬롯 수 = 전체 - 빈 슬롯 (현재 접속 수, m_lock 보호)
                int      ReapIdleSessions(UINT64 nowTick, UINT64 deadlineMs);   // 무수신 데드라인 넘긴 소켓 일괄 회수(무음 소켓 슬롯 고갈 방어), 회수 수 반환

                int      SnapshotSendMetrics(SessionSendMetrics* out, int maxOut) const;   // 슬롯[0..max) 무락 근사 순회 - out[i]=슬롯 i 송신 압력, 채운 수 반환 (정렬/랭킹/라벨은 게임측)
                void     GetRetiredSendTotals(RetiredSendTotals& out);   // 은퇴(Release) 세션 송신 누적 (활성 슬롯 합과 더해 단조 총량 복원), m_lock 보호

                // 게임측이 sid 로 세션에 내리는 명령 둘 + 값 조회 둘 - 게임측에 Session* 를 내주지 않는다 (찾기/세대 검증/lease/놓기를 전부 이 안에서).
                bool     SendTo(UINT64 sessionId, const BYTE* data, int size, ESendDropPolicy policy);   // 큐 적재 성공 = true. 없는/옛/teardown 중 세션 = false
                bool     DisconnectTo(UINT64 sessionId, EDisconnectReason reason);                        // true = 세션이 있어 Disconnect 를 불렀다
                bool     IsSessionAlive(UINT64 sessionId);                                                // 세대 검증만 (IO 없는 생존 확인)
                UINT32   GetSessionRemoteIp(UINT64 sessionId);                                            // 접속자 IPv4 (없으면 0) - 풀 락 안에서 읽어 Reset 과 상호배제
                void     NoteSendDropEvict();                                                             // Session::Disconnect 가 SEND_TIMEOUT 로 CAS 를 통과할 때 1회 (세션당 1회)
                UINT64   GetSendDropEvictTotal() const;                                                   // 송신 압력으로 쫓아낸 세션 누적 (풀 락 밖에서 오르므로 Interlocked 카운터)

            private:
                Session* m_sessions;                  // 세션 슬롯 배열 (Init에서 1회 할당, 주소 고정)
                UINT32*  m_freeList;                  // free-list - m_freeList[i] = i 슬롯이 비었을 때 다음 빈 슬롯 idx


                int      m_maxSessionCount;           // 슬롯 총 개수
                int      m_freeCount;                 // 빈 슬롯 수 (Allocate에서 --, Release에서 ++) - GetUsedCount 계산용
                UINT32   m_freeHead;                  // 첫 빈 슬롯 idx (INVALID_FREE_INDEX면 풀 고갈)

                // 은퇴(Release 된) 세션 송신 누적 - Reset 0화로 증발하는 카운터를 접어 총량을 단조 복원 (m_lock 보호, Release=disconnect당 1회 cold path).
                UINT64   m_retiredDropCount;
                UINT64   m_retiredDropBytes;
                UINT64   m_retiredWrapCount;
                UINT64   m_retiredWsaPostCount;
                UINT64   m_retiredWatermarkHits;   // 워터마크 상향돌파 누적 - export 총량(wmHitsTotal)의 단조성 재료

                volatile LONG64 m_sendDropEvictTotal;   // SEND_TIMEOUT 축출 누적 - Disconnect 가 풀 락 밖에서 부르므로 Interlocked (은퇴 fold 불요 - 풀 전역 단조)
                SpinLock m_lock;                      // free-list 보호 (Allocate/Release/Find가 여러 스레드에서 불림)
            };
		}
	}
}
