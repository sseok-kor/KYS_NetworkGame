#include "pch_tests.h"

#include "Protocol/CPacket.h"
#include "Protocol/GamePackets.h"
#include "Types/Defines.h"

#include <string.h>

using namespace KYS::GAMECOMMON::PROTOCOL;

// ==========================================================================
// 컴파일 타임 계약: 패킷 하나의 상한 = 수신측이 받아 주는 크기 (Defines.h 의 static_assert 와 같은 등식을 테스트가 다시 댄다)
// ==========================================================================

static_assert(MAX_PACKET_SIZE == RECV_BUFFER_SIZE, "송신 CPacket 용량은 수신 버퍼와 같아야 End()==true 가 곧 수신 허용이 된다");
static_assert(MAX_PACKET_SIZE <= 65535,            "길이 접두가 USHORT");

// ==========================================================================
// End() 의 두 얼굴 - 정상이면 길이 자리를 채우고 true, 넘쳤으면 길이 자리를 0 으로 둔 채 false
// ==========================================================================

// 정상 패킷: End() 가 true 를 돌려주고 맨 앞 2 바이트(BE)에 전체 길이를 채운다
TEST(CPacketTest, EndTrueBackfillsLength)
{
	CPacket p(64);
	p.Begin(0x1234);
	p.Write(static_cast<BYTE>(7));
	EXPECT_TRUE(p.End());
	EXPECT_TRUE(p.IsGood());
	EXPECT_EQ(p.GetSize(), 5);                                  // 길이 2 + 종류 2 + 페이로드 1
	EXPECT_EQ(p.GetBuffer()[0], static_cast<BYTE>(0x00));       // BE 길이 = 0x0005
	EXPECT_EQ(p.GetBuffer()[1], static_cast<BYTE>(0x05));
	EXPECT_EQ(p.GetType(), static_cast<USHORT>(0x1234));
}

// 넘친 패킷: End() 가 false 이고 길이 자리는 Begin 이 비워 둔 0 그대로다.
//   이것이 호출부가 반환값을 검사해야 하는 이유 - 그대로 보내면 [len=0] 프레임이 나가 받는 쪽 Session 이 연결을 끊는다.
TEST(CPacketTest, EndFalseLeavesLengthZeroOnOverflow)
{
	CPacket p(8);                                               // 헤더 4 + 여유 4
	p.Begin(0x0001);
	for (int i = 0; i < 8; ++i) { p.Write(static_cast<BYTE>(i)); }   // 5번째부터 넘친다
	EXPECT_FALSE(p.IsGood());
	EXPECT_FALSE(p.End());
	EXPECT_EQ(p.GetBuffer()[0], static_cast<BYTE>(0));          // 길이 backfill 도 sticky 실패에 막혀 0 그대로
	EXPECT_EQ(p.GetBuffer()[1], static_cast<BYTE>(0));
	EXPECT_EQ(p.GetSize(), 8);                                  // 넘친 뒤의 쓰기는 전부 무시 - 용량에서 멈춘다
}

// Reset 뒤에는 같은 버퍼로 다시 정상 패킷을 만들 수 있다 (풀 재사용 버퍼가 첫 실패로 영구 불능이 되면 안 된다)
TEST(CPacketTest, ResetRecoversAfterOverflow)
{
	CPacket p(8);
	p.Begin(0x0001);
	for (int i = 0; i < 8; ++i) { p.Write(static_cast<BYTE>(i)); }
	EXPECT_FALSE(p.End());
	p.Reset();
	p.Begin(0x0002);
	p.Write(static_cast<BYTE>(1));
	EXPECT_TRUE(p.End());
	EXPECT_EQ(p.GetSize(), 5);
}

// ==========================================================================
// 스키마 상한 - 지금 가장 큰 패킷들이 MAX_PACKET_SIZE 안에 든다 (이 테스트가 깨지면 상수를 올리기 전에 수신측을 먼저 본다)
// ==========================================================================

// IS_ONLINE_SYNC 는 스키마에서 가장 큰 패킷 (500 엔트리 x 12 B = 6,000 B) - 8,192 안에 든다
TEST(CPacketTest, LargestPacketFitsMaxPacketSize)
{
	IS_ONLINE_SYNC sync{};
	sync.count = IS_ONLINE_SYNC::MAX_ENTRIES;
	for (int i = 0; i < IS_ONLINE_SYNC::MAX_ENTRIES; ++i) { sync.accountIds[i] = 0xFFFFFFFFu; sync.sids[i] = ~0ull; }

	CPacket p(MAX_PACKET_SIZE);
	p.Begin(0x9001);
	sync.Serialize(p);
	EXPECT_TRUE(p.End());
	EXPECT_EQ(p.GetSize(), 4 + 2 + IS_ONLINE_SYNC::MAX_ENTRIES * 12);
	EXPECT_LE(p.GetSize(), MAX_PACKET_SIZE);
}

// SC_INVENTORY 를 가득 채워도(24 + 2 슬롯) 8,192 안에 든다
TEST(CPacketTest, FullInventoryFitsMaxPacketSize)
{
	SC_INVENTORY inv{};
	inv.count = static_cast<BYTE>(SC_INVENTORY::MAX_ENTRIES);
	for (int i = 0; i < SC_INVENTORY::MAX_ENTRIES; ++i)
	{
		inv.items[i].slot = static_cast<BYTE>(i); inv.items[i].uid = ~0ull; inv.items[i].templateId = -1; inv.items[i].quantity = -1;
	}
	CPacket p(MAX_PACKET_SIZE);
	p.Begin(0x9002);
	inv.Serialize(p);
	EXPECT_TRUE(p.End());
	EXPECT_LE(p.GetSize(), MAX_PACKET_SIZE);
}

// 채팅 계열 최대 - 이름 16 + 본문 80 wchar 를 다 채워도 8,192 에 한참 못 미친다
TEST(CPacketTest, MaxChatFitsMaxPacketSize)
{
	SC_CHAT_BROADCAST chat{};
	for (int i = 0; i < WHISPER_NAME_MAX - 1; ++i) { chat.senderName[i] = L'가'; }
	for (int i = 0; i < CHAT_MSG_MAX - 1; ++i)     { chat.message[i]    = L'나'; }
	CPacket p(MAX_PACKET_SIZE);
	p.Begin(0x9003);
	chat.Serialize(p);
	EXPECT_TRUE(p.End());
	EXPECT_LT(p.GetSize(), 512);
}

// ==========================================================================
// 빌더 헬퍼 - Build*Packet 이 End() 결과를 그대로 돌려준다 (호출자가 검사할 값)
// ==========================================================================

TEST(CPacketTest, BuildDamagePacketReportsOverflow)
{
	CPacket ok(MAX_PACKET_SIZE);
	EXPECT_TRUE(BuildDamagePacket(ok, 1, 2, 3, 4));

	CPacket tiny(6);                                            // 헤더 4 + 2 - SC_DAMAGE 본체(16 B)가 안 들어간다
	EXPECT_FALSE(BuildDamagePacket(tiny, 1, 2, 3, 4));
	EXPECT_EQ(tiny.GetBuffer()[0], static_cast<BYTE>(0));
	EXPECT_EQ(tiny.GetBuffer()[1], static_cast<BYTE>(0));
}

TEST(CPacketTest, BuildDeathPacketReportsOverflow)
{
	CPacket ok(MAX_PACKET_SIZE);
	EXPECT_TRUE(BuildDeathPacket(ok, 42));

	CPacket tiny(5);
	EXPECT_FALSE(BuildDeathPacket(tiny, 42));
}
