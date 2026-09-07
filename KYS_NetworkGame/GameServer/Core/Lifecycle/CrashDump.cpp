#include "pch_gameserver.h"
#include "Core/Lifecycle/CrashDump.h"
#include <dbghelp.h>   // MINIDUMP_TYPE / MINIDUMP_EXCEPTION_INFORMATION (dbghelp.lib 는 링크하지 않음 - 런타임 동적 로드)
#include <signal.h>    // signal / SIGABRT
#include <stdlib.h>    // _set_purecall_handler / _set_invalid_parameter_handler
#include <string.h>    // wcsncpy_s

namespace KYS
{
	namespace GAMESERVER
	{
		namespace
		{
			// MiniDumpWriteDump 함수 포인터 타입 - Install 에서 dbghelp.dll 을 미리 로드해 GetProcAddress 로 얻어 캐시한다.
			//   정적 링크(dbghelp.lib) 대신 동적 로드하는 이유: 대상 머신의 시스템 dbghelp.dll 을 그대로 써서 버전 호환 문제를 피한다(Valve/CGSF 관행).
			//   크래시 시점이 아니라 Install(로더 락이 없는 시작 시점)에서 로드하는 이유: 크래시가 로더 락을 쥔 채 났을 때
			//   덤프 스레드가 LoadLibrary 에서 그 락을 기다리다 데드락하는 것을 피한다.
			typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(
				HANDLE process, DWORD processId, HANDLE file,
				MINIDUMP_TYPE dumpType,
				PMINIDUMP_EXCEPTION_INFORMATION exceptionParam,
				PMINIDUMP_USER_STREAM_INFORMATION userStream,
				PMINIDUMP_CALLBACK_INFORMATION callback);

			// 덤프 내용(균형형): 콜스택 + 모듈 + 스레드 상태 + 언로드 모듈 + 스택이 직접 가리키는 힙(지역변수 참조 객체).
			//   전역 데이터섹션 / PrivateReadWrite 힙 전체(평문 비번/토큰/PBKDF2/DB 접속정보 상주)는 제외 -> 민감정보 노출 최소화.
			const MINIDUMP_TYPE DUMP_CONTENT = static_cast<MINIDUMP_TYPE>(
				MiniDumpNormal
				| MiniDumpWithIndirectlyReferencedMemory
				| MiniDumpWithThreadInfo
				| MiniDumpWithUnloadedModules);

			const wchar_t DUMP_FOLDER[] = L"Dumps";
			const DWORD   DUMP_WAIT_LIMIT_MS = 30000;   // 크래시 스레드가 덤프 완료를 기다리는 안전 상한

			wchar_t             g_appName[64] = L"App";   // 덤프 파일 접두사 (Install 에서 복사)
			HANDLE              g_dumpThread = nullptr;    // 전용 덤프 스레드 (미리 띄워 두고 이벤트 대기)
			HANDLE              g_dumpRequested = nullptr; // 크래시 -> 덤프 스레드 깨우기 (auto-reset)
			HANDLE              g_dumpFinished = nullptr;  // 덤프 완료 -> 크래시 스레드 깨우기 (auto-reset)
			EXCEPTION_POINTERS* g_exceptionInfo = nullptr; // 크래시 스레드가 실어 넘기는 예외 정보 (없으면 CRT 함정 경로)
			DWORD               g_crashedThreadId = 0;     // 크래시 난 스레드 id (덤프가 이 스레드를 폴트 스레드로 표시)
			volatile LONG       g_dumping = 0;             // 단일 진입 게이트 (여러 스레드 동시 크래시 시 최초 1개만 덤프)
			HMODULE             g_dbghelp = nullptr;       // dbghelp.dll (Install 에서 미리 로드 - 크래시 시점 LoadLibrary 의 로더 락 데드락 회피)
			MiniDumpWriteDumpFn g_writeMiniDump = nullptr; // MiniDumpWriteDump 함수 포인터 (Install 에서 캐시)
			void (*g_postDumpCallback)() = nullptr;        // 덤프 작성 후 덤프 스레드에서 부르는 앱 콜백 (예: 로그 flush)

			// 흔한 예외 코드를 사람이 읽을 이름으로. (전체 심볼 스택은 .dmp 를 cdb 로 열어 확인)
			const wchar_t* ExceptionCodeName(DWORD code)
			{
				switch (code)
				{
				case EXCEPTION_ACCESS_VIOLATION:      return L"ACCESS_VIOLATION";
				case EXCEPTION_STACK_OVERFLOW:        return L"STACK_OVERFLOW";
				case EXCEPTION_ILLEGAL_INSTRUCTION:   return L"ILLEGAL_INSTRUCTION";
				case EXCEPTION_INT_DIVIDE_BY_ZERO:    return L"INT_DIVIDE_BY_ZERO";
				case EXCEPTION_PRIV_INSTRUCTION:      return L"PRIV_INSTRUCTION";
				case EXCEPTION_IN_PAGE_ERROR:         return L"IN_PAGE_ERROR";
				case EXCEPTION_DATATYPE_MISALIGNMENT: return L"DATATYPE_MISALIGNMENT";
				case EXCEPTION_NONCONTINUABLE_EXCEPTION: return L"NONCONTINUABLE_EXCEPTION";
				default:                              return L"UNKNOWN";
				}
			}

			// 미니덤프(.dmp) 작성 - 덤프 스레드 컨텍스트에서만 호출. dbghelp 는 Install 에서 미리 로드해 캐시(g_writeMiniDump)한 것을 쓴다.
			void WriteMiniDump(const wchar_t* stem, EXCEPTION_POINTERS* exceptionInfo)
			{
				if (g_writeMiniDump == nullptr) { return; }   // dbghelp 프리로드 실패 시 .dmp 는 건너뛴다(.txt 는 그대로 작성)

				wchar_t path[MAX_PATH];
				::swprintf_s(path, L"%s.dmp", stem);

				HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file != INVALID_HANDLE_VALUE)
				{
					MINIDUMP_EXCEPTION_INFORMATION mei;
					mei.ThreadId = g_crashedThreadId;
					mei.ExceptionPointers = exceptionInfo;
					mei.ClientPointers = FALSE;   // 예외 포인터는 같은 프로세스 주소공간(덤프 스레드와 크래시 스레드가 공유)

					// exceptionInfo 가 없으면(CRT 함정 경로) 예외 레코드 없이 전 스레드 상태만 덤프.
					PMINIDUMP_EXCEPTION_INFORMATION meiPtr = (exceptionInfo != nullptr) ? &mei : nullptr;

					g_writeMiniDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, DUMP_CONTENT, meiPtr, nullptr, nullptr);
					::CloseHandle(file);
				}
			}

			// 요약(.txt) 작성 - 심볼 없이 얻는 triage 정보(예외 코드/주소/폴트 모듈+오프셋/스레드). UTF-16LE + BOM.
			void WriteTextSummary(const wchar_t* stem, EXCEPTION_POINTERS* exceptionInfo, const SYSTEMTIME& now, DWORD pid)
			{
				wchar_t path[MAX_PATH];
				::swprintf_s(path, L"%s.txt", stem);

				HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file == INVALID_HANDLE_VALUE) { return; }

				wchar_t text[2048];
				int n = 0;
				n += ::swprintf_s(text + n, _countof(text) - n,
					L"process : %s (pid %u)\r\n"
					L"time    : %04u-%02u-%02u %02u:%02u:%02u\r\n"
					L"thread  : %u (crashed)\r\n",
					g_appName, pid,
					now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
					g_crashedThreadId);

				if (exceptionInfo != nullptr && exceptionInfo->ExceptionRecord != nullptr)
				{
					const EXCEPTION_RECORD* record = exceptionInfo->ExceptionRecord;
					const void* faultAddr = record->ExceptionAddress;

					n += ::swprintf_s(text + n, _countof(text) - n,
						L"code    : 0x%08X (%s)\r\n"
						L"address : 0x%p\r\n",
						record->ExceptionCode, ExceptionCodeName(record->ExceptionCode), faultAddr);

					// 접근 위반이면 읽기/쓰기 + 대상 주소를 덧붙인다(ExceptionInformation[0]=0 읽기/1 쓰기, [1]=대상 주소).
					if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
					{
						const wchar_t* op = (record->ExceptionInformation[0] == 1) ? L"write" : L"read";
						n += ::swprintf_s(text + n, _countof(text) - n,
							L"access  : %s at 0x%p\r\n",
							op, reinterpret_cast<const void*>(record->ExceptionInformation[1]));
					}

					// 폴트 주소가 속한 모듈 + 모듈 시작으로부터의 오프셋 (심볼 없이 얻는다).
					HMODULE mod = nullptr;
					if (::GetModuleHandleExW(
						GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCWSTR>(faultAddr), &mod) && mod != nullptr)
					{
						wchar_t modPath[MAX_PATH];
						if (::GetModuleFileNameW(mod, modPath, MAX_PATH) > 0)
						{
							const uintptr_t offset =
								reinterpret_cast<uintptr_t>(faultAddr) - reinterpret_cast<uintptr_t>(mod);
							n += ::swprintf_s(text + n, _countof(text) - n,
								L"module  : %s +0x%zX\r\n", modPath, offset);
						}
					}
				}
				else
				{
					n += ::swprintf_s(text + n, _countof(text) - n,
						L"code    : (CRT trap - no exception record)\r\n");
				}

				n += ::swprintf_s(text + n, _countof(text) - n,
					L"\r\nfull symbolic call stack: open the .dmp in WinDbg/cdb (Tools\\analyze_dump.cmd)\r\n");

				if (n > 0)
				{
					const unsigned char bom[2] = { 0xFF, 0xFE };   // UTF-16LE BOM
					DWORD written = 0;
					::WriteFile(file, bom, 2, &written, nullptr);
					::WriteFile(file, text, static_cast<DWORD>(n) * sizeof(wchar_t), &written, nullptr);
				}

				::CloseHandle(file);
			}

			// .dmp + .txt 를 Dumps 폴더에 한 쌍으로 작성.
			void WriteDumpFiles(EXCEPTION_POINTERS* exceptionInfo)
			{
				::CreateDirectoryW(DUMP_FOLDER, nullptr);   // 이미 있으면 무해(ERROR_ALREADY_EXISTS)

				SYSTEMTIME now;
				::GetLocalTime(&now);
				const DWORD pid = ::GetCurrentProcessId();

				wchar_t stem[MAX_PATH];
				::swprintf_s(stem, L"%s\\%s_%04u%02u%02u_%02u%02u%02u_pid%u",
					DUMP_FOLDER, g_appName,
					now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, pid);

				WriteMiniDump(stem, exceptionInfo);
				WriteTextSummary(stem, exceptionInfo, now, pid);
			}

			// 크래시 정보를 전역에 싣고 전용 덤프 스레드를 깨워 덤프 완료를 기다린다.
			//   단일 진입 게이트로 최초 1개 스레드만 덤프한다(DbgHelp 비스레드세이프 + 여러 스레드 동시 크래시 대비).
			void RequestDumpAndWait(EXCEPTION_POINTERS* exceptionInfo)
			{
				if (::InterlockedCompareExchange(&g_dumping, 1, 0) != 0)
				{
					// 이미 덤프 중(다른 스레드가 게이트 선점). 크래시 난 원본 스레드(g_crashedThreadId)가 덤프 완료 대기/폴백 중 또 폴트해 재진입한 경우만 -
					// 자기 종료를 못 하므로 즉시 종료한다(무한 hang 방지). 덤프는 전용 덤프 스레드가 뜨고, 그 외 스레드는 아래 Sleep 으로 멈춰 원본 스레드의 30s 백스톱(DUMP_WAIT_LIMIT_MS)에 종료된다.
					if (g_crashedThreadId == ::GetCurrentThreadId())
					{
						::TerminateProcess(::GetCurrentProcess(), 1);
					}
					// 다른 스레드가 레이스에서 짐 -> 최초 스레드가 곧 프로세스를 종료시킨다. 그때까지 대기.
					::Sleep(INFINITE);
					return;
				}

				g_exceptionInfo = exceptionInfo;
				g_crashedThreadId = ::GetCurrentThreadId();

				if (g_dumpThread != nullptr && g_dumpRequested != nullptr && g_dumpFinished != nullptr)
				{
					::SetEvent(g_dumpRequested);                          // 전용(깨끗한 스택) 덤프 스레드 깨우기
					::WaitForSingleObject(g_dumpFinished, DUMP_WAIT_LIMIT_MS);
				}
				else
				{
					// 전용 스레드 준비 실패 폴백 - 현재(크래시) 스레드에서 직접 덤프.
					//   post-dump 콜백은 부르지 않는다: 크래시 스택에서는 얇은 경로만 쓴다는 불변식 유지.
					WriteDumpFiles(exceptionInfo);
				}
			}

			// 덤프 확보 "후" 앱 콜백(로그 flush 등)을 SEH 가드 안에서 부른다 - 콜백이 2차 폴트를 내도
			//   여기서 삼키고 계속 간다(덤프는 이미 디스크에 있고, g_dumpFinished 신호는 반드시 도달해야 한다).
			//   C++ unwinding 객체가 없는 함수에만 __try 를 둘 수 있어(C2712) 별도 함수로 분리.
			void CallPostDumpCallbackGuarded()
			{
				if (g_postDumpCallback == nullptr) { return; }
				__try
				{
					g_postDumpCallback();
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					// 콜백 실패는 무시 - 덤프 소실로 승격시키지 않는다.
				}
			}

			// 전용 덤프 스레드 - 요청 이벤트를 기다렸다가 깨끗한 스택에서 덤프를 뜬다. 크래시는 프로세스당 1회(게이트).
			unsigned __stdcall DumpThreadMain(void*)
			{
				if (g_dumpRequested == nullptr) { return 0; }

				::WaitForSingleObject(g_dumpRequested, INFINITE);
				WriteDumpFiles(g_exceptionInfo);
				CallPostDumpCallbackGuarded();   // 덤프 확보가 먼저, 로그 flush 는 best-effort 나중
				if (g_dumpFinished != nullptr) { ::SetEvent(g_dumpFinished); }
				return 0;
			}

			// 최상위 SEH 필터 - 어떤 __except/catch 도 처리하지 못한 예외(= 크래시)에서만 호출된다.
			LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* exceptionInfo)
			{
				RequestDumpAndWait(exceptionInfo);
				return EXCEPTION_EXECUTE_HANDLER;   // 덤프 후 프로세스 종료(fail-fast) - 손상된 상태로 계속 실행하지 않는다.
			}

			// CRT 함정 3종 - SEH 필터를 우회하므로 별도로 잡아 덤프 후 종료한다. 예외 레코드가 없어 전 스레드 상태만 덤프.
			void __cdecl OnPureVirtualCall()
			{
				RequestDumpAndWait(nullptr);
				::TerminateProcess(::GetCurrentProcess(), 1);
			}

			void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
			{
				RequestDumpAndWait(nullptr);
				::TerminateProcess(::GetCurrentProcess(), 1);
			}

			void __cdecl OnAbortSignal(int)
			{
				RequestDumpAndWait(nullptr);
				::TerminateProcess(::GetCurrentProcess(), 1);
			}
		}

		void CrashDump::SetPostDumpCallback(void (*callback)())
		{
			g_postDumpCallback = callback;
		}

		void CrashDump::Install(const wchar_t* appName)
		{
			if (appName != nullptr)
			{
				::wcsncpy_s(g_appName, _countof(g_appName), appName, _TRUNCATE);
			}

			// dbghelp.dll 을 지금(로더 락이 없는 시작 시점) 미리 로드하고 MiniDumpWriteDump 를 캐시한다.
			//   크래시 시점에 LoadLibrary 하면 크래시가 로더 락을 쥔 경우 덤프 스레드가 데드락하므로 여기서 미리 잡아 둔다.
			g_dbghelp = ::LoadLibraryW(L"dbghelp.dll");
			if (g_dbghelp != nullptr)
			{
				g_writeMiniDump = reinterpret_cast<MiniDumpWriteDumpFn>(::GetProcAddress(g_dbghelp, "MiniDumpWriteDump"));
			}

			// 스택 고갈 크래시 때 SEH 필터가 신호(SetEvent/Wait)를 보낼 여유 스택을 확보(main 스레드).
			//   무거운 덤프 작성(포매팅/dbghelp)은 전용 스레드가 깨끗한 스택에서 맡으므로, 워커 스레드는 이 얇은 신호 경로만 크래시 스택에서 쓴다.
			ULONG stackReserve = 65536;
			::SetThreadStackGuarantee(&stackReserve);

			// 덤프 요청/완료 이벤트(auto-reset, 비신호 시작).
			g_dumpRequested = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			g_dumpFinished = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

			// 전용 덤프 스레드를 미리 띄운다 - 크래시 순간 깨끗한 스택에서 덤프하기 위해(스택 고갈 크래시 대비).
			unsigned threadId = 0;
			g_dumpThread = reinterpret_cast<HANDLE>(::_beginthreadex(nullptr, 0, &DumpThreadMain, nullptr, 0, &threadId));

			// 최상위 SEH 필터 등록 - 프로세스 전역이라 이후 만든 모든 스레드를 커버.
			::SetUnhandledExceptionFilter(&OnUnhandledException);

			// SEH 필터를 우회하는 CRT 함정 3종.
			::_set_purecall_handler(&OnPureVirtualCall);
			::_set_invalid_parameter_handler(&OnInvalidParameter);
			// Debug CRT 의 abort() 는 raise(SIGABRT) 전에 모달 창("abort() has been called")을 띄워
			//   무인 서버가 사람이 버튼을 누를 때까지 멈춘다. 그 리포트를 꺼서 abort() 가 곧바로 SIGABRT 핸들러로 가게 한다.
			//   이 mask 는 Release CRT 에선 무의미(no-op)라 Release 동작은 불변.
			::_set_abort_behavior(0, _WRITE_ABORT_MSG);
			::signal(SIGABRT, &OnAbortSignal);
		}
	}
}
