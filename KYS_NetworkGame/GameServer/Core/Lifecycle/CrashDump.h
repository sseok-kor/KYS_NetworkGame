#pragma once

namespace KYS
{
	namespace GAMESERVER
	{
		// 미처리 예외(크래시) 발생 시 미니덤프(.dmp) + 요약(.txt)을 Dumps 폴더에 남기고
		// 프로세스를 종료(fail-fast)하는 프로세스 전역 크래시 핸들러.
		//   - wmain 시작부에서 Install을 1회 부르면 이후 만든 모든 스레드가 자동으로 커버된다
		//     (SetUnhandledExceptionFilter는 프로세스 전역이라 스레드마다 재등록 불필요).
		//   - 덤프는 미리 띄워 둔 전용 스레드가 깨끗한 스택에서 뜬다(스택 고갈 크래시에도 안전).
		//   - SEH 필터를 우회하는 CRT 함정 3종(순가상 호출/잘못된 인자/abort)도 함께 잡는다.
		class CrashDump
		{
		public:
			// 크래시 핸들러 등록. appName은 덤프 파일 이름 접두사(예: L"ServerApp"). wmain 시작부에서 1회만 호출.
			static void Install(const wchar_t* appName);

			// 덤프(.dmp/.txt) 작성이 끝난 뒤 전용 덤프 스레드에서 한 번 불러 줄 콜백 등록(예: 로그 큐 잔량 flush).
			//   - 덤프 "이후"에 SEH 가드 안에서 호출되므로 콜백이 2차 폴트를 내도 덤프는 이미 확보돼 있다.
			//   - 덤프 스레드 준비 실패 폴백(크래시 스레드가 직접 덤프)에서는 호출하지 않는다(크래시 스택에서는 얇은 경로만).
			static void SetPostDumpCallback(void (*callback)());
		};
	}
}
