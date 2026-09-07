#pragma once
#include "../../GameServer/Core/Thread/IJob.h"
#include "../../GameCommon/GameDefines.h"   // WHISPER_NAME_MAX
#include <cwchar>                           // wcscpy_s

namespace KYS
{
    namespace SERVERAPP
    {
        class SharedServicesThread;   // 전방 선언

        // 디렉터리 write를 SST 스레드 한 곳으로 모은다 (로그인/로그아웃은 다른 스레드 -> Job 위임).
        class RegisterNameJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            RegisterNameJob(const wchar_t* name, UINT64 sid)
                : m_sid(sid)
            {
                wcscpy_s(m_name, WHISPER_NAME_MAX, name);   // 값 스냅샷
            }
            ~RegisterNameJob() override = default;
            void Execute() override;
        private:
            wchar_t               m_name[WHISPER_NAME_MAX];
            UINT64                m_sid;
        };

        class UnregisterNameJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            UnregisterNameJob(const wchar_t* name, UINT64 sid)   // sid 동봉
                : m_sid(sid)
            {
                wcscpy_s(m_name, WHISPER_NAME_MAX, name);   // 값 스냅샷
            }
            ~UnregisterNameJob() override = default;
            void Execute() override;
        private:
            wchar_t               m_name[WHISPER_NAME_MAX];
            UINT64                m_sid;   // 등록 시 sid - 일치 검사로 재로그인 race 봉인
        };
    }
}
