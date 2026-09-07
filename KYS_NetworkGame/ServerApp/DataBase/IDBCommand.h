#pragma once
#include "../../GameServer/Core/Thread/IJob.h"   // IJob (KYS::GAMESERVER::THREAD)

namespace KYS
{
    namespace SERVERAPP
    {
        class IDBProvider;   // 전방 선언 (포인터만 보유 - DBThread 가 큐에서 꺼낸 뒤 주입)

        // DB 작업 한 건. DBThread 가 Execute() 를 호출하면 그 안에서 m_provider 의 알맞은 작업 함수(Load/Save)를 부른다.
        //   요청하는 쪽(Channel)은 백엔드(CSV/MySQL)를 모르고, provider 는 DBThread 가 끼워 넣는다.
        //   복사/이동 4줄 =delete + virtual ~ 는 IJob base 상속 (재선언 안 함).
        class IDBCommand : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            // IJob::Execute 를 순수가상 그대로 둠 (파생이 provider 작업 함수 호출을 구현).
            void Execute() override = 0;

            // DBThread 가 큐에서 꺼낸 직후 1회 호출 (producer 는 호출 안 함).
            void SetProvider(IDBProvider* provider) { m_provider = provider; }

        protected:
            IDBCommand() : m_provider(nullptr) {}   // m_provider 초기화 (파생 ctor 가 암시 호출)
            IDBProvider* m_provider;                // 비소유 (DBThread 소유 자원을 빌려 씀)
        };
    }
}
