#pragma once
#include "IAccountCommand.h"
#include "../GameCommon/Protocol/GamePackets.h"   // LoginReq::ID_MAX / PASSWORD_MAX(GameDefines)

namespace KYS
{
    namespace GAMESERVER
    {
        namespace NETWORK
        {
            class IOCPServer;   // 응답 SendTo 진입점 - 전방 선언
        }
    }
}

namespace KYS
{
    namespace LOGINSERVER
    {
        // 계정 생성 한 건. LoginDbThread 에서 Execute(): PBKDF2 해시 -> accounts INSERT -> 성공 시 메모리맵 등록 -> 결과 회신.
        //   PBKDF2(느림)와 DB INSERT 를 IOCP 워커가 아니라 DB 스레드에서 수행(워커 블로킹 0). 결과 Send 도 여기서(fire-and-forget 안전).
        class CreateAccountCommand : public IAccountCommand
        {
        public:
            CreateAccountCommand(UINT64 sid, const wchar_t* id, const wchar_t* pw,
                                 KYS::GAMESERVER::NETWORK::IOCPServer* server);
            void Execute() override;

        private:
            UINT64  m_sid;                       // 요청 세션 (응답 라우팅)
            wchar_t m_id[LoginReq::ID_MAX];      // 새 계정 id (고정 배열 복사 - 큐 대기 중 원본 수명 무관)
            wchar_t m_pw[PASSWORD_MAX];          // 새 계정 비번 평문 (Execute 에서 해시 후 버려짐)
            KYS::GAMESERVER::NETWORK::IOCPServer* m_server;   // 비소유 (응답 Send 진입점)
        };
    }
}
