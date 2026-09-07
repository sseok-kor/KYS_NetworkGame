#pragma once
#include "pch_tests.h"
#include "Core/Thread/IJob.h"   // KYS::GAMESERVER::THREAD::IJob (GameServer.lib)

namespace KYS
{
	namespace TESTS
	{
		// IJob 계약을 검증하는 계측용 Job (Test Spy).
		//   - Execute()가 정확히 1회 불렸는가 (m_executed)
		//   - 다형 delete가 파생 소멸자를 실행하는가 (s_destroyCount)
		//   - 어느 Job이 유실/중복됐는가 (m_jobId = 보존 검증용 고유 id)
		// 여러 워커가 동시에 Execute/delete 하므로 카운터는 전부 Interlocked.
		class MockJob : public KYS::GAMESERVER::THREAD::IJob
		{
		public:
			// 각 테스트 시작 시 전역 카운터를 0으로 되돌린다.
			static void ResetCounters()
			{
				::InterlockedExchange(&s_executeCount, 0);
				::InterlockedExchange(&s_destroyCount, 0);
			}

			// 전역 카운터의 원자적 읽기 (InterlockedCompareExchange로 현재값 획득)
			static LONG GetExecuteCount()
			{
				return ::InterlockedCompareExchange(&s_executeCount, 0, 0);
			}
			static LONG GetDestroyCount()
			{
				return ::InterlockedCompareExchange(&s_destroyCount, 0, 0);
			}

			explicit MockJob(UINT32 jobId)
				: m_jobId(jobId), m_executed(0)
			{
			}

			// 다형 delete가 이 소멸자를 부르는지 = virtual ~IJob() 검증 지점
			virtual ~MockJob()
			{
				::InterlockedIncrement(&s_destroyCount);
			}

			// 워커 스레드가 호출. 이 Job이 2번 실행되면 m_executed로 잡힌다.
			virtual void Execute() override
			{
				::InterlockedIncrement(&m_executed);
				::InterlockedIncrement(&s_executeCount);
			}

			UINT32 GetJobId() const          { return m_jobId; }
			LONG   GetOwnExecuteCount() const
			{
				return ::InterlockedCompareExchange(
					const_cast<volatile LONG*>(&m_executed), 0, 0);
			}

			// IJob이 복사/이동을 4줄 =delete 했으므로 MockJob도 자동으로 비복사/비이동.
			// (멤버를 추가해도 base가 삭제했으므로 암시적으로 삭제됨 - slicing/부분이동 차단)

		private:
			UINT32        m_jobId;    // 고유 id (보존 집합 검증용)
			volatile LONG m_executed; // 이 인스턴스가 실행된 횟수 (정확히 1이어야 함)

			static volatile LONG s_executeCount;  // 전체 Execute 호출 수
			static volatile LONG s_destroyCount;  // 전체 소멸 수
		};
	}
}
