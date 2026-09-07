#pragma once
#include "../GameServer/Network/IOCP/IOCPServer.h"

namespace KYS
{
    namespace LOGINSERVER
    {
        // 로그인 서버의 accept 엔진 - 라이브러리 IOCPServer를 상속해 두 콜백(연결 수락 여부 / 오류 보고)만 채운다.
        //   accept/sid 발급/framing/Send는 전부 base IOCPServer가 처리하고, 게임 로직은 LoginDispatcher가 받는다.
        //   ServerApp의 GameServer(= IOCPServer 싱글턴)와 같은 Meyers static 패턴.
        class LoginIocpServer : public KYS::GAMESERVER::NETWORK::IOCPServer
        {
        public:
            static LoginIocpServer& GetInstance() { static LoginIocpServer instance; return instance; }   // Meyers static (GameServer 미러)

            LoginIocpServer(const LoginIocpServer&) = delete;            // 복사 2줄 (싱글턴 - 이동 자동 미선언)
            LoginIocpServer& operator=(const LoginIocpServer&) = delete;

            // 라이브러리 콜백 구현 (IOCPServer 순수가상 override)
            bool OnConnectionRequest(const sockaddr_in& addr) override;   // accept 직전 - 이 연결을 받을지 결정 (지금은 무조건 수락)
            void OnError(ErrorCode code, const wchar_t* msg) override;    // 라이브러리가 알린 오류를 로그로 남김

        private:
            LoginIocpServer();              // 싱글턴 - GetInstance로만 생성
            ~LoginIocpServer() override;
        };
    }
}
