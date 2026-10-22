#include "pch_tests.h"

#include "Core/Config/ConfigParse.h"

#include <cstring>   // strcpy_s / strcmp
#include <cwchar>    // wcsstr

using namespace KYS::GAMESERVER::CONFIG;

// ==========================================================================
// ParseIpv4 - 계약 3가지 (정확히 4옥텟 / 각 옥텟 0..255 / 문자열 전량 소비)
//   conf/*.ini 의 public_ip(서버) 와 host(클라) 가 같은 함수로 검증된다 - 여기서 한 번 고정해 두면 두 로더가 같이 지킨다.
// ==========================================================================

TEST(ConfigParseTest, Ipv4AcceptsDottedAndReturnsHostOrder)
{
	UINT32 ip = 0;
	const wchar_t* reason = nullptr;
	EXPECT_TRUE(ParseIpv4("127.0.0.1", &ip, &reason));
	EXPECT_EQ(ip, 0x7F000001u);
	EXPECT_TRUE(ParseIpv4("172.30.1.78", &ip, &reason));
	EXPECT_EQ(ip, 0xAC1E014Eu);
	EXPECT_TRUE(ParseIpv4("255.255.255.255", &ip, &reason));
	EXPECT_EQ(ip, 0xFFFFFFFFu);
}

// 호스트명은 형식 오류 - DNS 해석은 이 로더의 일이 아니다 (서버 견본 ini 가 "localhost 는 거부" 라 적은 그 계약)
TEST(ConfigParseTest, Ipv4RejectsHostname)
{
	UINT32 ip = 0;
	const wchar_t* reason = nullptr;
	EXPECT_FALSE(ParseIpv4("localhost", &ip, &reason));
	EXPECT_NE(reason, nullptr);
}

TEST(ConfigParseTest, Ipv4RejectsOctetOver255)
{
	UINT32 ip = 0;
	const wchar_t* reason = nullptr;
	EXPECT_FALSE(ParseIpv4("127.0.0.300", &ip, &reason));
	EXPECT_NE(wcsstr(reason, L"0..255"), nullptr);   // 거부 사유가 어느 계약을 어겼는지 말한다
}

// "127.0.0.1x" 와 "1.2.3.4.5" - sscanf_s 를 안 쓰는 이유가 이 두 입력이다 (둘 다 "4개 읽음" 으로 통과시킨다)
TEST(ConfigParseTest, Ipv4RejectsTrailingCharsAndFifthGroup)
{
	UINT32 ip = 0;
	const wchar_t* reason = nullptr;
	EXPECT_FALSE(ParseIpv4("127.0.0.1x", &ip, &reason));
	EXPECT_NE(wcsstr(reason, L"남은 문자"), nullptr);
	EXPECT_FALSE(ParseIpv4("1.2.3.4.5", &ip, &reason));
	EXPECT_NE(wcsstr(reason, L"남은 문자"), nullptr);
	EXPECT_FALSE(ParseIpv4("127.0.0.1 ", &ip, &reason));   // 뒤 공백도 남은 문자 - 공백 제거는 로더의 Trim 몫
}

TEST(ConfigParseTest, Ipv4RejectsTooFewOctetsAndEmptyAndLeadingSpace)
{
	UINT32 ip = 0;
	const wchar_t* reason = nullptr;
	EXPECT_FALSE(ParseIpv4("127.0.0", &ip, &reason));
	EXPECT_FALSE(ParseIpv4("", &ip, &reason));
	EXPECT_FALSE(ParseIpv4(" 127.0.0.1", &ip, &reason));   // 옥텟은 숫자로 시작해야 한다
	EXPECT_FALSE(ParseIpv4("127..0.1", &ip, &reason));
	EXPECT_FALSE(ParseIpv4(nullptr, &ip, &reason));
}

// 0.0.0.0 은 "형식" 으로는 맞다 - 거부는 로더가 값(==0)을 보고 한다. 이 경계를 여기 박아 두어 두 로더가 같은 자리에서 거부하게 한다.
TEST(ConfigParseTest, Ipv4ZeroIsFormatValidValueZero)
{
	UINT32 ip = 1;
	const wchar_t* reason = nullptr;
	EXPECT_TRUE(ParseIpv4("0.0.0.0", &ip, &reason));
	EXPECT_EQ(ip, 0u);
}

// 옥텟은 base 10 고정 - "010" 을 8진 8 로 읽지 않는다
TEST(ConfigParseTest, Ipv4LeadingZeroOctetIsDecimal)
{
	UINT32 ip = 0;
	const wchar_t* reason = nullptr;
	EXPECT_TRUE(ParseIpv4("010.0.0.1", &ip, &reason));
	EXPECT_EQ(ip, 0x0A000001u);
}

// ==========================================================================
// ParseU32 - 전량 소비 + 부호 거부 + 오버플로 거부 + 진법
// ==========================================================================

TEST(ConfigParseTest, U32Base10AcceptsWholeNumberOnly)
{
	UINT32 v = 0;
	EXPECT_TRUE(ParseU32("7777", &v, 10));
	EXPECT_EQ(v, 7777u);
	EXPECT_FALSE(ParseU32("7777x", &v, 10));
	EXPECT_FALSE(ParseU32("", &v, 10));
	EXPECT_FALSE(ParseU32(nullptr, &v, 10));
	EXPECT_FALSE(ParseU32("abc", &v, 10));
}

// "-1" 은 strtoul 이 4294967295 로 되돌리고 전량 소비까지 만족시킨다 - 부호를 앞에서 거부해야 secret=-1 이 non-zero 검증을 못 넘는다
TEST(ConfigParseTest, U32RejectsSignAndOverflow)
{
	UINT32 v = 0;
	EXPECT_FALSE(ParseU32("-1", &v, 10));
	EXPECT_FALSE(ParseU32("+1", &v, 10));
	EXPECT_FALSE(ParseU32("99999999999", &v, 10));   // 32비트 초과 - ERANGE
}

// base 0 = 진법 자동 판별 (secret 전용). 포트에 쓰면 "07777" 이 8진 4095 로 조용히 바뀌므로 포트는 base 10 이어야 한다.
TEST(ConfigParseTest, U32BaseZeroAutoDetectsHexAndOctal)
{
	UINT32 v = 0;
	EXPECT_TRUE(ParseU32("0x1234", &v, 0));
	EXPECT_EQ(v, 0x1234u);
	EXPECT_TRUE(ParseU32("07777", &v, 0));
	EXPECT_EQ(v, 4095u);                 // 8진으로 읽힌다 - 이것이 포트에 base 0 을 못 쓰는 이유
	EXPECT_TRUE(ParseU32("07777", &v, 10));
	EXPECT_EQ(v, 7777u);                 // base 10 이면 그대로
}

// ==========================================================================
// Trim / SkipUtf8Bom - 줄 다듬기
// ==========================================================================

TEST(ConfigParseTest, TrimStripsBothEndsInPlace)
{
	char buf[32];
	strcpy_s(buf, "  \thost=127.0.0.1 \t\r\n");
	EXPECT_STREQ(Trim(buf), "host=127.0.0.1");

	char blank[8];
	strcpy_s(blank, " \t\r\n");
	EXPECT_STREQ(Trim(blank), "");

	char none[8];
	strcpy_s(none, "abc");
	EXPECT_STREQ(Trim(none), "abc");
}

TEST(ConfigParseTest, SkipUtf8BomOnlyWhenPresent)
{
	char withBom[16] = { '\xEF', '\xBB', '\xBF', '[', 'l', 'o', 'g', 'i', 'n', ']', '\0' };
	EXPECT_STREQ(SkipUtf8Bom(withBom), "[login]");

	char without[16];
	strcpy_s(without, "[login]");
	EXPECT_EQ(SkipUtf8Bom(without), without);   // 포인터 그대로
	EXPECT_STREQ(without, "[login]");
}
