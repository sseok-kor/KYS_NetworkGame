#include "pch_serverapp.h"
#include "DBThread.h"
#include "../Game/Item/Item.h"   // SeedItemUid (부팅 시 uid 발급기 시드)
#include "../GameServer/Core/Log/Logger.h"   // 부팅/장애 이력 파일 로그
#include <process.h>   // _beginthreadex

namespace KYS
{
    namespace SERVERAPP
    {
        DBThread::DBThread()
            // 로그인(LoadCharacter)/저장(SaveFull) 명령은 절대 드롭 금지 - 드롭되면 DbResultJob 미생성 -> AttachPlayer 미호출
            //   -> 세션이 플레이어 없이 CCU 슬롯을 점유(로딩 데드라인 sweep 이 LOADING_DEADLINE_MS 뒤 걷어가지만, 그 전까지 슬롯 낭비 + 유저는 결과 없이 끊김). 그래서 DROP(기본) 아닌 GROW.
            //   풀 8192(> MAX_CCU 6000)는 fast-path, 그 이상 burst만 heap 노드로 이어감(처리 후 회수).
            : m_requestQueue(8192, KYS::GAMESERVER::THREAD::EOverflowPolicy::GROW)
            , m_provider()
            , m_config()
            , m_thread(NULL)
            , m_connectDone(NULL)
            , m_connectOk(false)
            , m_running(false)
        {
            // 연결 결과 신호용 수동 리셋 이벤트(Start 가 대기, ThreadLoop 의 Connect 후 SetEvent).
            m_connectDone = CreateEvent(NULL, TRUE, FALSE, NULL);
        }

        DBThread::~DBThread()
        {
            // main 이 Stop(=join+CloseHandle) 후 파괴. 잔여 명령은 m_requestQueue 소멸이 회수.
            if (m_connectDone != NULL)
            {
                CloseHandle(m_connectDone);
                m_connectDone = NULL;
            }
        }

        // 이 파일 안의 스레드 진입 함수 - this 의 ThreadLoop 를 바로 호출 (Start 를 부르면 무한 재귀).
        //   시그니처 unsigned __stdcall (void*) 고정. main.cpp 의 ChannelThreadEntry 와 같은 모양.
        static unsigned __stdcall DbThreadEntry(void* arg)
        {
            static_cast<DBThread*>(arg)->ThreadLoop();   // ThreadLoop 은 public 이라 바로 호출
            return 0;
        }

        bool DBThread::Start(const DbConfig& config)
        {
            m_config = config;   // ThreadLoop 가 Connect 에 넘길 연결 설정
            m_running = true;    // 스레드가 읽기 전에 true (Channel ctor m_running=true 정합)
            m_thread = reinterpret_cast<HANDLE>(
                _beginthreadex(NULL, 0, &DbThreadEntry, this, 0, NULL));
            if (m_thread == NULL)
            {
                return false;   // 스레드 생성 실패
            }
            // ThreadLoop 가 Connect 를 끝내고 SetEvent 할 때까지 대기 -> 연결 결과로 fail-fast 판정.
            WaitForSingleObject(m_connectDone, INFINITE);
            return m_connectOk;
        }

        void DBThread::Stop()
        {
            m_running = false;                           // 다음 루프 반복에 관측 (volatile)
            if (m_thread != NULL)
            {
                WaitForSingleObject(m_thread, INFINITE);  // ThreadLoop 반환 대기 (join)
                CloseHandle(m_thread);
                m_thread = NULL;
            }
        }

        void DBThread::Enqueue(IDBCommand* command)
        {
            m_requestQueue.Enqueue(command);   // producer (Channel/LoginLinkThread) - tail 락. IDBCommand* -> IJob*
        }

        void DBThread::ThreadLoop()
        {
            // 이 스레드의 MySQL 클라이언트 thread-local 초기화(C API 규칙). mysql_init 이 자동 호출하나 명시한다.
            mysql_thread_init();

            // 연결 + 스키마 부트스트랩(이 스레드 소유 - ctor 가 아니라 여기서 connect). 결과를 Start 에 신호.
            m_connectOk = m_provider.Connect(m_config);

            // 아이템 uid 발급기 시드 - 연결 직후 1회. 조회 실패면 부팅 중단으로 승격한다:
            //   시드 없이 0부터 재발급하면 DB 에 있는 기존 uid 와 충돌해 남의 아이템을 덮어쓰기 때문(부팅 성공 조건에 포함).
            if (m_connectOk)
            {
                UINT64 maxItemUid = 0;
                if (m_provider.LoadMaxItemUid(maxItemUid))
                {
                    SeedItemUid(maxItemUid);
                    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                        KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"db",
                        L"item_uid 시드 OK (DB max=%llu, 다음 발급 %llu부터)", maxItemUid, maxItemUid + 1);

                    // 거래 감사 hot/cold 이관 - 부팅 1회. 실패해도 부팅은 계속(hot 에 남고 다음 부팅 재시도).
                    m_provider.ArchiveOldTradeAudit();
                }
                else
                {
                    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                        KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_FATAL, L"db",
                        L"item_uid 시드 조회 실패 - 부팅 중단");
                    m_connectOk = false;
                }
            }
            SetEvent(m_connectDone);
            if (!m_connectOk)
            {
                // Connect 자체가 실패했으면 m_conn 은 이미 nullptr 이라 no-op 이지만,
                //   연결은 됐는데 시드 조회에서 실패한 경로는 연결이 열린 채 여기 온다.
                //   main 은 이 실패 경로에서도 mysql_library_end() 를 부르므로 그 전에 닫아야 한다.
                m_provider.Disconnect();
                mysql_thread_end();
                return;   // 연결 실패 -> Start 가 false 반환 -> main 이 fail-fast.
            }

            // 단일 consumer. Channel TickLoop 의 큐 비우기와 같은 방식. DB I/O 는 여기(전용 스레드)서만.
            while (m_running)
            {
                KYS::GAMESERVER::THREAD::IJob* job = nullptr;
                while (m_requestQueue.Dequeue(job))
                {
                    // 큐엔 IDBCommand* 만 들어온다(Enqueue 가 IDBCommand* 만 받음) -> 다운캐스트 안전.
                    IDBCommand* command = static_cast<IDBCommand*>(job);
                    command->SetProvider(&m_provider);   // provider 주입 (producer 는 provider 를 모름, DBThread 가 소유)
                    command->Execute();                  // provider 작업 함수 호출 / Load 면 DbResultJob enqueue
                    delete command;                      // 소비자 회수 (OnRecvJob 동적할당 정합)
                }
                ::Sleep(1);   // 큐 비면 짧게 쉼 (DB 는 저빈도 - 바쁜 대기 회피, 1ms 무해)
            }

            KYS::GAMESERVER::THREAD::IJob* job = nullptr;
            while (m_requestQueue.Dequeue(job))
            {
                // 큐엔 IDBCommand* 만 들어온다(Enqueue 가 IDBCommand* 만 받음) -> 다운캐스트 안전.
                IDBCommand* command = static_cast<IDBCommand*>(job);
                command->SetProvider(&m_provider);   // provider 주입 (producer 는 provider 를 모름, DBThread 가 소유)
                command->Execute();                  // provider 작업 함수 호출 / Load 면 DbResultJob enqueue
                delete command;                      // 소비자 회수 (OnRecvJob 동적할당 정합)
            }

            m_provider.Disconnect();   // 이 스레드가 연 연결을 이 스레드가 닫음 - main 의 mysql_library_end() 보다 먼저여야 한다
            mysql_thread_end();        // 이 스레드의 MySQL thread-local 정리(누수 방지).
        }
    }
}
