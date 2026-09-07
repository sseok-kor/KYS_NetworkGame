#pragma once

// 워커 스레드가 실행할 "할 일 한 덩어리"를 추상화. JobQueue에 담겨 워커가 꺼내 Execute()를 호출한다.

namespace KYS
{
	namespace GAMESERVER
	{
		namespace THREAD
		{
			class IJob
			{
			public:
				IJob() = default;
				virtual ~IJob() = default;

				IJob(const IJob&) = delete;
				IJob& operator=(const IJob&) = delete;
				IJob(IJob&&) = delete;
				IJob& operator=(IJob&&) = delete;

				// 기대 역할: 이 작업의 실제 처리 내용 (워커 스레드가 호출)
				virtual void Execute() = 0;

			};
		}
	}
}
