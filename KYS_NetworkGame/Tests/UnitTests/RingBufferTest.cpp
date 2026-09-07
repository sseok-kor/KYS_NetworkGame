#include "pch_tests.h"

#include "Types/Defines.h"           // RECV_BUFFER_SIZE (RingBuffer.h 기본 인자가 참조 - 선포함 필요)
#include "Core/Memory/RingBuffer.h"
#include "TestHarness/ConcurrentHammer.h"   // FailFastTestHarness

#include <string.h>
#include <type_traits>

using namespace KYS::GAMESERVER::MEMORY;

// ==========================================================================
// 컴파일 타임 계약: 버퍼는 자기 메모리 단독 소유 (Rule-of-Five 4줄 =delete)
// ==========================================================================

static_assert(!std::is_copy_constructible<RingBuffer>::value, "RingBuffer 복사 생성 금지");
static_assert(!std::is_copy_assignable<RingBuffer>::value,    "RingBuffer 복사 대입 금지");
static_assert(!std::is_move_constructible<RingBuffer>::value, "RingBuffer 이동 생성 금지");
static_assert(!std::is_move_assignable<RingBuffer>::value,    "RingBuffer 이동 대입 금지");

// ==========================================================================
// 정확성 (단일스레드): wrap 양방향 / full, empty 경계 / Peek / zero-copy 동치
// ==========================================================================

// wrap 강제: readPos/writePos를 끝 근처로 민 뒤 seam(끝-시작 2조각)을 양방향으로 통과
TEST(RingBufferCorrectness, WrapAroundPreservesBytesBothDirections)
{
	RingBuffer rb(8);

	char six[6] = { 1, 2, 3, 4, 5, 6 };
	ASSERT_TRUE(rb.Enqueue(six, 6));        // writePos 0->6
	char sink[6] = { 0 };
	ASSERT_TRUE(rb.Dequeue(sink, 6));       // readPos 0->6, 버퍼 비움
	EXPECT_EQ(rb.GetUsedSize(), 0);

	char four[4] = { 10, 20, 30, 40 };
	ASSERT_TRUE(rb.Enqueue(four, 4));       // writePos 6->2 : [6,7]+[0,1] wrap 쓰기
	char out[4] = { 0 };
	ASSERT_TRUE(rb.Dequeue(out, 4));        // readPos 6->2 : [6,7]+[0,1] wrap 읽기
	EXPECT_EQ(memcmp(out, four, 4), 0);     // 되감김을 넘나들며 바이트 보존
}

// full 경계: 꽉 차면 Enqueue false, Free==0, DirectEnqueueSize==0
TEST(RingBufferCorrectness, FullBoundaryRejectsAndReportsZeroFree)
{
	RingBuffer rb(4);
	char payload[4] = { 1, 2, 3, 4 };
	ASSERT_TRUE(rb.Enqueue(payload, 4));    // 꽉 채움
	EXPECT_EQ(rb.GetUsedSize(), 4);
	EXPECT_EQ(rb.GetFreeSize(), 0);
	EXPECT_EQ(rb.GetDirectEnqueueSize(), 0);

	char one = 9;
	EXPECT_FALSE(rb.Enqueue(&one, 1));      // 공간 없음
}

// empty 경계: 비면 Dequeue/Peek false, DirectDequeueSize==0
TEST(RingBufferCorrectness, EmptyBoundaryRejectsReadAndPeek)
{
	RingBuffer rb(4);
	char sink[2] = { 0 };
	EXPECT_FALSE(rb.Dequeue(sink, 1));
	EXPECT_FALSE(rb.Peek(sink, 1));
	EXPECT_EQ(rb.GetUsedSize(), 0);
	EXPECT_EQ(rb.GetDirectDequeueSize(), 0);
}

// Peek는 소비하지 않음 - 이어진 Dequeue가 같은 바이트를 냄
TEST(RingBufferCorrectness, PeekDoesNotAdvanceRead)
{
	RingBuffer rb(8);
	char payload[3] = { 7, 8, 9 };
	ASSERT_TRUE(rb.Enqueue(payload, 3));

	char peeked[3] = { 0 };
	ASSERT_TRUE(rb.Peek(peeked, 3));
	EXPECT_EQ(memcmp(peeked, payload, 3), 0);
	EXPECT_EQ(rb.GetUsedSize(), 3);         // Peek는 읽기 위치 안 움직임

	char dequeued[3] = { 0 };
	ASSERT_TRUE(rb.Dequeue(dequeued, 3));
	EXPECT_EQ(memcmp(dequeued, payload, 3), 0);
	EXPECT_EQ(rb.GetUsedSize(), 0);
}

// zero-copy 직접접근 == 복사 API: GetWritePtr+memcpy+MoveWritePos, GetReadPtr+read+MoveReadPos
TEST(RingBufferCorrectness, DirectAccessEqualsCopyApi)
{
	RingBuffer rb(8);

	char payload[5] = { 2, 4, 6, 8, 10 };
	int room = rb.GetDirectEnqueueSize();
	ASSERT_GE(room, 5);
	memcpy(rb.GetWritePtr(), payload, 5);   // OS(WSARecv)가 하는 직접 쓰기 흉내
	ASSERT_TRUE(rb.MoveWritePos(5));
	EXPECT_EQ(rb.GetUsedSize(), 5);

	int avail = rb.GetDirectDequeueSize();
	ASSERT_GE(avail, 5);
	EXPECT_EQ(memcmp(rb.GetReadPtr(), payload, 5), 0);   // 직접 읽기
	ASSERT_TRUE(rb.MoveReadPos(5));
	EXPECT_EQ(rb.GetUsedSize(), 0);
}

// MoveWritePos/MoveReadPos는 가용량 초과 시 거부(false) + 불변 유지
TEST(RingBufferCorrectness, MovePastAvailableIsRejected)
{
	RingBuffer rb(4);
	EXPECT_FALSE(rb.MoveWritePos(5));        // free(4) 초과 -> 거부
	EXPECT_EQ(rb.GetUsedSize(), 0);

	char payload[2] = { 1, 2 };
	ASSERT_TRUE(rb.Enqueue(payload, 2));
	EXPECT_FALSE(rb.MoveReadPos(3));         // used(2) 초과 -> 거부
	EXPECT_EQ(rb.GetUsedSize(), 2);
}

// Clear는 내용을 비우고 위치 초기화, 불변식 Free==Capacity-Used 유지
TEST(RingBufferCorrectness, ClearResetsAndInvariantHolds)
{
	RingBuffer rb(8);
	char payload[5] = { 1, 2, 3, 4, 5 };
	ASSERT_TRUE(rb.Enqueue(payload, 5));
	EXPECT_EQ(rb.GetFreeSize(), rb.GetCapacity() - rb.GetUsedSize());

	rb.Clear();
	EXPECT_EQ(rb.GetUsedSize(), 0);
	EXPECT_EQ(rb.GetFreeSize(), rb.GetCapacity());
}

// ==========================================================================
// 스트레스: multi-cycle wrap soak (단일스레드) + copy-API multi-producer 원자성
// ==========================================================================

// 포지션이 버퍼를 수천 바퀴 돌아도 무결성 유지 (memmove 없이 wrap만으로)
TEST(RingBufferStress, MultiCycleWrapSoakKeepsIntegrity)
{
	const int cap = 16;
	RingBuffer rb(cap);

	unsigned char counter = 0;              // 다음에 넣을 바이트 (0..255 순환)
	unsigned char expected = 0;             // 다음에 나와야 할 바이트
	const int laps = 100000;                // 버퍼를 수천 바퀴 돌림

	for (int i = 0; i < laps; ++i)
	{
		int chunk = (i % 7) + 1;            // 1..7 가변 (seam 위치를 계속 바꿈)
		char chunkBytes[8];
		for (int k = 0; k < chunk; ++k)
		{
			chunkBytes[k] = static_cast<char>(counter++);
		}

		if (rb.Enqueue(chunkBytes, chunk))
		{
			char out[8] = { 0 };
			ASSERT_TRUE(rb.Dequeue(out, chunk));
			for (int k = 0; k < chunk; ++k)
			{
				ASSERT_EQ(static_cast<unsigned char>(out[k]), expected++)
					<< "lap " << i << " byte " << k << " 손상";
			}
		}
		else
		{
			counter = static_cast<unsigned char>(counter - chunk);  // 안 넣었으면 되돌림
		}
		EXPECT_EQ(rb.GetFreeSize(), rb.GetCapacity() - rb.GetUsedSize());
	}
}

namespace
{
	struct RecordArgs
	{
		RingBuffer* rb;
		volatile LONG* startFlag;
		char tag;             // 이 producer의 레코드 바이트값
		int records;          // 넣을 레코드 수
	};
	// 각 producer는 4바이트 전부 자기 tag인 레코드를 반복 Enqueue (락이 레코드 원자성 보장)
	unsigned __stdcall ProducerProc(void* param)
	{
		RecordArgs* a = reinterpret_cast<RecordArgs*>(param);
		while (*a->startFlag == 0) ::YieldProcessor();
		char record[4] = { a->tag, a->tag, a->tag, a->tag };
		for (int i = 0; i < a->records; ++i)
		{
			while (!a->rb->Enqueue(record, 4)) ::YieldProcessor();   // 가득이면 소비 대기
		}
		return 0;
	}
}

// 잠금된 복사 API는 다중 생산에서도 4바이트 레코드가 섞이지 않음 (레코드 단위 원자성)
TEST(RingBufferStress, LockedCopyApiKeepsRecordsIntactMultiProducer)
{
	RingBuffer rb(4096);
	volatile LONG startFlag = 0;
	const int producerCount = 4;
	const int recordsEach = 20000;

	HANDLE handles[producerCount];
	RecordArgs args[producerCount];
	for (int p = 0; p < producerCount; ++p)
	{
		args[p].rb = &rb;
		args[p].startFlag = &startFlag;
		args[p].tag = static_cast<char>(0x11 * (p + 1));   // 서로 다른 값
		args[p].records = recordsEach;
		handles[p] = reinterpret_cast<HANDLE>(
			::_beginthreadex(nullptr, 0, ProducerProc, &args[p], 0, nullptr));
		ASSERT_NE(handles[p], nullptr);
	}
	::InterlockedExchange(&startFlag, 1);

	// 단일 소비자(메인): 4바이트씩 꺼내 레코드가 온전한지(네 바이트 동일) 확인
	const int totalRecords = producerCount * recordsEach;
	int consumed = 0;
	int torn = 0;
	const ULONGLONG deadline = ::GetTickCount64() + 30000;   // 유실 회귀 시 무한 대기 방지
	while (consumed < totalRecords)
	{
		char out[4] = { 0 };
		if (rb.Dequeue(out, 4))
		{
			if (!(out[0] == out[1] && out[1] == out[2] && out[2] == out[3]))
			{
				++torn;             // 레코드 바이트가 섞임 = 원자성 깨짐
			}
			++consumed;
		}
		else
		{
			if (::GetTickCount64() > deadline)
			{
				// 총량이 안 차면 producer 도 가득 찬 버퍼에서 영원히 스핀한다
				// (지역 args 참조) -> 반환하면 UB, 즉시 실패 종료.
				KYS::TESTS::FailFastTestHarness("RingBuffer multi-producer: consume deadline", 0);
			}
			::YieldProcessor();     // 아직 안 들어옴
		}
	}

	DWORD wait = ::WaitForMultipleObjects(producerCount, handles, TRUE, 30000);
	if (wait != WAIT_OBJECT_0)
	{
		KYS::TESTS::FailFastTestHarness("RingBuffer multi-producer: producers hung", 0);
	}
	for (int p = 0; p < producerCount; ++p) ::CloseHandle(handles[p]);

	EXPECT_EQ(torn, 0) << "잠금된 Enqueue인데 레코드가 섞임";
	EXPECT_EQ(consumed, totalRecords);
}

// ==========================================================================
// 엣지: read==write 모호성 / 직접접근 Single-Writer 경계
// ==========================================================================

// read==write가 Empty와 Full 양쪽에서 발생 - usedSize가 둘을 구분
TEST(RingBufferEdge, ReadEqualsWriteDisambiguatedByUsedSize)
{
	RingBuffer rb(4);

	// Empty: read==write, used==0
	EXPECT_EQ(rb.GetUsedSize(), 0);
	EXPECT_EQ(rb.GetDirectDequeueSize(), 0);   // 읽을 것 없음

	char payload[4] = { 1, 2, 3, 4 };
	ASSERT_TRUE(rb.Enqueue(payload, 4));        // Full: writePos 되감겨 readPos와 같아짐
	EXPECT_EQ(rb.GetUsedSize(), 4);
	EXPECT_EQ(rb.GetFreeSize(), 0);
	EXPECT_EQ(rb.GetDirectEnqueueSize(), 0);    // 쓸 곳 없음
	// 같은 read==write 상태지만 usedSize로 Empty/Full이 정확히 갈린다.
}

// 음수 size는 테스트하지 않는다 (의도된 경계):
//   Enqueue/Dequeue/Peek/MoveWritePos/MoveReadPos는 전 파라미터 non-negative가 선결조건이라
//   음수 가드가 없다. 예: Enqueue(data, -5)는 (-5 > GetFreeSize())가 false라 통과한 뒤
//   memcpy(..., firstSize)에서 음수가 size_t로 확장돼 거대 복사 -> 크래시(UB).
//   따라서 음수 케이스는 "잡아야 할 버그"가 아니라 "호출자 계약"이며 테스트하지 않는다.

// 직접접근 6함수는 Single-Writer 계약 -> 단일스레드에서만 검증한다.
// cross-thread로 GetWritePtr/MoveWritePos 등을 동시 호출하는 것은 data race이며
// 방어 대상이 아니므로 그런 테스트를 작성하지 않는다 (복사 API만 다중 진입 방어).
TEST(RingBufferEdge, DirectAccessIsSingleWriterOnly)
{
	RingBuffer rb(16);

	// 단일스레드 직접접근 라운드트립만 (락 없음이 정당한 유일 문맥)
	char payload[6] = { 5, 6, 7, 8, 9, 10 };
	ASSERT_GE(rb.GetDirectEnqueueSize(), 6);
	memcpy(rb.GetWritePtr(), payload, 6);
	ASSERT_TRUE(rb.MoveWritePos(6));

	char out[6] = { 0 };
	ASSERT_GE(rb.GetDirectDequeueSize(), 6);
	memcpy(out, rb.GetReadPtr(), 6);
	ASSERT_TRUE(rb.MoveReadPos(6));
	EXPECT_EQ(memcmp(out, payload, 6), 0);
	EXPECT_EQ(rb.GetUsedSize(), 0);
}
