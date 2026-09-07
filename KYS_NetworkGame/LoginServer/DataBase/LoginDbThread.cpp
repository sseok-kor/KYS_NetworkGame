#include "pch_loginserver.h"
#include "LoginDbThread.h"
#include "../GameServer/Core/Log/Logger.h"   // DB 장애 이력 파일 로그
#include <process.h>   // _beginthreadex

namespace KYS
{
    namespace LOGINSERVER
    {
        using KYS::GAMESERVER::LOG::LogLevel;

        // 파일-로컬 로그 헬퍼 - DB 계층 이벤트를 server 채널에 남긴다 (ServerApp DbLog 미러).
        template<typename... Args>
        static void ServerLog(LogLevel level, const wchar_t* tag, const wchar_t* format, Args&&... args)
        {
            KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                KYS::GAMESERVER::LOG::LogChannel::SERVER, level, tag, format, std::forward<Args>(args)...);
        }

        // 재접속 정책: 연결 끊김 감지 시 최대 5회, 3초 간격 - ServerApp MySqlDBProvider 와 동일 값.
        static const int   DB_RECONNECT_MAX_TRIES = 5;
        static const DWORD DB_RECONNECT_RETRY_MS  = 3000;

        volatile LONG LoginDbThread::s_reconnectCount = 0;

        LoginDbThread::LoginDbThread()
            // 계정 생성 명령은 드롭 금지(드롭되면 SC_REGISTER_RESULT 미회신 -> 클라 영구 대기). 그래서 DROP(기본) 아닌 GROW.
            //   풀 8192 는 fast-path, 그 이상 burst 만 heap 노드로 이어감(처리 후 회수). 계정 생성은 저빈도라 사실상 풀 안에서 끝.
            : m_requestQueue(8192, KYS::GAMESERVER::THREAD::EOverflowPolicy::GROW)
            , m_config()
            , m_conn(nullptr)
            , m_thread(NULL)
            , m_connectDone(NULL)
            , m_connectOk(false)
            , m_running(false)
        {
            // 연결 결과 신호용 수동 리셋 이벤트(Start 가 대기, ThreadLoop 의 connect 후 SetEvent).
            m_connectDone = CreateEvent(NULL, TRUE, FALSE, NULL);
        }

        LoginDbThread::~LoginDbThread()
        {
            // main 이 Stop(=join+CloseHandle) 후 파괴. m_conn 은 ThreadLoop 가 닫음.
            // 잔여 명령은 ThreadLoop 종료 직전 drain 이 Execute 후 delete 로 회수(큐 소멸자는 노드만 반납).
            if (m_connectDone != NULL)
            {
                CloseHandle(m_connectDone);
                m_connectDone = NULL;
            }
        }

        // 이 파일 안의 스레드 진입 함수 - this 의 ThreadLoop 를 바로 호출(Start 를 부르면 무한 재귀).
        static unsigned __stdcall LoginDbThreadEntry(void* arg)
        {
            static_cast<LoginDbThread*>(arg)->ThreadLoop();
            return 0;
        }

        bool LoginDbThread::Start(const DbConfig& config)
        {
            m_config = config;   // ThreadLoop 가 connect 에 넘길 연결 설정
            m_running = true;    // 스레드가 읽기 전에 true
            m_thread = reinterpret_cast<HANDLE>(
                _beginthreadex(NULL, 0, &LoginDbThreadEntry, this, 0, NULL));
            if (m_thread == NULL)
            {
                return false;   // 스레드 생성 실패
            }
            // ThreadLoop 가 connect 를 끝내고 SetEvent 할 때까지 대기 -> 연결 결과로 fail-fast 판정.
            WaitForSingleObject(m_connectDone, INFINITE);
            return m_connectOk;
        }

        void LoginDbThread::Stop()
        {
            m_running = false;                            // 다음 루프 반복에 관측 (volatile)
            if (m_thread != NULL)
            {
                WaitForSingleObject(m_thread, INFINITE);  // ThreadLoop 반환 대기 (join)
                CloseHandle(m_thread);
                m_thread = NULL;
            }
        }

        void LoginDbThread::Enqueue(IAccountCommand* command)
        {
            m_requestQueue.Enqueue(command);   // producer (LoginHandler) - tail 락. IAccountCommand* -> IJob*
        }

        // 연결 1회 시도 (부팅/재접속 공용). 성공 시 m_conn 세팅 + true / 실패 시 로그 + m_conn=nullptr + false.
        bool LoginDbThread::TryConnect()
        {
            m_conn = mysql_init(nullptr);
            if (m_conn == nullptr)
            {
                return false;   // 핸들 할당 실패(OOM)
            }
            mysql_options(m_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");   // 한글(utf8mb4), connect 전 옵션

            // 네트워크 타임아웃 - 응답 없는 단절(silent-drop)에서도 연결/쿼리/ping 각 시도가 유한 시간에
            //   끝나게 해 재접속 정책(5회x3초)의 시간 상한을 지킨다 (ServerApp MySqlDBProvider 와 동일 값).
            unsigned int connectTimeoutSec = 10;
            unsigned int rwTimeoutSec      = 30;
            mysql_options(m_conn, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeoutSec);
            mysql_options(m_conn, MYSQL_OPT_READ_TIMEOUT,    &rwTimeoutSec);
            mysql_options(m_conn, MYSQL_OPT_WRITE_TIMEOUT,   &rwTimeoutSec);

            if (mysql_real_connect(m_conn, m_config.host, m_config.user, m_config.password,
                                   m_config.dbname, static_cast<unsigned int>(m_config.port),
                                   nullptr, 0) == nullptr)
            {
                // 치명 여부는 호출측이 판정(부팅 = fail-fast / 재접속 루프 = 재시도).
                ServerLog(LogLevel::LL_ERROR, L"db", L"mysql_real_connect 실패: %S", mysql_error(m_conn));
                mysql_close(m_conn);
                m_conn = nullptr;
                return false;
            }
            return true;
        }

        // 커맨드 실행 직전 게이트. mysql_ping 으로 연결 생사를 확인하고, 죽었으면 명시 재접속.
        //   MYSQL_OPT_RECONNECT 는 쓰지 않으므로 ping 이 몰래 재접속하는 일은 없다 - 순수 생사 확인.
        //   재접속이 전부 실패하면 프로세스 종료 - DB 없이는 로그인/계정생성이 전부 막힌 반쪽 서버라 즉시 크게 알린다.
        void LoginDbThread::EnsureConnectionAlive()
        {
            if (m_conn != nullptr && mysql_ping(m_conn) == 0)
            {
                return;   // 연결 정상
            }

            ServerLog(LogLevel::LL_WARN, L"db", L"연결 끊김 감지 - 재접속 시도 (최대 %d회, %lums 간격)",
                      DB_RECONNECT_MAX_TRIES, DB_RECONNECT_RETRY_MS);
            for (int attempt = 1; attempt <= DB_RECONNECT_MAX_TRIES; ++attempt)
            {
                if (m_conn != nullptr)
                {
                    mysql_close(m_conn);   // 죽은 핸들 폐기 - TryConnect 가 새로 init
                    m_conn = nullptr;
                }
                if (TryConnect())
                {
                    InterlockedIncrement(&s_reconnectCount);
                    ServerLog(LogLevel::LL_WARN, L"db", L"재접속 성공 (%d번째 시도)", attempt);
                    return;
                }
                ::Sleep(DB_RECONNECT_RETRY_MS);
            }

            ServerLog(LogLevel::LL_FATAL, L"db", L"재접속 %d회 전부 실패 - 프로세스 종료", DB_RECONNECT_MAX_TRIES);
            ::ExitProcess(1);
        }

        void LoginDbThread::ThreadLoop()
        {
            // 이 스레드의 MySQL 클라이언트 thread-local 초기화(C API 규칙).
            mysql_thread_init();

            // 연결(이 스레드 소유 - ctor 가 아니라 여기서 connect). 결과를 Start 에 신호.
            //   실패 시 [logindb] 로그는 TryConnect 가 찍고, fail-fast 판정은 Start(main)가 한다.
            m_connectOk = TryConnect();
            SetEvent(m_connectDone);
            if (!m_connectOk)
            {
                mysql_thread_end();
                return;   // 연결 실패 -> Start 가 false 반환 -> main 이 fail-fast.
            }

            // 단일 consumer. DB I/O(+PBKDF2 해시)는 여기(전용 스레드)서만.
            while (m_running)
            {
                KYS::GAMESERVER::THREAD::IJob* job = nullptr;
                while (m_requestQueue.Dequeue(job))
                {
                    IAccountCommand* command = static_cast<IAccountCommand*>(job);
                    EnsureConnectionAlive();          // 끊겨 있으면 재접속 후 진행 (실패 지속이면 프로세스 종료)
                    command->SetConnection(m_conn);   // 연결 주입 (producer 는 연결을 모름)
                    command->Execute();               // PBKDF2 해시 + INSERT + 결과 Send
                    delete command;                   // 소비자 회수
                }
                ::Sleep(1);   // 큐 비면 짧게 쉼 (계정 생성은 저빈도)
            }

            // 종료 전 잔여 명령 마저 처리(미회신 방지).
            KYS::GAMESERVER::THREAD::IJob* job = nullptr;
            while (m_requestQueue.Dequeue(job))
            {
                IAccountCommand* command = static_cast<IAccountCommand*>(job);
                EnsureConnectionAlive();
                command->SetConnection(m_conn);
                command->Execute();
                delete command;
            }

            mysql_close(m_conn);   // 이 스레드가 연 연결을 이 스레드가 닫음
            m_conn = nullptr;
            mysql_thread_end();    // 이 스레드의 MySQL thread-local 정리
        }
    }
}
