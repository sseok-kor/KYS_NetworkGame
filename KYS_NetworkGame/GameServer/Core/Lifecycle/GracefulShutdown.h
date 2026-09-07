#pragma once
#include <Windows.h>

namespace KYS
{
	namespace GAMESERVER
	{
		// 콘솔 제어 이벤트(창 X / 로그오프 / 시스템 종료)를 붙잡아, OS 가 프로세스를 강제 종료하기 전에
		// main 이 정리 코드를 끝까지 돌리게 해 주는 프로세스 전역 graceful shutdown 게이트.
		//   - 두 exe(ServerApp/LoginServer) 공유 인프라. 메커니즘(이벤트 2개 + 콘솔 핸들러 + OS 예산 캡)만 제공하고
		//     실제 정리(teardown) 순서는 각 main 이 소유한다(ServerApp=접속 플레이어 저장, LoginServer=저장 없음).
		//   - 종료는 오직 main 스레드에서만 실행된다(콘솔 핸들러는 신호 + 완료 대기만) -> 이중 실행/경합 없음.
		//   - CTRL_C / CTRL_BREAK 는 붙잡지 않는다(OS 기본 = 즉시 종료 - 개발 중 Ctrl+C 로 빠르게 죽이는 편의 보존).
		//   - 데드라인은 콘솔 핸들러 한 곳에서만 건다(OS 강제 경로 캡). 'x' 키/관리자 정지는 데드라인 없이 완주(유실 0).
		class GracefulShutdown
		{
		public:
			// 이벤트 2개 생성 + SetConsoleCtrlHandler 등록. 부팅 완료 후(키 입력 대기 직전) 1회 호출.
			//   반환 false = 콘솔 핸들러 등록 실패(치명 아님 - 창 X graceful 만 없고 'x' 정상 종료는 유지).
			static bool Install();

			// main 의 입력 루프가 폴링한다. timeoutMs 동안 대기하다 종료가 요청됐으면 true(루프 탈출 -> 정리로).
			static bool WaitShutdownRequested(DWORD timeoutMs);

			// main 이 'x' 키에서 호출(콘솔 핸들러와 같은 종료 이벤트를 세운다).
			static void RequestShutdown();

			// main 이 정리(teardown)를 끝까지 마친 직후 호출. 콘솔 핸들러의 완료 대기를 풀어 OS 종료를 진행시킨다.
			static void SignalTeardownDone();
		};
	}
}
