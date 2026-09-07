#pragma once
#include "pch_tests.h"

namespace KYS
{
	namespace TESTS
	{
		// 테스트 하니스의 hang/오류는 복구 불가 상태다: 살아있는 스레드가 slots 원소(vector 가
		// 쥔 힙 버퍼 - Run 이 반환하는 순간 해제된다)와 Run 스택의 body 사본을 계속 참조하므로,
		// 그대로 반환하면 이후 테스트가 UB로 오염된다.
		// -> 재현 정보(seed)를 stderr에 남기고 즉시 프로세스를 실패 종료한다(fail-fast).
		inline void FailFastTestHarness(const char* reason, UINT64 seed)
		{
			fprintf(stderr, "\n[TEST-HARNESS FAIL-FAST] %s (seed=%llu)\n", reason, seed);
			::TerminateProcess(::GetCurrentProcess(), 3);
		}

		// 스트레스 실행 결과 (실패 재현 정보 포함)
		struct HammerResult
		{
			bool   timedOut;      // 항상 false - hang은 위 fail-fast로 종료된다 (호출부 방어 assert용으로 유지)
			UINT64 seed;          // 이번 실행 시드 (실패 시 이 값으로 재실행)
			DWORD  threadCount;   // 실제로 띄운 스레드 수
		};

		// 각 스레드가 실행할 본체에 넘겨줄 컨텍스트.
		template<typename Body>
		struct HammerSlot
		{
			Body*          body;         // 스레드가 공유하는 테스트 대상 연산
			DWORD          threadIndex;  // 이 스레드의 번호 (0..threadCount-1)
			UINT32         iterations;   // 이 스레드가 body를 부를 횟수
			UINT64         seed;         // 재현용 시드 (body가 난수에 쓰라고 전달)
			volatile LONG* readyCount;   // 준비 완료한 스레드 수 (시작 배리어)
			volatile LONG* startGate;    // 0=대기, 1=출발 (시작 배리어)
		};

		// K 스레드를 동시에 출발시켜 고경합으로 두들기는 스트레스 하니스.
		class ConcurrentHammer
		{
		public:
			// threadCount 스레드가 각각 iterationsPerThread번 body를 호출한다.
			// 모든 스레드가 준비되면 게이트를 열어 동시에 출발(경합 극대화).
			// timeoutMs 안에 다 못 끝나면 fail-fast 로 프로세스를 종료한다(데드락 = hang 대신 즉시 실패).
			template<typename Body>
			static HammerResult Run(DWORD threadCount,
			                        UINT32 iterationsPerThread,
			                        Body body,
			                        DWORD timeoutMs = 30000,
			                        UINT64 seed = 0)
			{
				// WaitForMultipleObjects 한계(64) 밖은 지원하지 않는다 - 시작 전에 실패 종료.
				// (배치 분할 대기는 미구현 - 필요해지면 그때 구현한다.)
				if (threadCount == 0 || threadCount > MAXIMUM_WAIT_OBJECTS)
				{
					FailFastTestHarness("ConcurrentHammer: threadCount out of range (1..64)", 0);
				}
				if (seed == 0)
				{
					LARGE_INTEGER now;
					::QueryPerformanceCounter(&now);   // 시드 미지정이면 시계로 생성
					seed = static_cast<UINT64>(now.QuadPart);
				}

				volatile LONG readyCount = 0;
				volatile LONG startGate  = 0;

				std::vector<HANDLE>           threads(threadCount, nullptr);
				std::vector<HammerSlot<Body>> slots(threadCount);

				for (DWORD t = 0; t < threadCount; ++t)
				{
					slots[t].body        = &body;
					slots[t].threadIndex = t;
					slots[t].iterations  = iterationsPerThread;
					slots[t].seed        = seed;
					slots[t].readyCount  = &readyCount;
					slots[t].startGate   = &startGate;

					threads[t] = reinterpret_cast<HANDLE>(::_beginthreadex(
						nullptr, 0, &ThreadProc<Body>, &slots[t], 0, nullptr));
					if (threads[t] == nullptr)
					{
						// 스폰 실패: 이미 뜬 스레드만 게이트를 열어 완주시킨 뒤 실패 종료.
						// (열지 않으면 그 스레드들이 게이트에서 영원히 스핀한다.)
						::InterlockedExchange(&startGate, 1);
						if (t > 0)
						{
							::WaitForMultipleObjects(t, threads.data(), TRUE, timeoutMs);
						}
						FailFastTestHarness("ConcurrentHammer: _beginthreadex failed", seed);
					}
				}

				// 전원 준비될 때까지 기다렸다 게이트를 연다 (동시 출발 = 경합 극대화)
				while (::InterlockedCompareExchange(&readyCount, 0, 0)
				       < static_cast<LONG>(threadCount))
				{
					::YieldProcessor();
				}
				::InterlockedExchange(&startGate, 1);

				// 전부 끝나기를 기다림 (한계 64는 진입 가드에서 이미 차단됨).
				DWORD wait = ::WaitForMultipleObjects(
					threadCount, threads.data(), TRUE, timeoutMs);

				// 완주 판정은 WAIT_OBJECT_0 하나만 성공으로 본다(fail-safe).
				// WAIT_FAILED 같은 예외 반환을 성공으로 오인하면 살아있는 스레드의
				// 핸들을 닫고 죽은 스택을 참조하게 된다.
				if (wait != WAIT_OBJECT_0)
				{
					FailFastTestHarness("ConcurrentHammer: threads hung or wait failed", seed);
				}

				HammerResult result;
				result.timedOut    = false;   // 여기 도달 = 전원 완주 (실패는 위에서 종료됨)
				result.seed        = seed;
				result.threadCount = threadCount;

				for (DWORD t = 0; t < threadCount; ++t)
				{
					::CloseHandle(threads[t]);
				}
				return result;
			}

		private:
			// 각 스레드의 진입점: 준비 신호 -> 게이트 대기 -> 본체 반복.
			template<typename Body>
			static unsigned __stdcall ThreadProc(void* arg)
			{
				HammerSlot<Body>* slot = static_cast<HammerSlot<Body>*>(arg);

				// (1) 준비 완료를 알리고 출발 게이트가 열릴 때까지 스핀 (동시 출발)
				::InterlockedIncrement(slot->readyCount);
				while (::InterlockedCompareExchange(slot->startGate, 0, 0) == 0)
				{
					::YieldProcessor();
				}

				// (2) 본체를 iterations번 두들긴다
				for (UINT32 i = 0; i < slot->iterations; ++i)
				{
					(*slot->body)(slot->threadIndex, i, slot->seed);
				}
				return 0;
			}
		};
	}
}
