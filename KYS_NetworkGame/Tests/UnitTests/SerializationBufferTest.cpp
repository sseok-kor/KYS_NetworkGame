#include "pch_tests.h"

#include "Protocol/SerializationBuffer.h"

#include <string.h>
#include <type_traits>

using namespace KYS::GAMESERVER::PROTOCOL;

// ==========================================================================
// 컴파일 타임 계약: 복사 2줄 =delete(이동 폴백 차단) + polymorphic base
// ==========================================================================

static_assert(!std::is_copy_constructible<SerializationBuffer>::value, "복사 생성 금지");
static_assert(!std::is_copy_assignable<SerializationBuffer>::value,    "복사 대입 금지");
static_assert(!std::is_move_constructible<SerializationBuffer>::value, "이동 생성 금지(복사 삭제 폴백)");
static_assert(!std::is_move_assignable<SerializationBuffer>::value,    "이동 대입 금지(복사 삭제 폴백)");
static_assert(std::has_virtual_destructor<SerializationBuffer>::value, "SerialBuf 상속 base - virtual ~ 필수");

// ==========================================================================
// 왕복 + BE wire 레이아웃 + operator/Put/Reset
// ==========================================================================

// BYTE 1개 왕복 - 값 보존 + 크기 1바이트
TEST(SerializationBufferTest, ByteRoundTrip)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<BYTE>(0x42));
	EXPECT_EQ(buf.GetSize(), 1);
	EXPECT_EQ(buf.GetBuffer()[0], static_cast<BYTE>(0x42));

	BYTE out = 0;
	buf.Read(out);
	EXPECT_EQ(out, static_cast<BYTE>(0x42));
	EXPECT_EQ(buf.GetReadOffset(), 1);
}

// USHORT 2바이트 - 빅엔디안 wire 순서 (0x0102 -> [01 02])
TEST(SerializationBufferTest, UShortBigEndianWire)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<USHORT>(0x0102));
	EXPECT_EQ(buf.GetSize(), 2);

	BYTE* wire = buf.GetBuffer();
	EXPECT_EQ(wire[0], 0x01);   // 상위 바이트 먼저 = 네트워크 바이트순서
	EXPECT_EQ(wire[1], 0x02);

	USHORT out = 0;
	buf.Read(out);
	EXPECT_EQ(out, static_cast<USHORT>(0x0102));
}

// UINT 4바이트 - 빅엔디안 wire 순서 (0x01020304 -> [01 02 03 04])
TEST(SerializationBufferTest, UIntBigEndianWire)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<UINT>(0x01020304));
	EXPECT_EQ(buf.GetSize(), 4);

	BYTE* wire = buf.GetBuffer();
	EXPECT_EQ(wire[0], 0x01);
	EXPECT_EQ(wire[1], 0x02);
	EXPECT_EQ(wire[2], 0x03);
	EXPECT_EQ(wire[3], 0x04);

	UINT out = 0;
	buf.Read(out);
	EXPECT_EQ(out, 0x01020304u);
}

// int 음수/최대값 왕복 - 부호 보존
TEST(SerializationBufferTest, IntSignedRoundTrip)
{
	SerializationBuffer buf(64);
	buf.Write(-1);
	buf.Write(0x7FFFFFFF);   // INT_MAX
	buf.Write(0);

	int a = 0, b = 0, c = 123;
	buf.Read(a);
	buf.Read(b);
	buf.Read(c);
	EXPECT_EQ(a, -1);
	EXPECT_EQ(b, 0x7FFFFFFF);
	EXPECT_EQ(c, 0);
}

// operator<< / >> 체이닝 - Write/Read 와 동치
TEST(SerializationBufferTest, OperatorChaining)
{
	SerializationBuffer buf(64);
	buf << static_cast<BYTE>(0x01)
	    << static_cast<USHORT>(0x0203)
	    << static_cast<UINT>(0x04050607);

	BYTE a = 0; USHORT b = 0; UINT c = 0;
	buf >> a >> b >> c;
	EXPECT_EQ(a, static_cast<BYTE>(0x01));
	EXPECT_EQ(b, static_cast<USHORT>(0x0203));
	EXPECT_EQ(c, 0x04050607u);
}

// Put - 이미 쓴 자리 backfill (패킷 앞 길이), writePos 불변 + BE
TEST(SerializationBufferTest, PutBackfillsLengthSlot)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<USHORT>(0));         // 길이 자리 예약 (pos 0)
	buf.Write(static_cast<UINT>(0xAABBCCDD));  // payload
	const int totalLen = buf.GetSize();        // 6

	buf.Put(0, static_cast<USHORT>(totalLen));

	EXPECT_EQ(buf.GetSize(), 6);               // Put 은 writePos 를 안 움직임
	BYTE* wire = buf.GetBuffer();
	EXPECT_EQ(wire[0], 0x00);                  // 6 의 BE = [00 06]
	EXPECT_EQ(wire[1], 0x06);
}

// Reset - 위치만 0, 버퍼 메모리/용량은 유지 (풀 재사용)
TEST(SerializationBufferTest, ResetKeepsBuffer)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<UINT>(0x12345678));
	BYTE* before = buf.GetBuffer();

	buf.Reset();
	EXPECT_EQ(buf.GetSize(), 0);
	EXPECT_EQ(buf.GetReadOffset(), 0);
	EXPECT_EQ(buf.GetBuffer(), before);        // 같은 버퍼 재사용

	buf.Write(static_cast<BYTE>(0x99));         // 재사용 가능
	EXPECT_EQ(buf.GetSize(), 1);
}

// ==========================================================================
// 문자열 정상 경로 (ASCII / 한글 / 빈 / 연속)
// ==========================================================================

// ASCII 문자열 왕복
TEST(SerializationBufferTest, StringAsciiRoundTrip)
{
	SerializationBuffer buf(256);
	buf.WriteString(L"Hello", 6);

	wchar_t dest[64] = {};
	buf.ReadString(dest, 64);
	EXPECT_STREQ(dest, L"Hello");
}

// 한글(wchar_t) 왕복 - 각 글자를 USHORT BE 로 (파일 UTF-8 BOM 필요)
TEST(SerializationBufferTest, StringKoreanRoundTrip)
{
	SerializationBuffer buf(256);
	buf.WriteString(L"안녕하세요", 6);

	wchar_t dest[64] = {};
	buf.ReadString(dest, 64);
	EXPECT_STREQ(dest, L"안녕하세요");
}

// 빈 문자열 - 길이 0 만 쓰고 dest[0] 널
TEST(SerializationBufferTest, StringEmpty)
{
	SerializationBuffer buf(64);
	buf.WriteString(L"", 1);

	wchar_t dest[64];
	dest[0] = L'X';                    // 널이 실제로 써지는지 보려 오염
	buf.ReadString(dest, 64);
	EXPECT_STREQ(dest, L"");
	EXPECT_EQ(dest[0], L'\0');
}

// 여러 문자열 연속 - readPos 가 정확히 전진해 다음 문자열이 안 어긋남
TEST(SerializationBufferTest, StringSequential)
{
	SerializationBuffer buf(256);
	buf.WriteString(L"first", 6);
	buf.WriteString(L"second", 7);

	wchar_t a[64] = {};
	wchar_t b[64] = {};
	buf.ReadString(a, 64);
	buf.ReadString(b, 64);
	EXPECT_STREQ(a, L"first");
	EXPECT_STREQ(b, L"second");        // b 가 맞으면 a 읽은 뒤 readPos 정확히 전진한 것
}

// ==========================================================================
// 신뢰 경계 방어 - Read 가드 + ReadString 2단계 clamp (보안 핵심)
// ==========================================================================

// 빈 버퍼 Read - 가드가 out=0, readPos 미전진 (Release 에서도 생존)
TEST(SerializationBufferTest, ReadOnEmptyBufferIsGuarded)
{
	SerializationBuffer buf(64);           // 아무것도 안 씀
	BYTE b = 0xFF;
	buf.Read(b);
	EXPECT_EQ(b, 0);                        // 가드가 0 대입
	EXPECT_EQ(buf.GetReadOffset(), 0);      // 위치 안 움직임
}

// 짧은 버퍼 Read - 남은 바이트보다 크게 읽으면 0 + 미전진
TEST(SerializationBufferTest, ReadPastEndIsGuarded)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<BYTE>(0x42));    // 1바이트만

	USHORT s = 0xFFFF;                      // 2바이트 필요
	buf.Read(s);
	EXPECT_EQ(s, 0);
	EXPECT_EQ(buf.GetReadOffset(), 0);      // 실패 read 는 전진 안 함
}

// Read(void*, size) 가드 - 남은 것보다 크면 0 으로 채우고 미전진
TEST(SerializationBufferTest, ReadBlockPastEndZeroFills)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<USHORT>(0x1234)); // 2바이트

	BYTE dest[10];
	memset(dest, 0xAB, sizeof(dest));
	buf.Read(dest, 10);                     // 10 > 남은 2
	for (int i = 0; i < 10; ++i)
	{
		EXPECT_EQ(dest[i], 0);              // 전부 0
	}
	EXPECT_EQ(buf.GetReadOffset(), 0);      // 미전진
}

// ReadString clamp #1 - 적대적 큰 길이 선언 -> 남은 글자수로 clamp (OOB read 방어)
TEST(SerializationBufferTest, ReadStringClampsHostileLength)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<USHORT>(1000));  // "1000 글자" 라고 거짓 선언
	buf.Write(static_cast<USHORT>(L'A'));  // 실제론 2 글자
	buf.Write(static_cast<USHORT>(L'B'));

	wchar_t dest[64] = {};
	buf.ReadString(dest, 64);              // 크래시/OOB 없이 남은 2 로 clamp
	EXPECT_STREQ(dest, L"AB");
}

// ReadString clamp #2 - dest 가 작으면 maxLen-1 truncate + readPos 는 full 선언길이 전진
TEST(SerializationBufferTest, ReadStringClampsSmallDestAndStaysAligned)
{
	SerializationBuffer buf(256);
	buf.WriteString(L"HELLO", 6);             // 5 글자
	buf.Write(static_cast<UINT>(0xABCD));  // 다음 필드

	wchar_t small[3] = {};                 // 2 글자 + 널만 담김
	buf.ReadString(small, 3);
	EXPECT_STREQ(small, L"HE");            // truncate

	UINT next = 0;
	buf.Read(next);                        // readPos 가 "HELLO" 전체를 지나쳤어야 정렬
	EXPECT_EQ(next, 0xABCDu);
}

// ---------------------------------------------------------------------------
// [정정] 위 자리에는 "Write 오버플로는 _ASSERTE 라 실행 안 함" 이라는 문단이 있었다.
//   그 서술은 낡았다 - Write 5종과 Put 은 런타임 가드 + m_failed 로 바뀌었고
//   그 자리 주석이 사유까지 적는다 ("_ASSERTE 는 Release 에서 사라져 힙 침범이 되므로
//   읽기 경로와 같은 형태로 대체"). 그래서 이제 실제로 돌릴 수 있다. 아래가 그것이다.
// ---------------------------------------------------------------------------

// ==========================================================================
// 쓰기 실패 경로 - m_failed 와 IsGood
// ==========================================================================

// 용량 초과 쓰기는 abort 하지 않고 실패 표시만 남긴다
TEST(SerializationBufferTest, WriteOverflowSetsFailedNotAssert)
{
	SerializationBuffer buf(4);
	buf.Write(static_cast<UINT>(0x11223344));
	EXPECT_EQ(buf.GetSize(), 4);
	EXPECT_TRUE(buf.IsGood());

	buf.Write(static_cast<BYTE>(0xFF));    // 들어갈 자리 없음
	EXPECT_EQ(buf.GetSize(), 4);           // 안 늘어남
	EXPECT_FALSE(buf.IsGood());            // 실패가 남음
}

// 첫 실패 뒤에는 자리가 남아 있어도 안 쓴다 (버퍼가 첫 실패 직전 상태로 언다)
TEST(SerializationBufferTest, WriteFreezesAfterFirstFailure)
{
	SerializationBuffer buf(4);
	buf.Write(static_cast<BYTE>(0xAA));
	buf.Write(static_cast<UINT>(0xDEADBEEF));   // 실패 (1 + 4 > 4)
	EXPECT_FALSE(buf.IsGood());

	buf.Write(static_cast<BYTE>(0xBB));         // 자리는 3바이트 남았지만
	EXPECT_EQ(buf.GetSize(), 1);                // 얼어 있어 안 씀
	EXPECT_EQ(buf.GetBuffer()[0], static_cast<BYTE>(0xAA));
}

// Reset 이 실패 표시를 내려 풀 재사용 버퍼가 영구 불능이 되지 않는다
TEST(SerializationBufferTest, ResetClearsFailedFlag)
{
	SerializationBuffer buf(4);
	buf.Write(static_cast<UINT>(0x11223344));
	buf.Write(static_cast<BYTE>(0xFF));
	EXPECT_FALSE(buf.IsGood());

	buf.Reset();
	EXPECT_EQ(buf.GetSize(), 0);
	EXPECT_EQ(buf.GetReadOffset(), 0);
	EXPECT_TRUE(buf.IsGood());

	buf.Write(static_cast<BYTE>(0x99));
	EXPECT_EQ(buf.GetSize(), 1);
}

// Put 이 아직 안 쓴 영역을 가리키면 실패로 처리한다 (기존 테스트는 정상 backfill 만 덮었다)
TEST(SerializationBufferTest, PutOutOfRangeSetsFailed)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<BYTE>(0x11));
	buf.Put(0, 0xABCD);                    // 0+2 > writePos 1
	EXPECT_FALSE(buf.IsGood());
	EXPECT_EQ(buf.GetSize(), 1);

	buf.Write(static_cast<BYTE>(0x22));    // 얼었으니 무시
	EXPECT_EQ(buf.GetSize(), 1);
}

// 문자열 도중 오버플로 - 선언 length 와 실제 기록 글자 수가 갈리는 유일한 경로
TEST(SerializationBufferTest, WriteStringOverflowDeclaresMoreThanWritten)
{
	SerializationBuffer buf(10);
	buf.WriteString(L"ABCDE", 6);          // 선언 5글자 = 2 + 10 = 12바이트 필요, 용량 10

	EXPECT_EQ(buf.GetSize(), 10);
	EXPECT_FALSE(buf.IsGood());

	const BYTE* w = buf.GetBuffer();
	const USHORT declared = static_cast<USHORT>((w[0] << 8) | w[1]);
	EXPECT_EQ(declared, 5);                          // 선언은 5
	EXPECT_EQ((buf.GetSize() - 2) / 2, 4);           // 실제 기록은 4 - 어긋난다
}

// maxLen 은 배열 크기(널 자리 포함)이고 최대 maxLen-1 글자만 쓴다 - ReadString 과 같은 뜻
TEST(SerializationBufferTest, WriteStringMaxLenIsArraySize)
{
	SerializationBuffer buf(64);
	buf.WriteString(L"ABCDE", 4);          // 3글자까지

	const BYTE* w = buf.GetBuffer();
	const USHORT declared = static_cast<USHORT>((w[0] << 8) | w[1]);
	EXPECT_EQ(declared, 3);
	EXPECT_EQ(buf.GetSize(), 8);           // 2 + 3*2
}

// ==========================================================================
// 읽기 가드의 성질 - sticky 하지 않다
// ==========================================================================

// 인자 sanity 분기 (프로덕션 호출자가 0 이라 한 번도 안 밟힌 경로)
TEST(SerializationBufferTest, ReadBlockRejectsNullAndZeroSize)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<UINT>(0xAABBCCDD));

	buf.Read(nullptr, 4);
	EXPECT_EQ(buf.GetReadOffset(), 0);

	BYTE dest[4];
	memset(dest, 0x09, sizeof(dest));
	buf.Read(dest, 0);
	EXPECT_EQ(buf.GetReadOffset(), 0);
	EXPECT_EQ(dest[0], static_cast<BYTE>(0x09));   // memset 안 함
}

// [현행 동작 고정] 가드가 sticky 하지 않아, 실패한 큰 필드의 바이트를 뒤의 작은 필드가 값으로 가져간다
//   ServerEntry(BYTE serverId, UINT ip, USHORT port, ...) 모양에 payload 3바이트만 온 경우.
TEST(SerializationBufferTest, ReadGuardIsNotStickyFieldShift)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<BYTE>(0xAA));
	buf.Write(static_cast<BYTE>(0xBB));
	buf.Write(static_cast<BYTE>(0xCC));

	BYTE   serverId = 0;  buf.Read(serverId);
	UINT   ip       = 0;  buf.Read(ip);
	USHORT port     = 0;  buf.Read(port);
	USHORT pop      = 0;  buf.Read(pop);

	EXPECT_EQ(serverId, static_cast<BYTE>(0xAA));
	EXPECT_EQ(ip, 0u);                              // 가드 히트 - 커서 안 움직임
	EXPECT_EQ(port, static_cast<USHORT>(0xBBCC));   // ip 가 실패한 바로 그 바이트를 읽는다
	EXPECT_EQ(pop, 0);
	EXPECT_TRUE(buf.IsGood());                      // 아무도 실패를 모른다
}

// 읽기가 가드에 걸린 것을 호출자가 물을 수 있다 (IsReadGood 신설로 켜진 테스트)
//   쓰기 창구(IsGood)와 갈라 둔 것이 이 테스트의 요점이다 - 읽기 실패는 IsGood 을 안 건드린다.
TEST(SerializationBufferTest, ReadFailureIsObservable)
{
	SerializationBuffer buf(64);
	UINT v = 0;
	buf.Read(v);
	EXPECT_EQ(v, 0u);
	EXPECT_FALSE(buf.IsReadGood());   // 읽기 실패는 여기로
	EXPECT_TRUE(buf.IsGood());        // 쓰기 창구는 안 건드린다 (LoginHandler 의 "서버 버그" 계약 보존)
}

// 성공한 읽기만으로는 표시가 안 선다 (거짓 양성 없음)
TEST(SerializationBufferTest, ReadGoodStaysTrueOnCleanRead)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<UINT>(0x11223344));
	buf.Write(static_cast<BYTE>(0x55));

	UINT a = 0;  buf.Read(a);
	BYTE b = 0;  buf.Read(b);
	EXPECT_EQ(a, 0x11223344u);
	EXPECT_EQ(b, static_cast<BYTE>(0x55));
	EXPECT_TRUE(buf.IsReadGood());
}

// Reset 이 읽기 표시도 내린다 - 안 내리면 재사용 버퍼가 첫 절단 패킷 하나로 영구 불능이 된다
//   (클라 m_parsePkt / 봇 m_parsePkt 가 Reset 으로 재사용된다)
TEST(SerializationBufferTest, ResetClearsReadFailedFlag)
{
	SerializationBuffer buf(64);
	UINT v = 0;
	buf.Read(v);                       // 빈 버퍼 - 가드 발화
	EXPECT_FALSE(buf.IsReadGood());

	buf.Reset();
	EXPECT_TRUE(buf.IsReadGood());     // 다시 쓸 수 있어야 한다

	buf.Write(static_cast<UINT>(0xCAFEBABE));
	UINT w = 0;  buf.Read(w);
	EXPECT_EQ(w, 0xCAFEBABEu);
	EXPECT_TRUE(buf.IsReadGood());
}

// 선언 길이가 버퍼에 남은 글자보다 크면 표시가 선다 - 정직한 피어는 이 값을 만들 수 없다
//   (선언 L 이면 그 뒤 필드 T 를 더해 remainChars = L + floor(T/2) >= L 이라 항상 통과한다)
TEST(SerializationBufferTest, OverDeclaredStringLengthIsFlagged)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<USHORT>(1000));      // 거짓 선언
	buf.Write(static_cast<USHORT>(L'a'));
	buf.Write(static_cast<USHORT>(L'b'));

	wchar_t dest[16] = {};
	buf.ReadString(dest, 16);
	EXPECT_STREQ(dest, L"ab");                 // 값은 지금처럼 잘라 담는다 (동작 무변경)
	EXPECT_FALSE(buf.IsReadGood());            // 다만 "선언이 거짓이었다" 는 남는다
}

// 정직한 문자열은 표시를 안 세운다 - 뒤에 다른 필드가 붙어 있어도 (거짓 양성 없음)
TEST(SerializationBufferTest, HonestStringWithTrailingFieldStaysGood)
{
	SerializationBuffer buf(64);
	buf.WriteString(L"hello", 16);
	buf.Write(static_cast<UINT>(0xDEADBEEF));

	wchar_t dest[16] = {};
	buf.ReadString(dest, 16);
	UINT tail = 0;  buf.Read(tail);

	EXPECT_STREQ(dest, L"hello");
	EXPECT_EQ(tail, 0xDEADBEEFu);
	EXPECT_TRUE(buf.IsReadGood());
}

// [바라는 동작 - 아직 안 넣음] 한 필드가 가드에 걸렸으면 뒤 필드도 값을 안 받아야 한다
//   이것을 넣으면 위 ReadGuardIsNotStickyFieldShift 와 TruncatedLengthFieldLeavesByteForNextField 가
//   함께 깨진다. 그 둘은 현행 동작을 박아 둔 것이라 깨지는 것이 곧 고쳐졌다는 뜻이다.
TEST(SerializationBufferTest, DISABLED_ReadGuardIsSticky)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<BYTE>(0xAA));
	buf.Write(static_cast<BYTE>(0xBB));
	buf.Write(static_cast<BYTE>(0xCC));

	BYTE   serverId = 0;  buf.Read(serverId);
	UINT   ip       = 0;  buf.Read(ip);
	USHORT port     = 0;  buf.Read(port);

	EXPECT_EQ(port, 0);   // 실제로는 0xBBCC
}

// ==========================================================================
// skip 루프가 실제로 값을 하는 자리
// ==========================================================================

// 그릇이 작아 버린 글자를 skip 이 정확히 소비해 다음 필드를 지킨다
TEST(SerializationBufferTest, SkipLoopRealignsWhenDestTooSmall)
{
	SerializationBuffer buf(256);
	buf.Write(static_cast<USHORT>(20));
	for (int i = 0; i < 20; ++i) { buf.Write(static_cast<USHORT>(L'A' + i)); }
	buf.Write(static_cast<UINT>(0xDEADBEEF));
	const int total = buf.GetSize();
	EXPECT_EQ(total, 46);

	wchar_t dest[16] = {};
	buf.ReadString(dest, 16);
	EXPECT_STREQ(dest, L"ABCDEFGHIJKLMNO");   // 15 글자

	UINT next = 0;
	buf.Read(next);
	EXPECT_EQ(next, 0xDEADBEEFu);             // skip 이 돌았다는 증거
	EXPECT_EQ(buf.GetReadOffset(), total);
}

// 잔여 clamp 가 먼저 걸린 뒤에도 그릇 clamp 가 남으면 skip 이 실제로 돈다
TEST(SerializationBufferTest, SkipLoopStillRunsAfterRemainClamp)
{
	SerializationBuffer buf(256);
	buf.Write(static_cast<USHORT>(1000));     // 거짓 선언
	for (int i = 0; i < 30; ++i) { buf.Write(static_cast<USHORT>(L'a')); }
	const int total = buf.GetSize();
	EXPECT_EQ(total, 62);

	wchar_t dest[16] = {};
	buf.ReadString(dest, 16);
	EXPECT_EQ(static_cast<int>(wcslen(dest)), 15);
	EXPECT_EQ(buf.GetReadOffset(), total);    // 1000 -> 30 clamp 후 15 담고 15 skip
}

// ==========================================================================
// [현행 동작 고정] 공격자가 length 하나로 필드 경계를 민다
// ==========================================================================

// length 필드 자체가 절단되면 남은 1바이트를 다음 BYTE 필드가 먹는다
TEST(SerializationBufferTest, TruncatedLengthFieldLeavesByteForNextField)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<BYTE>(0x7E));       // payload 1바이트

	wchar_t dest[16] = {};
	buf.ReadString(dest, 16);
	EXPECT_STREQ(dest, L"");
	EXPECT_EQ(buf.GetReadOffset(), 0);        // 커서가 안 움직였다

	BYTE b = 0;
	buf.Read(b);
	EXPECT_EQ(b, static_cast<BYTE>(0x7E));    // 문자열의 잔해를 값으로 읽는다
	EXPECT_EQ(buf.GetReadOffset(), 1);
}

// 홀수 잔여의 마지막 1바이트가 문자열에서 떨어져 나와 다음 BYTE 필드의 값이 된다
TEST(SerializationBufferTest, OddTailByteEatenByNextByteField)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<USHORT>(1));
	buf.Write(static_cast<USHORT>(L'A'));
	buf.Write(static_cast<BYTE>(0x63));       // 홀수 꼬리
	const int total = buf.GetSize();
	EXPECT_EQ(total, 5);

	wchar_t dest[16] = {};
	buf.ReadString(dest, 16);
	EXPECT_STREQ(dest, L"A");
	EXPECT_EQ(buf.GetReadOffset(), 4);

	BYTE b = 0;
	buf.Read(b);
	EXPECT_EQ(b, static_cast<BYTE>(0x63));
	EXPECT_EQ(buf.GetReadOffset(), total);
}

// 정직한 선언과 과대 선언 둘 다에서 버림 나눗셈과 clamp 가 상쇄해 뒤 BYTE 가 온전하다
TEST(SerializationBufferTest, OddTailHonestDeclarationStaysAligned)
{
	for (int variant = 0; variant < 2; ++variant)
	{
		SerializationBuffer buf(64);
		buf.Write(static_cast<USHORT>(variant == 0 ? 2 : 3));   // 정직 / 과대
		buf.Write(static_cast<USHORT>(L'a'));
		buf.Write(static_cast<USHORT>(L'b'));
		buf.Write(static_cast<BYTE>(0x01));
		const int total = buf.GetSize();
		EXPECT_EQ(total, 7);

		wchar_t name[16] = {};
		buf.ReadString(name, 16);
		EXPECT_STREQ(name, L"ab") << "variant=" << variant;

		BYTE b = 0;
		buf.Read(b);
		EXPECT_EQ(b, static_cast<BYTE>(1)) << "variant=" << variant;
		EXPECT_EQ(buf.GetReadOffset(), total) << "variant=" << variant;
	}
}

// [바라는 동작] 패킷을 끝까지 소비하지 않았다는 사실을 호출자가 알 수 있어야 한다
TEST(SerializationBufferTest, DISABLED_TrailingBytesAreDetected)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<USHORT>(2));
	buf.Write(static_cast<USHORT>(L'h'));
	buf.Write(static_cast<USHORT>(L'i'));
	buf.Write(static_cast<BYTE>(0xDE));
	buf.Write(static_cast<BYTE>(0xAD));
	buf.Write(static_cast<BYTE>(0xBE));

	wchar_t dest[16] = {};
	buf.ReadString(dest, 16);
	EXPECT_STREQ(dest, L"hi");
	EXPECT_EQ(buf.GetReadOffset(), buf.GetSize());   // 실제로는 6 != 9 이고 아무도 안 본다
}

// [바라는 동작] wire 의 제어문자가 그대로 wchar_t 로 들어오면 안 된다
TEST(SerializationBufferTest, DISABLED_ReadStringRejectsControlChars)
{
	SerializationBuffer buf(64);
	buf.Write(static_cast<USHORT>(3));
	buf.Write(static_cast<USHORT>(0x000D));
	buf.Write(static_cast<USHORT>(0x000A));
	buf.Write(static_cast<USHORT>(L'X'));

	wchar_t dest[16] = {};
	buf.ReadString(dest, 16);
	EXPECT_EQ(static_cast<int>(wcslen(dest)), 0);    // 실제로는 3 글자 그대로 담긴다
}

