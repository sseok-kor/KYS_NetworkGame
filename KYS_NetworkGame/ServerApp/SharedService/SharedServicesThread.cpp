#include "pch_serverapp.h"
#include "SharedServicesThread.h"
#include <Windows.h>   // Sleep

namespace KYS
{
    namespace SERVERAPP
    {
        // FNV-1a 64-bit - wchar_t 단위로 종단 전까지 누산.
        size_t NameKeyHash::operator()(const NameKey& key) const
        {
            size_t h = 1469598103934665603ULL;   // FNV offset basis
            for (int i = 0; i < WHISPER_NAME_MAX && key.name[i] != L'\0'; ++i)
            {
                h ^= static_cast<size_t>(key.name[i]);
                h *= 1099511628211ULL;            // FNV prime
            }
            return h;
        }

        SharedServicesThread::SharedServicesThread()
            : m_queue()
            , m_nameDirectory()
            , m_crossChannelChat()   // 무인자 (CrossChannelChat 멤버 없음 - Route가 GetInstance 직접)
            , m_running(true)   // ctor에서 true (스레드 생성 전). Run() 본체에서 설정하면 Stop과 race.
        {
        }

        SharedServicesThread::~SharedServicesThread()
        {
            // 큐는 IJob*를 소유하지 않는다(노드만 회수). 정상 종료 경로는 Run() 말미의
            // 최종 비우기가 잔여 Job을 delete하므로 여기 도달 시 큐는 비어 있다.
            // main이 Stop() + 스레드 join 후 파괴한다(순서 보장).
        }

        void SharedServicesThread::EnqueueJob(KYS::GAMESERVER::THREAD::IJob* job)
        {
            m_queue.Enqueue(job);   // 큐 tail에 넣음 (tail 락) - 발신 Channel/로그인 스레드가 호출
        }

        void SharedServicesThread::Run()
        {
            // _beginthreadex 진입점이 호출. Channel TickLoop의 mailbox 비우기를 그대로 따른다.
            while (m_running)
            {
                KYS::GAMESERVER::THREAD::IJob* job = nullptr;
                while (m_queue.Dequeue(job))
                {
                    job->Execute();   // 디렉터리 쓰기 / 라우팅 = 전부 이 스레드 한 곳에서만 (쓰기 단일화)
                    delete job;       // 소비자 회수
                }
                Sleep(1);   // busy-spin 회피 (저빈도 cross-channel, 간격은 측정 후 조정)
            }

            // 루프 종료 후 큐 마지막 비우기 1회 - 잔여 Job 누수 방지.
            // 이때 죽은 채널로 PostToChannel되는 WhisperDeliverJob은 프로세스 종료 직전이라 무해.
            KYS::GAMESERVER::THREAD::IJob* job = nullptr;
            while (m_queue.Dequeue(job))
            {
                job->Execute();
                delete job;
            }
        }

        void SharedServicesThread::Stop()
        {
            m_running = false;
        }

        void SharedServicesThread::RegisterName(const wchar_t* name, UINT64 sid)
        {
            // 이 스레드에서만 (RegisterNameJob::Execute 경유). 디렉터리 쓰기 단일화.
            NameKey key;
            wcscpy_s(key.name, WHISPER_NAME_MAX, name);
            m_nameDirectory[key] = sid;
        }

        void SharedServicesThread::UnregisterName(const wchar_t* name, UINT64 sid)
        {
            NameKey key;
            wcscpy_s(key.name, WHISPER_NAME_MAX, name);
            // 저장된 sid가 이 Job의 sid와 같을 때만 erase. 같은 이름으로 빠르게 재로그인하면
            // 새 등록이 먼저 디렉터리에 박힐 수 있는데, 옛 로그아웃 Job이 무조건 erase하면
            // 새 등록을 지워버려 새 접속이 귓속말을 못 받는다. sid 일치 검사로 그 race를 봉인.
            std::unordered_map<NameKey, UINT64, NameKeyHash>::iterator it = m_nameDirectory.find(key);
            if (it != m_nameDirectory.end() && it->second == sid)
            {
                m_nameDirectory.erase(it);
            }
        }

        UINT64 SharedServicesThread::ResolveName(const wchar_t* name) const
        {
            NameKey key;
            wcscpy_s(key.name, WHISPER_NAME_MAX, name);
            std::unordered_map<NameKey, UINT64, NameKeyHash>::const_iterator it = m_nameDirectory.find(key);
            if (it == m_nameDirectory.end())
            {
                return 0;   // 미발견/오프라인
            }
            return it->second;
        }

        CrossChannelChat* SharedServicesThread::GetRouter()
        {
            return &m_crossChannelChat;
        }
    }
}
