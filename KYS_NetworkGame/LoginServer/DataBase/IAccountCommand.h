#pragma once
#include "../GameServer/Core/Thread/IJob.h"   // IJob (KYS::GAMESERVER::THREAD)

// MYSQL 타입은 pch_loginserver.h(mysql.h)로 들어온다. 이 헤더는 항상 pch 뒤에 포함된다.

namespace KYS
{
    namespace LOGINSERVER
    {
        // 계정 DB 작업 한 건. LoginDbThread 가 Execute() 를 호출하면 그 안에서 m_conn(연결)으로 INSERT 등을 수행한다.
        //   요청하는 쪽(LoginHandler)은 연결을 모르고, LoginDbThread 가 큐에서 꺼낸 뒤 SetConnection 으로 끼워 넣는다.
        //   ServerApp IDBCommand 미러 - 단 LoginServer 는 단일 백엔드라 IDBProvider seam 없이 raw MYSQL* 직접.
        //   복사/이동 4줄 =delete + virtual ~ 는 IJob base 상속(재선언 안 함).
        class IAccountCommand : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            void Execute() override = 0;   // 파생이 INSERT 등 DB 작업 구현

            // LoginDbThread 가 큐에서 꺼낸 직후 1회 호출 (producer 는 호출 안 함).
            void SetConnection(MYSQL* conn) { m_conn = conn; }

        protected:
            IAccountCommand() : m_conn(nullptr) {}   // m_conn 초기화 (파생 ctor 가 암시 호출)
            MYSQL* m_conn;                           // 비소유 (LoginDbThread 소유 연결을 빌려 씀)
        };
    }
}
