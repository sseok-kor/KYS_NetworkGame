#pragma once
#include "CrossChannelChat.h"
#include "../../GameServer/Core/Thread/TwoLockJobQueue.h"   // IJob* 전용 mailbox (비-템플릿, 꺾쇠 없음)
#include "../../GameServer/Core/Thread/IJob.h"
#include "../../GameCommon/GameDefines.h"                   // WHISPER_NAME_MAX
#include <cwchar>   // wcscmp / wcscpy_s
#include <unordered_map>

namespace KYS
{
    namespace SERVERAPP
    {
        class ChannelManager;   // 전방 선언 (CrossChannelChat이 보유/라우팅)

        // name 키 래퍼 - std::wstring 대신 고정 wchar_t 배열 + wcscmp 비교.
        struct NameKey
        {
            wchar_t name[WHISPER_NAME_MAX];
            bool operator==(const NameKey& other) const
            {
                return wcscmp(name, other.name) == 0;
            }
        };
        // wchar_t 단위 FNV-1a 해시 - std::wstring 미사용, 힙 할당 0.
        struct NameKeyHash
        {
            size_t operator()(const NameKey& key) const;
        };

        // cross-channel 전용 스레드. Channel의 _beginthreadex + 큐 비우기 루프 패턴을 그대로 따른다.
        //   name->sid 디렉터리를 이 스레드만 읽고 쓴다. 미래 길드/파티 공유 거점.
        class SharedServicesThread
        {
        public:
            static SharedServicesThread& GetInstance() { static SharedServicesThread instance; return instance; }   // Meyers static

            SharedServicesThread(const SharedServicesThread&) = delete;             // 복사 2줄 (단일 소유, 이동은 자동 미선언)
            SharedServicesThread& operator=(const SharedServicesThread&) = delete;

            void EnqueueJob(KYS::GAMESERVER::THREAD::IJob* job);   // 다른 스레드가 Job을 큐에 넣음 (발신 Channel/로그인 스레드)
            void Run();                                            // _beginthreadex 진입점, 큐 비우기 루프
            void Stop();                                           // m_running = false (main 종료)

            // 디렉터리 접근 - 이 스레드에서만 호출 (Job Execute 내부 / CrossChannelChat)
            void   RegisterName(const wchar_t* name, UINT64 sid);
            void   UnregisterName(const wchar_t* name, UINT64 sid);   // 저장 sid 일치 시에만 erase
            UINT64 ResolveName(const wchar_t* name) const;            // name->sid (0 = 미발견)

            CrossChannelChat* GetRouter();   // OnChat이 WhisperRouteJob 생성 시 router 획득

        private:
            SharedServicesThread();    // 싱글턴 - GetInstance로만 생성 (무인자, m_crossChannelChat 값 멤버 기본 구성)
            ~SharedServicesThread();

            KYS::GAMESERVER::THREAD::TwoLockJobQueue          m_queue;            // mailbox
            std::unordered_map<NameKey, UINT64, NameKeyHash>  m_nameDirectory;    // name->sid 단일 소유
            CrossChannelChat                                  m_crossChannelChat; // 귓속말 라우팅 (도메인 로직, 값 멤버)
            volatile bool                                     m_running;          // Run 루프 종료 플래그
        };
    }
}
