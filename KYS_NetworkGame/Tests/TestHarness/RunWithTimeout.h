#pragma once
#include "pch_tests.h"
#include "TestHarness/ConcurrentHammer.h"   // FailFastTestHarness

namespace KYS
{
	namespace TESTS
	{
		// 단일 작업을 워커 스레드에서 돌리고 timeout으로 감시.
		// 정상 완료 true, 워커 스레드 생성 실패 false.
		// 시간 초과는 값을 반환하지 않고 프로세스를 즉시 실패 종료한다 (무한 스핀/hang 방어).
		struct SoloArg
		{
			void (*fn)(void*);
			void* arg;
		};

		inline unsigned __stdcall SoloThreadProc(void* raw)
		{
			SoloArg* a = static_cast<SoloArg*>(raw);
			a->fn(a->arg);
			return 0;
		}

		inline bool RunWithTimeout(void (*fn)(void*), void* arg, DWORD timeoutMs)
		{
			SoloArg s{ fn, arg };
			HANDLE h = reinterpret_cast<HANDLE>(
				::_beginthreadex(nullptr, 0, SoloThreadProc, &s, 0, nullptr));
			if (h == nullptr) return false;
			DWORD wait = ::WaitForSingleObject(h, timeoutMs);
			if (wait != WAIT_OBJECT_0)
			{
				// 작업 스레드가 지역 인자(호출자 스택)를 계속 참조하므로
				// 그대로 반환하면 UB로 이후 테스트를 오염시킨다 -> 즉시 실패 종료.
				FailFastTestHarness("RunWithTimeout: worker hung", 0);
			}
			::CloseHandle(h);
			return true;
		}
	}
}
