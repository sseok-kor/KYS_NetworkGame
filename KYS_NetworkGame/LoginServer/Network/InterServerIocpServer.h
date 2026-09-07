#pragma once
#include "../GameServer/Network/IOCP/IOCPServer.h"

namespace KYS
{
    namespace LOGINSERVER
    {
        // 인터서버(게임 서버 전용) accept 엔진 - 클라 대면 LoginIocpServer 와 별개로 2번째 포트를 듣는다.
        //   신뢰 경계를 포트로 물리화한다 - 외부 클라는 이 포트(loopback/LAN 바인딩+방화벽)에
        //   닿지 못해 IS_* 핸들러에 접근할 수 없다. 클라 포트(LoginIocpServer)와 코드는 같고 들어오는 트래픽만 다르다.
        //   accept/sid 발급/framing/Send 는 base IOCPServer 가 처리하고, 게임 서버의 IS_* 질의는 InterServerDispatcher 가 받는다.
        //   IOCPServer 가 인스턴스당 listen 소켓 1개라 클라용과 분리된 별도 싱글턴이 필요하다(같은 클래스 2 인스턴스 불가).
        class InterServerIocpServer : public KYS::GAMESERVER::NETWORK::IOCPServer
        {
        public:
            static InterServerIocpServer& GetInstance() { static InterServerIocpServer instance; return instance; }   // Meyers static (LoginIocpServer 미러)

            InterServerIocpServer(const InterServerIocpServer&) = delete;            // 복사 2줄 (싱글턴 - 이동 자동 미선언)
            InterServerIocpServer& operator=(const InterServerIocpServer&) = delete;

            // 라이브러리 콜백 구현 (IOCPServer 순수가상 override)
            bool OnConnectionRequest(const sockaddr_in& addr) override;   // accept 직전 - 이 연결을 받을지 결정 (loopback 만 수락하고 나머지는 Session 배정 전 거부, 링크 인증은 IS_REGISTER 가 담당)
            void OnError(ErrorCode code, const wchar_t* msg) override;    // 라이브러리가 알린 오류를 로그로 남김

        private:
            InterServerIocpServer();              // 싱글턴 - GetInstance 로만 생성
            ~InterServerIocpServer() override;
        };
    }
}
