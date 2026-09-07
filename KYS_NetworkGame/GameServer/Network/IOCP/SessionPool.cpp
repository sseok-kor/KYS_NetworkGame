#include "pch_gameserver.h"
#include "SessionPool.h"

static constexpr UINT32 INVALID_FREE_INDEX = 0xFFFFFFFF;

// 빈 풀 - 실제 슬롯 배열은 Init에서 잡는다.
KYS::GAMESERVER::NETWORK::SessionPool::SessionPool()
	: m_sessions(nullptr)
	, m_freeList(nullptr)
	, m_maxSessionCount(0)
	, m_freeCount(0)
	, m_freeHead(INVALID_FREE_INDEX)
	, m_retiredDropCount(0)
	, m_retiredDropBytes(0)
	, m_retiredWrapCount(0)
	, m_retiredWsaPostCount(0)
	, m_retiredWatermarkHits(0)
	, m_sendDropEvictTotal(0)
{

}

// 슬롯 배열과 free-list 메모리 해제.
KYS::GAMESERVER::NETWORK::SessionPool::~SessionPool()
{
	delete[] m_sessions;
	delete[] m_freeList;
}

// 슬롯 배열과 free-list를 할당하고, 각 세션에 통지 통로/풀 포인터를 주입한다.
//   maxSessionCount : 만들 슬롯 수 (idx가 32비트라 그 범위 안이어야 함)
//   netHandler      : 모든 세션이 공유할 게임측 통지 통로
bool KYS::GAMESERVER::NETWORK::SessionPool::Init(int maxSessionCount,INetEventHandler* netHandler)
{
	if (maxSessionCount <= 0 || static_cast<UINT32>(maxSessionCount) >= INVALID_FREE_INDEX)   // 0 이하거나 idx 범위 초과면 실패
	{
		_ASSERTE(false && "SessionPool::Init - maxSessionCount out of UINT32 range");
		return false;
	}

	if (m_sessions != nullptr)   // 이미 만들어졌으면 실패
	{
		_ASSERTE(false && "SessionPool::Init - already Initialized");
		return false;
	}

	m_sessions = new Session[maxSessionCount];
	m_freeList = new UINT32[maxSessionCount];

	for (int i = 0; i < maxSessionCount; i++)   // 모든 슬롯에 통지 통로 + 풀 back-pointer 주입
	{
		m_sessions[i].SetSink(netHandler);
		m_sessions[i].SetPool(this);            // 자가 반납용 풀 포인터 주입
	}

	for (int i = 0; i < maxSessionCount - 1; ++i)   // free-list 사슬 연결: 각 슬롯이 다음 슬롯을 가리킴
	{
		m_freeList[i] = static_cast<UINT32>(i + 1);
	}
	m_freeList[maxSessionCount - 1] = INVALID_FREE_INDEX;   // 마지막 슬롯은 끝 표시
	m_freeHead = 0;
	m_freeCount = maxSessionCount;              // 처음엔 모든 슬롯이 비어 있음
	m_maxSessionCount = maxSessionCount;        // 마지막에 발행 - 무락 sweep(ReapIdleSessions)이 이 값을 N으로 보면 m_sessions/free-list는 이미 완성(x64 TSO). count를 먼저 쓰면 sweep이 mid-Init 창에 null deref 가능
	return true;
}

// 빈 슬롯 하나를 꺼내 핸들(idx<<32 | generation)을 만들어 돌려준다 (없으면 0).
UINT64 KYS::GAMESERVER::NETWORK::SessionPool::Allocate()
{
	SpinLockGuard g(m_lock);
	if (m_freeHead == INVALID_FREE_INDEX)
	{
		return 0;   // 풀 고갈
	}

	UINT32 idx = m_freeHead;
	m_freeHead = m_freeList[idx];               // 빈 슬롯 사슬에서 다음으로 전진
	--m_freeCount;                              // 빈 슬롯 하나 줄임 (고갈 체크 통과 후라 안전)

	// 슬롯 재사용 - 지금 세대로 핸들 생성 (Reset이 미리 세대를 올려둠)
	UINT32 gen = m_sessions[idx].GetGeneration();

	_ASSERTE(gen != 0);

	UINT64 sid = (static_cast<UINT64>(idx) << 32) | gen;   // 핸들 = 슬롯 idx(상위) + 세대(하위)
	m_sessions[idx].Init(sid);                  // 세션에 식별자 세팅

	return sid;
}

// 핸들이 가리키는 슬롯을 초기화하고 free-list에 되돌린다 (옛 핸들이면 아무 것도 안 함).
//   sessionId : 반납할 세션 핸들
void KYS::GAMESERVER::NETWORK::SessionPool::Release(UINT64 sessionId)
{
	SpinLockGuard g(m_lock);
	UINT32 idx = static_cast<UINT32>(sessionId >> 32);   // 핸들 상위 32비트 = 슬롯 idx
	if (idx >= static_cast<UINT32>(m_maxSessionCount))
	{
		return;                                 // 범위 밖이면 무시
	}

	UINT32 gen = static_cast<UINT32>(sessionId & 0xFFFFFFFF);   // 핸들 하위 32비트 = 세대
	if (m_sessions[idx].GetGeneration() != gen)
	{
		return;                                 // 이미 회수/재사용된 핸들 - 그냥 무시 (두 번 반납 차단)
	}

	// 은퇴 누적 접기 - Reset 이 이 슬롯의 송신 카운터를 0화하기 '직전' 에 풀 누적에 가산(단조 총량 복원).
	//   Release=disconnect당 1회 cold path 라 Send 핫패스에 무영향. 활성 슬롯 합 + retired = 세션 이탈로 카운터가 증발해도 감소하지 않는 총량.
	SessionSendMetrics retiredSlot;
	m_sessions[idx].GetSendMetrics(retiredSlot);
	m_retiredDropCount      += retiredSlot.dropCount;
	m_retiredDropBytes      += retiredSlot.dropBytes;
	m_retiredWrapCount      += retiredSlot.wrapCount;
	m_retiredWsaPostCount   += retiredSlot.wsaPostCount;
	m_retiredWatermarkHits  += retiredSlot.watermarkHits;   // 워터마크 히트도 접는다 (export 총량 단조성)

	m_sessions[idx].Reset();                    // 세대 올리고 상태 초기화 (되돌리기 전에)
	m_freeList[idx] = m_freeHead;               // 빈 슬롯 사슬 맨 앞에 되돌림
	m_freeHead = idx;
	++m_freeCount;                              // 빈 슬롯 하나 늘림 (세대 확인 통과 후라 두 번 늘지 않음)
}

// 핸들로 슬롯을 찾는다 - 세대가 맞아야 살아있는 세션.
//   sessionId : 찾을 세션 핸들
KYS::GAMESERVER::NETWORK::Session* KYS::GAMESERVER::NETWORK::SessionPool::Find(UINT64 sessionId)
{
	SpinLockGuard g(m_lock);
	UINT32 idx = static_cast<UINT32>(sessionId >> 32);
	UINT32 gen = static_cast<UINT32>(sessionId & 0xFFFFFFFF);

	if (idx >= static_cast<UINT32>(m_maxSessionCount))
	{
		return nullptr;                         // 범위 밖
	}

	if (m_sessions[idx].GetGeneration() != gen)
	{
		return nullptr;                         // 세대 안 맞음 = 옛 핸들 (use-after-free 차단)
	}

	return &m_sessions[idx];
}

// 게임 스레드가 송신/종료로 세션을 빌려 쓸 때 - 세대 검증과 IO lease(+1)를 풀 락 안에서 원자적으로 마친다.
//   Find는 락을 놓은 뒤 lease가 m_sendLock에서 일어나 Reset(pool lock)과 경합하던 TOCTOU(언더플로/이중 종료)가 있었다.
//   여기선 lease까지 같은 락 안에서 끝내 Reset과 상호배제한다. 반환!=null이면 호출자가 SessionLeaseGuard로 정확히 1회 반납.
//   sessionId : 빌릴 세션 핸들
KYS::GAMESERVER::NETWORK::Session* KYS::GAMESERVER::NETWORK::SessionPool::FindAndLease(UINT64 sessionId)
{
	SpinLockGuard g(m_lock);
	UINT32 idx = static_cast<UINT32>(sessionId >> 32);
	UINT32 gen = static_cast<UINT32>(sessionId & 0xFFFFFFFF);

	if (idx >= static_cast<UINT32>(m_maxSessionCount))
	{
		return nullptr;                         // 범위 밖
	}

	if (m_sessions[idx].GetGeneration() != gen)
	{
		return nullptr;                         // 세대 안 맞음 = 옛 핸들 (use-after-free 차단)
	}

	// lease(+1)를 락 안에서 - Reset(같은 락)과 상호배제되어 stray +1이 Reset의 store에 덮이는 언더플로가 없다.
	// 증가 결과가 1 = 떠 있던 IO가 0이던 세션(recv 상시라 살아있으면 항상 >=1) = teardown 진행 중이라 빌리면 안 됨.
	if (m_sessions[idx].AcquireIoLease() == 1)
	{
		m_sessions[idx].AbortIoLease();         // 되돌림 (raw -1, 종료는 worker가 이미 진행 - 여기서 트리거 금지)
		return nullptr;
	}

	return &m_sessions[idx];                     // 빌림 성공 (lease +1 보유 - 호출자가 가드로 반납)
}

// worker 완료 통지 처리 전용(가장 잦은 경로) - 락 없이 세대만 acquire 읽기로 확인한다.
//   sessionId : 검증할 세션 핸들
KYS::GAMESERVER::NETWORK::Session* KYS::GAMESERVER::NETWORK::SessionPool::FindLockFree(UINT64 sessionId) const
{
	// free-list는 안 건드리고 세대만 acquire 읽기로 확인.
	// 배열 주소는 Init 후 안 바뀌어서 락 없이 읽어도 안전하고,
	// 세대가 안 맞으면 (이미 회수/재사용된 슬롯) 옛 세션의 늦게 도착한 완료통지라 nullptr.
	UINT32 idx = static_cast<UINT32>(sessionId >> 32);
	UINT32 gen = static_cast<UINT32>(sessionId & 0xFFFFFFFF);

	if (idx >= static_cast<UINT32>(m_maxSessionCount))
	{
		return nullptr;                         // 범위 밖
	}

	if (m_sessions[idx].GetGenerationAcquire() != gen)
	{
		return nullptr;                         // 세대 안 맞음 = 옛 완료통지 - 무시 (안전하게 실패)
	}

	return &m_sessions[idx];
}

// 지금 쓰이는 슬롯 수 = 현재 접속 수.
int KYS::GAMESERVER::NETWORK::SessionPool::GetUsedCount()
{
	SpinLockGuard g(m_lock);
	return m_maxSessionCount - m_freeCount;     // 전체 - 빈 슬롯 = 현재 접속 수 (빼기라 음수가 안 됨)
}

// 무수신 타임아웃을 넘긴 소켓을 일괄 회수한다 - 연결만 맺고 deadline 내 아무것도 안 보낸 무음 소켓(슬롯 고갈 방어).
//   전 슬롯을 락 없이 근사 스캔(자유/미개시 슬롯은 lastRecvTick==0로 제외) -> 후보만 FindAndLease(세대 검증+lease)로 안전 종료.
//   실제 종료는 게임측 Kick과 동일 경로(FindAndLease+Guard+Disconnect)라 근사 판정 오판도 세대 불일치로 걸러진다.
//   nowTick    : 현재 시각(GetTickCount64)
//   deadlineMs : 마지막 수신 후 회수까지 허용 시간
int KYS::GAMESERVER::NETWORK::SessionPool::ReapIdleSessions(UINT64 nowTick, UINT64 deadlineMs)
{
	int reaped = 0;
	for (int i = 0; i < m_maxSessionCount; ++i)
	{
		if (!m_sessions[i].IsIdleTimedOut(nowTick, deadlineMs))   // 락 없는 근사 판정 - 자유/활성 슬롯 즉시 통과
		{
			continue;
		}
		UINT64 sid = m_sessions[i].GetSessionId();
		Session* s = FindAndLease(sid);   // 세대 검증 + IO lease
		if (s == nullptr)
		{
			continue;   // 이미 teardown 중/슬롯 재사용 - 세대 불일치로 걸러짐
		}
		SessionLeaseGuard guard(s);       // 스코프 종료 시 lease 반납(continue/정상 종료 모두 dtor가 처리)
		if (!s->IsIdleTimedOut(nowTick, deadlineMs))
		{
			continue;   // lease로 incarnation 고정 후 재확인 - 스캔~lease 사이 활성화/재사용된 세션 오회수 차단(TOCTOU)
		}
		s->Disconnect(EDisconnectReason::NO_RECV_TIMEOUT);
		++reaped;
	}
	return reaped;
}

// 전 슬롯[0..max) 을 락 없이 근사 순회하며 각 슬롯의 송신 압력 스냅샷을 채운다 (out[i] = 슬롯 i). 채운 수 반환.
//   무락 = 모니터(main) 1Hz pull 이 Send 핫패스를 방해하지 않게. torn/근사는 1s 창 허용(기존 socketCount 근사 철학, FindLockFree 선례).
//   라이브러리는 raw per-slot 숫자만 내보낸다 - 정렬/상위 N/accountId 라벨/창평균은 게임측 모니터가 한다(도메인 무관 유지).
int KYS::GAMESERVER::NETWORK::SessionPool::SnapshotSendMetrics(SessionSendMetrics* out, int maxOut) const
{
	if (out == nullptr || maxOut <= 0) { return 0; }
	const int limit = (m_maxSessionCount < maxOut) ? m_maxSessionCount : maxOut;
	for (int i = 0; i < limit; ++i)
	{
		m_sessions[i].GetSendMetrics(out[i]);   // 슬롯당 근사 복사 (자유 슬롯은 metrics 0 + sid 0)
	}
	return limit;
}

// 은퇴 세션 송신 누적을 복사한다 (m_lock 보호 - Release 의 fold 와 직렬화해 일관 스냅샷). 활성 슬롯 합과 더하면 단조 총량.
void KYS::GAMESERVER::NETWORK::SessionPool::GetRetiredSendTotals(RetiredSendTotals& out)
{
	SpinLockGuard g(m_lock);
	out.dropCount     = m_retiredDropCount;
	out.dropBytes     = m_retiredDropBytes;
	out.wrapCount     = m_retiredWrapCount;
	out.wsaPostCount  = m_retiredWsaPostCount;
	out.watermarkHits = m_retiredWatermarkHits;
}

// sid 로 세션을 찾아(세대 검증) 빌리고(lease) 보내고 놓는다 - 게임측이 넉 줄로 조립하던 것을 풀 소유자가 한 함수로 (2026-09-02 §409).
//   반환 false = 세션 없음(끊김/옛 핸들/teardown 중) 또는 큐 넘침/송신 실패. policy 가 EVICT_ON_DROP 이면 큐 넘침 시 Send 안에서 끊는다.
bool KYS::GAMESERVER::NETWORK::SessionPool::SendTo(UINT64 sessionId, const BYTE* data, int size, ESendDropPolicy policy)
{
	Session* s = FindAndLease(sessionId);
	if (s == nullptr) { return false; }
	SessionLeaseGuard lease(s);              // 반납 - 0 이 되면 이 스레드에서 종료 통지 + 슬롯 반납
	return s->Send(data, size, policy);
}

// sid 로 세션을 찾아 빌리고 끊는다 - 게임 사유(계정 중복·채널 만석·프로토콜 위반)로 끊는 자리가 쓴다. 없는/옛 핸들이면 false (새 주인은 안 건드린다).
bool KYS::GAMESERVER::NETWORK::SessionPool::DisconnectTo(UINT64 sessionId, EDisconnectReason reason)
{
	Session* s = FindAndLease(sessionId);
	if (s == nullptr) { return false; }
	SessionLeaseGuard lease(s);
	s->Disconnect(reason);
	return true;
}

// 세대 검증만 - IO 없이 "아직 살아 있는 핸들인가" 를 본다 (Find 의 결과를 bool 로 - 게임측에 Session* 를 안 내준다).
bool KYS::GAMESERVER::NETWORK::SessionPool::IsSessionAlive(UINT64 sessionId)
{
	return Find(sessionId) != nullptr;
}

// 접속자 IPv4 를 값으로 - 풀 락 안에서 읽어 Reset(m_remoteIp = 0) 과 상호배제. 없는/옛 핸들이면 0.
UINT32 KYS::GAMESERVER::NETWORK::SessionPool::GetSessionRemoteIp(UINT64 sessionId)
{
	SpinLockGuard g(m_lock);
	const UINT32 idx = static_cast<UINT32>(sessionId >> 32);
	const UINT32 gen = static_cast<UINT32>(sessionId & 0xFFFFFFFF);
	if (idx >= static_cast<UINT32>(m_maxSessionCount)) { return 0; }
	if (m_sessions[idx].GetGeneration() != gen) { return 0; }
	return m_sessions[idx].GetRemoteIp();
}

// Session::Disconnect 가 SEND_TIMEOUT 로 CAS 를 통과할 때 부른다 - 그 경로가 세션당 한 번뿐이라 집계도 세션당 1회.
void KYS::GAMESERVER::NETWORK::SessionPool::NoteSendDropEvict()
{
	::InterlockedIncrement64(&m_sendDropEvictTotal);
}

// 송신 압력 축출 누적 (모니터 읽기). x64 정렬 8바이트 읽기는 원자적이라 락 없이 본다.
UINT64 KYS::GAMESERVER::NETWORK::SessionPool::GetSendDropEvictTotal() const
{
	return static_cast<UINT64>(m_sendDropEvictTotal);
}
