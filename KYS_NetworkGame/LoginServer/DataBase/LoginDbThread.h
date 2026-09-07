#pragma once
#include "IAccountCommand.h"                                  // IAccountCommand (큐 원소)
#include "../../GameServer/Core/Config/DbConfig.h"            // 연결 설정 (Start 인자 + 멤버)
#include "../GameServer/Core/Thread/TwoLockJobQueue.h"        // IJob* 전용 mailbox (꺾쇠 없는 비-템플릿)
#include "../GameServer/Core/Thread/IJob.h"
#include <Windows.h>                                          // HANDLE

// MYSQL* 은 pch_loginserver.h(mysql.h)로 들어온다(이 헤더는 항상 pch 뒤에 포함).

namespace KYS
{
    namespace LOGINSERVER
    {
        // 계정 DB 전용 스레드. 인게임 계정 생성(CS_REGISTER)의 PBKDF2 해시 + accounts INSERT 를 1 큐로 받아 1 스레드가 순차 처리.
        //   IOCP 워커를 한 번도 막지 않는다(Enqueue 후 즉시 반환). PBKDF2(~200-490ms)를 여기서 돌려 워커 블로킹/메모리 DoS 를 막는다.
        //   ServerApp DBThread 와 같은 구조(단일 consumer FIFO) - LoginServer 는 단일 백엔드라 IDBProvider seam 없이 raw MYSQL* 직접.
        class LoginDbThread
        {
        public:
            static LoginDbThread& GetInstance() { static LoginDbThread instance; return instance; }   // Meyers static

            LoginDbThread(const LoginDbThread&) = delete;            // 복사 2줄 (스레드/큐 단일 소유, 이동은 자동 미선언)
            LoginDbThread& operator=(const LoginDbThread&) = delete;

            bool Start(const DbConfig& config);            // 스레드 기동 + DB 연결(스레드측). 실패=false(fail-fast).
            void Stop();                                   // m_running=false + WaitForSingleObject + CloseHandle
            void Enqueue(IAccountCommand* command);        // producer (LoginHandler) -> tail 락

            void ThreadLoop();                             // 스레드 본체 (Dequeue -> SetConnection -> Execute -> delete)

            static volatile LONG s_reconnectCount;         // 재접속 성공 누적 (모니터 표시 - InterServerDispatcher s_verify* 관용구)

        private:
            LoginDbThread();     // 싱글턴 - GetInstance로만 생성
            ~LoginDbThread();

            // 연결(init + charset + real_connect) 1회 시도. 성공 시 m_conn 세팅 / 실패 시 로그 + m_conn=nullptr.
            //   부팅(ThreadLoop 진입부)과 런타임 재접속(EnsureConnectionAlive)이 공용.
            bool TryConnect();

            // 커맨드 실행 직전 게이트 - 끊겨 있으면 재접속(최대 5회, 3초 간격), 전부 실패 시 프로세스 종료.
            //   IAccountCommand::Execute 는 실패를 반환하지 않아(void) 사후 감지가 불가하고, 계정 생성은
            //   저빈도라 커맨드별 mysql_ping 비용이 무시 가능 - 요청이 있을 때만 확인하는 reactive 복구.
            void EnsureConnectionAlive();

            KYS::GAMESERVER::THREAD::TwoLockJobQueue m_requestQueue;   // IJob* 큐 (IAccountCommand* 만 들어옴)
            DbConfig                                 m_config;          // 연결 설정 (Start 가 받아 ThreadLoop 가 connect 에 전달)
            MYSQL*                                   m_conn;            // DB 연결 (ThreadLoop 진입부 connect, 종료부 close - 이 스레드 단독 소유)
            HANDLE                                   m_thread;          // _beginthreadex 핸들 (Stop 에서 join/close)
            HANDLE                                   m_connectDone;     // 연결 결과 신호 (Start 가 대기 -> fail-fast 판정)
            volatile bool                            m_connectOk;       // ThreadLoop 의 connect 성공 여부
            volatile bool                            m_running;         // ThreadLoop 종료 플래그
        };
    }
}
