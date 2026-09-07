#include "pch_tests.h"

#include "Network/IOCP/SessionPool.h"
#include "Network/IOCP/INetEventHandler.h"
#include "Types/Defines.h"

#include <string.h>

using namespace KYS::GAMESERVER::NETWORK;

// ==========================================================================
// 이 파일이 대는 것 - 2026-09-02 §409: sid 로 내리는 명령(SendTo/DisconnectTo)과 값 조회(IsSessionAlive/GetSessionRemoteIp)가
//   게임측에 Session* 를 안 내주고도 세대 검증·lease·처분을 풀 안에서 끝내는가. 소켓/IOCP 없이 검증한다 -
//   Allocate 만 한 슬롯은 "떠 있는 IO" 가 0 이라 FindAndLease 가 거절하고, 살리려면 테스트가 AcquireIoLease 로 recv 하나가 걸린 척한다.
// ==========================================================================

namespace
{
	// 라이브러리가 게임측에 통지하는 세 사건을 세기만 하는 스텁
	class CountingHandler : public INetEventHandler
	{
	public:
		int connect = 0, packet = 0, disconnect = 0;
		UINT64 lastDisconnectSid = 0;
		EDisconnectReason lastReason = EDisconnectReason::CLIENT_FIN;
		void EnqueueConnect(UINT64) override { ++connect; }
		void EnqueuePacket(UINT64, const BYTE*, int) override { ++packet; }
		void EnqueueDisconnect(UINT64 sid, EDisconnectReason reason) override { ++disconnect; lastDisconnectSid = sid; lastReason = reason; }
	};

	struct PoolFixture
	{
		CountingHandler handler;
		SessionPool pool;
		PoolFixture() { EXPECT_TRUE(pool.Init(8, &handler)); }
	};

	const BYTE kTiny[4] = { 0, 4, 0, 1 };
}

// ==========================================================================
// T1 · T2 - 없는 핸들에는 아무것도 안 한다 (옛 세대 / 범위 밖)
// ==========================================================================

TEST(SessionPoolTest, SendToRejectsStaleGeneration)
{
	PoolFixture f;
	const UINT64 sid = f.pool.Allocate();
	ASSERT_NE(sid, 0ull);
	const UINT64 stale = sid ^ 0x1ull;   // 같은 슬롯, 다른 세대
	EXPECT_FALSE(f.pool.SendTo(stale, kTiny, sizeof(kTiny), ESendDropPolicy::KEEP_CONNECTION));
	EXPECT_FALSE(f.pool.DisconnectTo(stale, EDisconnectReason::PROTOCOL_VIOLATION));
	EXPECT_FALSE(f.pool.IsSessionAlive(stale));
	EXPECT_EQ(f.pool.GetSessionRemoteIp(stale), 0u);
	EXPECT_EQ(f.handler.disconnect, 0);
}

TEST(SessionPoolTest, SendToRejectsOutOfRangeIndex)
{
	PoolFixture f;
	const UINT64 bogus = (static_cast<UINT64>(999999) << 32) | 1ull;
	EXPECT_FALSE(f.pool.SendTo(bogus, kTiny, sizeof(kTiny), ESendDropPolicy::EVICT_ON_DROP));
	EXPECT_FALSE(f.pool.DisconnectTo(bogus, EDisconnectReason::NET_RESET));
	EXPECT_FALSE(f.pool.IsSessionAlive(bogus));
	EXPECT_EQ(f.pool.GetSendDropEvictTotal(), 0ull);
}

// ==========================================================================
// T3 - recv 가 안 걸린 슬롯(떠 있는 IO 0)은 빌릴 수 없다 = teardown 중인 세션에 보내지 않는다
// ==========================================================================

TEST(SessionPoolTest, SendToRefusesSlotWithoutPendingIo)
{
	PoolFixture f;
	const UINT64 sid = f.pool.Allocate();
	EXPECT_TRUE(f.pool.IsSessionAlive(sid));                 // 세대는 맞지만
	EXPECT_FALSE(f.pool.SendTo(sid, kTiny, sizeof(kTiny), ESendDropPolicy::KEEP_CONNECTION));   // IO 가 0 이라 FindAndLease 가 거절 (after==1 -> Abort)
	EXPECT_FALSE(f.pool.DisconnectTo(sid, EDisconnectReason::PROTOCOL_VIOLATION));
	EXPECT_TRUE(f.pool.IsSessionAlive(sid));                 // 거절은 슬롯을 건드리지 않는다
	EXPECT_EQ(f.handler.disconnect, 0);
}

// ==========================================================================
// T5 - 값 조회: 세대가 맞을 때만 값, 회수되면 0/false (풀 락 안에서 읽는다)
// ==========================================================================

TEST(SessionPoolTest, RemoteIpAndLivenessFollowGeneration)
{
	PoolFixture f;
	const UINT64 sid = f.pool.Allocate();
	f.pool.Find(sid)->SetRemoteIp(0x0100007Fu);   // accept 경로가 심는 값을 흉내
	EXPECT_EQ(f.pool.GetSessionRemoteIp(sid), 0x0100007Fu);
	EXPECT_TRUE(f.pool.IsSessionAlive(sid));
	f.pool.Release(sid);                          // 회수 = 세대 ++
	EXPECT_FALSE(f.pool.IsSessionAlive(sid));
	EXPECT_EQ(f.pool.GetSessionRemoteIp(sid), 0u);
}

// ==========================================================================
// T4 - 첫 lease 반납이 teardown 을 일으킨 뒤(리뷰 M-6 의 순서), 옛 sid 로 내리는 두 번째 명령은 세대에 막혀 no-op
// ==========================================================================

TEST(SessionPoolTest, SecondCommandAfterTeardownIsNoOp)
{
	PoolFixture f;
	const UINT64 sid = f.pool.Allocate();
	Session* s = f.pool.Find(sid);
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->AcquireIoLease(), 1);   // recv 하나가 걸린 척 (pendingIo 1)

	// 소켓이 없으니 WSASend 가 실패한다 -> Send 안에서 Disconnect(SEND_FAIL) + 자기 +1 되돌림. 반환 false.
	EXPECT_FALSE(f.pool.SendTo(sid, kTiny, sizeof(kTiny), ESendDropPolicy::KEEP_CONNECTION));
	EXPECT_EQ(f.handler.disconnect, 0);   // 우리가 건 "recv" 1 이 아직 남아 teardown 은 안 일어났다

	// 그 recv 가 완료된 척 - 마지막 -1 이 0 을 만들어 종료 통지 + 슬롯 반납(세대 ++)
	s->DecPendingIo();
	EXPECT_EQ(f.handler.disconnect, 1);
	EXPECT_EQ(f.handler.lastDisconnectSid, sid);
	// (사유는 소켓이 있어야 기록된다 - Disconnect 가 소켓 INVALID 면 사유 대입 전에 돌아온다. 소켓 없는 이 테스트는 사유를 안 댄다)

	// 옛 sid 로 내리는 두 번째 명령 - 세대 불일치라 아무것도 안 한다 (재사용된 슬롯의 새 주인을 끊지 않는다)
	EXPECT_FALSE(f.pool.DisconnectTo(sid, EDisconnectReason::LOGIN_REJECTED));
	EXPECT_FALSE(f.pool.SendTo(sid, kTiny, sizeof(kTiny), ESendDropPolicy::KEEP_CONNECTION));
	EXPECT_EQ(f.handler.disconnect, 1);   // 통지도 한 번뿐
}

// ==========================================================================
// T7 · T6 - 큐 넘침 처분: EVICT 는 그 자리에서 SEND_TIMEOUT 로 끊고 축출을 센다(세션당 1회) · KEEP 은 버리기만 한다
// ==========================================================================

TEST(SessionPoolTest, QueueOverflowWithEvictPolicyDisconnectsAndCountsOnce)
{
	PoolFixture f;
	const UINT64 sid = f.pool.Allocate();
	Session* s = f.pool.Find(sid);
	ASSERT_NE(s, nullptr);
	s->AcquireIoLease();                                   // 살아 있는 세션 흉내 (recv 1)

	std::vector<BYTE> huge(static_cast<size_t>(SEND_BUFFER_SIZE) + 16, 0xAB);   // 링버퍼(16K)에 안 들어가는 크기 -> Enqueue 실패 = drop
	EXPECT_EQ(f.pool.GetSendDropEvictTotal(), 0ull);
	EXPECT_FALSE(f.pool.SendTo(sid, huge.data(), static_cast<int>(huge.size()), ESendDropPolicy::EVICT_ON_DROP));
	EXPECT_EQ(f.pool.GetSendDropEvictTotal(), 1ull);       // 넘친 그 자리에서 Disconnect(SEND_TIMEOUT) 가 CAS 를 통과하며 1회
	EXPECT_EQ(f.handler.disconnect, 0);                    // 우리 "recv" 가 남아 teardown 은 아직

	// 같은 세션에 두 번째 넘침 - 종료 진입한 세션이라 송신 생략, 집계는 그대로 1 (세션당 1회)
	EXPECT_FALSE(f.pool.SendTo(sid, huge.data(), static_cast<int>(huge.size()), ESendDropPolicy::EVICT_ON_DROP));
	EXPECT_EQ(f.pool.GetSendDropEvictTotal(), 1ull);

	s->DecPendingIo();                                     // recv 완료 척 -> 종료 통지
	EXPECT_EQ(f.handler.disconnect, 1);
}

TEST(SessionPoolTest, QueueOverflowWithKeepPolicyOnlyDrops)
{
	PoolFixture f;
	const UINT64 sid = f.pool.Allocate();
	Session* s = f.pool.Find(sid);
	ASSERT_NE(s, nullptr);
	s->AcquireIoLease();

	std::vector<BYTE> huge(static_cast<size_t>(SEND_BUFFER_SIZE) + 16, 0xCD);
	EXPECT_FALSE(f.pool.SendTo(sid, huge.data(), static_cast<int>(huge.size()), ESendDropPolicy::KEEP_CONNECTION));
	EXPECT_EQ(f.pool.GetSendDropEvictTotal(), 0ull);       // 버리기만 - 연결 유지
	EXPECT_TRUE(f.pool.IsSessionAlive(sid));

	SessionSendMetrics m; s->GetSendMetrics(m);
	EXPECT_EQ(m.dropCount, 1u);                            // 버린 것은 세션 지표에 남는다 (축출 집계와 별개)

	s->DecPendingIo();
}
