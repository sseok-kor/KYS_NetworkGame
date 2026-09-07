#include "pch_gameserver.h"
#include "Core/Lifecycle/GracefulShutdown.h"

namespace KYS
{
	namespace GAMESERVER
	{
		namespace
		{
			// OS 강제 종료 경로(콘솔 창 X / 로그오프 / 시스템 종료)의 정리 예산.
			//   콘솔 앱의 OS 데드라인은 기본 5000ms(HandlerRoutine 규약) - 그 안에 핸들러가 TRUE 를 반환해야 깨끗이 끝난다.
			//   안전 마진 500ms 를 빼 4500ms 를 캡으로 둔다. 초과분은 프로세스가 강제 종료되며 미저장분은 마지막 autosave 로 폴백
			//   (DB 저장은 트랜잭션 원자적이라 중단돼도 반쪽 손상 없음). 'x' 키/관리자 정지는 OS 데드라인이 없어 이 캡과 무관하게 완주(유실 0).
			const DWORD OS_SHUTDOWN_BUDGET_MS = 4500;

			HANDLE g_shutdownEvent = nullptr;      // 종료 요청 (manual-reset) - 'x' 키 또는 콘솔 핸들러가 세움. main 루프가 폴링.
			HANDLE g_teardownDoneEvent = nullptr;  // 정리 완료 (manual-reset) - main 이 정리 완주 후 세움. 콘솔 핸들러가 대기.

			// 콘솔 제어 이벤트 핸들러 - OS 가 임시 스레드를 주입해 여기서 실행한다.
			//   창 X / 로그오프 / 시스템 종료: main 을 깨워 정리를 돌리게 하고, 정리 완료(또는 예산 소진)까지 블록한 뒤 TRUE 반환.
			//     TRUE = "처리했다" -> 반환 즉시 OS 가 프로세스를 종료(정리가 끝났으면 깨끗이, 예산 초과면 중단 - 원자적 저장이라 안전).
			//   Ctrl+C / Ctrl+Break: FALSE -> OS 기본 처리(즉시 종료).
			BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
			{
				switch (ctrlType)
				{
				case CTRL_CLOSE_EVENT:
				case CTRL_LOGOFF_EVENT:
				case CTRL_SHUTDOWN_EVENT:
					::wprintf(L"\n[shutdown] 콘솔 종료 감지 - 저장 후 종료 진행(최대 %lums)\n", OS_SHUTDOWN_BUDGET_MS);
					if (g_shutdownEvent != nullptr) { ::SetEvent(g_shutdownEvent); }               // main 깨우기 -> 정리 시작
					if (g_teardownDoneEvent != nullptr) { ::WaitForSingleObject(g_teardownDoneEvent, OS_SHUTDOWN_BUDGET_MS); }   // 완료/예산까지 대기
					return TRUE;
				default:
					return FALSE;   // CTRL_C_EVENT / CTRL_BREAK_EVENT - OS 기본(즉시 종료)
				}
			}
		}

		bool GracefulShutdown::Install()
		{
			g_shutdownEvent = ::CreateEventW(NULL, TRUE, FALSE, NULL);       // manual-reset, 초기 비신호
			g_teardownDoneEvent = ::CreateEventW(NULL, TRUE, FALSE, NULL);   // manual-reset, 초기 비신호
			if (g_shutdownEvent == nullptr || g_teardownDoneEvent == nullptr)
			{
				// 한쪽만 성공했으면 성공한 핸들을 닫아 leak 방지 (둘 중 하나라도 null 이면 설치 실패).
				if (g_shutdownEvent != nullptr) { ::CloseHandle(g_shutdownEvent); g_shutdownEvent = nullptr; }
				if (g_teardownDoneEvent != nullptr) { ::CloseHandle(g_teardownDoneEvent); g_teardownDoneEvent = nullptr; }
				return false;   // 이벤트 생성 실패(극단 OOM). main 은 'x' 를 직접 break 로도 탈출하므로 정상 종료는 유지.
			}
			return ::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE) != FALSE;
		}

		bool GracefulShutdown::WaitShutdownRequested(DWORD timeoutMs)
		{
			if (g_shutdownEvent == nullptr)
			{
				return false;
			}
			return ::WaitForSingleObject(g_shutdownEvent, timeoutMs) == WAIT_OBJECT_0;
		}

		void GracefulShutdown::RequestShutdown()
		{
			if (g_shutdownEvent != nullptr) { ::SetEvent(g_shutdownEvent); }
		}

		void GracefulShutdown::SignalTeardownDone()
		{
			if (g_teardownDoneEvent != nullptr) { ::SetEvent(g_teardownDoneEvent); }
		}
	}
}
