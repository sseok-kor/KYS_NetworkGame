#pragma once
#include "pch_tests.h"
#include "Core/Thread/IJob.h"   // KYS::GAMESERVER::THREAD::IJob (GameServer.lib)

namespace KYS
{
	namespace TESTS
	{
		// 작업 실행/소멸/처리한 id를 스레드 안전하게 세는 계측 상태.
		// (MockJob의 전역 카운터와 달리 테스트별 인스턴스로 격리되고, id별 보존 집합을 가진다.)
		struct JobProbe
		{
			volatile LONG executeCount;   // Execute() 총 호출 수
			volatile LONG destroyCount;   // ~ProbeJob() 총 호출 수
			LONG*         seen;           // id별 처리 횟수 (보존 집합, 크기 seenCount)
			UINT32        seenCount;      // seen 배열 크기 (id 상한)
		};

		// 계측 상태를 초기화. seenArray는 호출자가 소유(스택/힙), n칸 0으로 채운다.
		inline void ResetProbe(JobProbe& probe, LONG* seenArray, UINT32 n)
		{
			probe.executeCount = 0;
			probe.destroyCount = 0;
			probe.seen = seenArray;
			probe.seenCount = n;
			for (UINT32 i = 0; i < n; ++i) seenArray[i] = 0;
		}

		// IJob 오라클: 실행 1회당 카운터/보존집합을 올리고, 소멸 1회당 소멸 카운터를 올린다.
		// exactly-once 실행이면 executeCount==발급수 && 모든 seen==1.
		// exactly-once delete면 destroyCount==발급수. 이중 delete는 App Verifier + destroyCount로 포착.
		class ProbeJob : public KYS::GAMESERVER::THREAD::IJob
		{
		public:
			ProbeJob(int id, JobProbe* probe)
				: m_id(id), m_probe(probe)
			{
			}

			// 다형 delete가 이 소멸자를 타는지(virtual ~IJob) 검증하는 지점.
			virtual ~ProbeJob()
			{
				::InterlockedIncrement(&m_probe->destroyCount);
			}

			virtual void Execute() override
			{
				::InterlockedIncrement(&m_probe->executeCount);
				if (m_id >= 0 && static_cast<UINT32>(m_id) < m_probe->seenCount)
				{
					::InterlockedIncrement(&m_probe->seen[m_id]);
				}
			}

			int GetId() const { return m_id; }

			// 복사/이동은 base IJob에서 이미 =delete -> 파생도 암묵 삭제. 슬라이싱/부분이동 차단.

		private:
			int       m_id;
			JobProbe* m_probe;
		};
	}
}
