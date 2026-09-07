#pragma once
#include "../GameServer/Network/IOCP/INetEventHandler.h"
#include "Handler/LoginHandler.h"

namespace KYS
{
    namespace GAMESERVER
    {
        namespace NETWORK
        {
            class IOCPServer;   // 전방 선언 (Init 인자 / 잘못된 패킷 종료용)
        }
    }
}

namespace KYS
{
    namespace LOGINSERVER
    {
        // 라이브러리(INetEventHandler)가 넘긴 네트워크 사건을 받는 로그인 서버측 유일 통로.
        //   채널/맵/broadcast 가 없으므로 ChannelManager 와 달리 Job 큐로 미루지 않고 워커 스레드에서 바로 처리한다(로그인=저빈도 cold path).
        //   완성 패킷의 opcode(헤더 type)를 보고 CS_LOGIN / CS_SERVER_SELECT 를 LoginHandler 로 갈래 짓는다.
        //   ServerApp의 ChannelManager(= INetEventHandler 싱글턴)와 같은 Meyers static 패턴.
        class LoginDispatcher : public KYS::GAMESERVER::NETWORK::INetEventHandler
        {
        public:
            static LoginDispatcher& GetInstance() { static LoginDispatcher instance; return instance; }   // Meyers static (ChannelManager 미러)

            LoginDispatcher(const LoginDispatcher&) = delete;            // 복사 2줄 (싱글턴 - 이동 자동 미선언)
            LoginDispatcher& operator=(const LoginDispatcher&) = delete;

            bool Init(KYS::GAMESERVER::NETWORK::IOCPServer* server);   // 서버 핸들 주입 (자기 종료용 + LoginHandler 송신용). null이면 false.

            // INetEventHandler 구현 (라이브러리가 호출)
            void EnqueueConnect(UINT64 sid) override;
            void EnqueuePacket(UINT64 sid, const BYTE* data, int len) override;
            void EnqueueDisconnect(UINT64 sid, EDisconnectReason reason) override;

        private:
            LoginDispatcher();     // 싱글턴 - GetInstance로만 생성
            ~LoginDispatcher() override;

            KYS::GAMESERVER::NETWORK::IOCPServer* m_server;   // 잘못된 패킷 보낸 세션 종료 진입점 (비소유)
            LoginHandler                          m_loginHandler;   // 실제 로그인 처리 (인스턴스 소유)
        };
    }
}
