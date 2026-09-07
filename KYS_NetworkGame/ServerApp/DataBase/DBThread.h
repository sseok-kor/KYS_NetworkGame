#pragma once
#include "IDBCommand.h"                                       // IDBCommand (큐 원소)
#include "MySqlDBProvider.h"                                  // 값 멤버 (MySQL 백엔드)
#include "../../GameServer/Core/Config/DbConfig.h"            // 연결 설정 (Start 인자 + 멤버)
#include "../../GameServer/Core/Thread/TwoLockJobQueue.h"     // IJob* 전용 mailbox (꺾쇠 없는 비-템플릿)
#include "../../GameServer/Core/Thread/IJob.h"
#include <Windows.h>                                          // HANDLE

namespace KYS
{
    namespace SERVERAPP
    {
        // DB 전용 스레드. 모든 DB 요청(load/save)을 1 큐로 받아 1 스레드가 순차 처리.
        //   30Hz Channel 을 한 번도 막지 않는다 (Channel 은 Enqueue 후 즉시 반환). 백엔드는 IDBProvider 로 교체(CSV->MySQL).
        //   ThreadLoop 은 public 이라 file-local 진입 함수(DbThreadEntry)가 바로 호출 (Channel::TickLoop 과 같은 방식).
        class DBThread
        {
        public:
            static DBThread& GetInstance() { static DBThread instance; return instance; }   // Meyers static

            DBThread(const DBThread&) = delete;            // 복사 2줄 (스레드/큐 단일 소유, 이동은 자동 미선언)
            DBThread& operator=(const DBThread&) = delete;

            bool Start(const DbConfig& config);            // 스레드 기동 + DB 연결(스레드측). 실패=false(fail-fast).
            void Stop();                                   // m_running=false + WaitForSingleObject + CloseHandle
            void Enqueue(IDBCommand* command);             // producer (Channel/LoginLinkThread) -> tail 락

            void ThreadLoop();                             // 스레드 본체 (Dequeue -> SetProvider -> Execute -> delete)

        private:
            DBThread();     // 싱글턴 - GetInstance로만 생성
            ~DBThread();

            KYS::GAMESERVER::THREAD::TwoLockJobQueue m_requestQueue;   // IJob* 큐 (IDBCommand* 만 들어옴)
            MySqlDBProvider                          m_provider;        // MySQL 백엔드 (연결은 ThreadLoop 진입부 Connect)
            DbConfig                                 m_config;          // 연결 설정 (Start 가 받아 ThreadLoop 가 Connect 에 전달)
            HANDLE                                   m_thread;          // _beginthreadex 핸들 (Stop 에서 join/close)
            HANDLE                                   m_connectDone;     // 연결 결과 신호 (Start 가 대기 -> fail-fast 판정)
            volatile bool                            m_connectOk;       // ThreadLoop 의 Connect 성공 여부
            volatile bool                            m_running;         // ThreadLoop 종료 플래그
        };
    }
}
