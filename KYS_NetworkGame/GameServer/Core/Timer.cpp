#include "pch_gameserver.h"
#include "Timer.h"


// 타이머 측정을 준비한다 (QPC 빈도 확보 + 시작 틱 기록, 성공하면 true).
bool KYS::GAMESERVER::Timer::Initialize()
{
	// QPC 틱 빈도 확보 (실패하면 측정 불가)
	if (!QueryPerformanceFrequency(&m_frequency))
	{
		m_initialized = false;
		return false;
	}

	// 빈도 0이면 시간 변환 때 0으로 나누게 되므로 막음
	if (m_frequency.QuadPart == 0)
	{
		m_initialized = false;
		return false;
	}

	// 현재 틱을 경과 시간의 시작 기준으로 기록
	QueryPerformanceCounter(&m_startTime);

	m_initialized = true;

	return true;
}

// 기준 시각을 현재 틱으로 다시 잡는다 (경과 시간 0부터 다시). 초기화 안 됐으면 무시.
void KYS::GAMESERVER::Timer::Reset()
{
	if (m_initialized)
	{
		QueryPerformanceCounter(&m_startTime);
	}
}

// 시작 이후 경과 시간을 밀리초로 반환한다.
UINT64 KYS::GAMESERVER::Timer::GetCurrentTimeMS()
{
	if (!m_initialized)
	{
		return 0;
	}

	LARGE_INTEGER currentTime;
	QueryPerformanceCounter(&currentTime);

	UINT64 elapsedTicks = currentTime.QuadPart - m_startTime.QuadPart;


	// 곱셈을 먼저 하면 elapsedTicks * scale 가 UINT64 를 넘쳐 wrap(장기 가동). 몫/나머지로 분리해 오버플로 없이 환산.
	const UINT64 freq = static_cast<UINT64>(m_frequency.QuadPart);
	return (elapsedTicks / freq) * 1000ULL + (elapsedTicks % freq) * 1000ULL / freq;
}

// 시작 이후 경과 시간을 마이크로초로 반환한다.
UINT64 KYS::GAMESERVER::Timer::GetCurrentTimeMicro()
{
	if (!m_initialized)
	{
		return 0;
	}

	LARGE_INTEGER currentTime;
	QueryPerformanceCounter(&currentTime);

	UINT64 elapsedTicks = currentTime.QuadPart - m_startTime.QuadPart;


	// 곱셈을 먼저 하면 elapsedTicks * scale 가 UINT64 를 넘쳐 wrap(장기 가동). 몫/나머지로 분리해 오버플로 없이 환산.
	const UINT64 freq = static_cast<UINT64>(m_frequency.QuadPart);
	return (elapsedTicks / freq) * 1000000ULL + (elapsedTicks % freq) * 1000000ULL / freq;
}

// 시작 이후 경과 시간을 나노초로 반환한다.
UINT64 KYS::GAMESERVER::Timer::GetCurrentTimeNano()
{
	if (!m_initialized)
	{
		return 0;
	}

	LARGE_INTEGER currentTime;
	QueryPerformanceCounter(&currentTime);

	UINT64 elapsedTicks = currentTime.QuadPart - m_startTime.QuadPart;


	// 곱셈을 먼저 하면 elapsedTicks * scale 가 UINT64 를 넘쳐 wrap(장기 가동). 몫/나머지로 분리해 오버플로 없이 환산.
	const UINT64 freq = static_cast<UINT64>(m_frequency.QuadPart);
	return (elapsedTicks / freq) * 1000000000ULL + (elapsedTicks % freq) * 1000000000ULL / freq;
}
