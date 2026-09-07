#pragma once
#include "pch_tests.h"

namespace KYS
{
	namespace TESTS
	{
		// "총량 보존 == 정확성"을 검사하는 도구.
		// 큐/풀이 각 항목을 정확히 한 번씩만 넘겼는지 (중복 0, 유실 0) 확인한다.
		//
		// 설계: 소비 스레드는 자기 칸(m_perThread[threadIndex])에만 기록하므로
		//       스레드 간 공유가 없어 락이 필요 없다(테스트 대상 락과 무관한 순수 프로브).
		//       조인 후 메인 스레드가 전부 합쳐서 단일 스레드로 검사한다.
		class UniqueIdLedger
		{
		public:
			explicit UniqueIdLedger(DWORD threadCount)
				: m_perThread(threadCount)
			{
			}

			// 소비 스레드가 소비한 id 하나를 자기 칸에 기록 (락 0)
			void Record(DWORD threadIndex, UINT32 id)
			{
				m_perThread[threadIndex].push_back(id);
			}

			// 지금까지 기록된 총 소비 수(진단용 스냅샷 - 조인 전 호출 시 근사값).
			size_t TotalRecorded() const
			{
				size_t total = 0;
				for (size_t t = 0; t < m_perThread.size(); ++t)
				{
					total += m_perThread[t].size();
				}
				return total;
			}

			// 조인 후 메인에서 호출: 총량 == expectedCount && 중복 0 && 유실 0 && 범위 안.
			//   firstId..lastId : 발급한 id 범위(닫힌 구간) - 범위 밖 id는 손상으로 본다.
			bool Verify(size_t expectedCount, UINT32 firstId, UINT32 lastId)
			{
				std::unordered_set<UINT32> seen;
				size_t total = 0;

				for (size_t t = 0; t < m_perThread.size(); ++t)
				{
					const std::vector<UINT32>& bucket = m_perThread[t];
					total += bucket.size();

					for (size_t k = 0; k < bucket.size(); ++k)
					{
						UINT32 id = bucket[k];
						if (id < firstId || id > lastId)
						{
							return false;                 // 범위 밖 = 손상된 값
						}
						if (!seen.insert(id).second)
						{
							return false;                 // 중복 = 이중 발급/이중 소비
						}
					}
				}
				// 총량 일치 + 고유 개수 일치 = 유실 0
				return total == expectedCount && seen.size() == expectedCount;
			}

		private:
			std::vector<std::vector<UINT32>> m_perThread;  // 스레드별 소비 목록(칸이 안 겹침)
		};
	}
}
