#pragma once
#include <Windows.h>

namespace KYS
{
	namespace GAMESERVER
	{
		// QueryPerformanceCounter로 시작 시점부터의 경과 시간을 재는 고해상도 타이머.
		class Timer
		{
		private:
			LARGE_INTEGER m_frequency;     // 1초당 QPC 틱 수 (틱을 시간으로 변환할 때 나누는 값)
			LARGE_INTEGER m_startTime;     // 측정 시작 시점의 틱 (이 시점부터 경과 시간 잼)
			bool m_initialized;            // Initialize 성공 여부 (false면 시간 조회가 0 반환)

		public:
			// 생성
			Timer()                        // 멤버만 0/false로 두고, 실제 준비는 Initialize에서
			{
				m_frequency.QuadPart = 0;
				m_startTime.QuadPart = 0;
				m_initialized = false;
			}

			// 측정 준비 / 기준 재설정
			bool Initialize();             // 측정 준비 (성공하면 true), 호출 후부터 경과 시간 측정 가능
			void Reset();                  // 기준 시각을 지금으로 다시 잡음 (경과 시간 0부터 다시)

			// 경과 시간 조회 (시작 이후)
			UINT64 GetCurrentTimeMS();     // 밀리초
			UINT64 GetCurrentTimeMicro();  // 마이크로초
			UINT64 GetCurrentTimeNano();   // 나노초

		};
	}
}
