#include "pch_loginserver.h"
#include "LoginDispatcher.h"
#include "../GameServer/Network/IOCP/IOCPServer.h"   // DisconnectTo
#include "../GameServer/Network/IOCP/Session.h"
#include "../GameCommon/Protocol/PacketHeader.h"   // 헤더 크기 / type 오프셋
#include "../GameCommon/Protocol/PacketType.h"     // opcode 분기 (CS_LOGIN / CS_SERVER_SELECT)

namespace KYS
{
    namespace LOGINSERVER
    {
        LoginDispatcher::LoginDispatcher()
            : m_server(nullptr)
        {
        }

        LoginDispatcher::~LoginDispatcher()
        {
            // m_server 비소유, m_loginHandler 는 멤버 소멸로 정리.
        }

        // 서버 핸들을 받아 자기 보관 + LoginHandler에도 전달한다 (null이면 false).
        bool LoginDispatcher::Init(KYS::GAMESERVER::NETWORK::IOCPServer* server)
        {
            if (server == nullptr)
            {
                return false;   // 송신/종료 진입점이 없으면 응답 불가 - 부팅 중단
            }
            m_server = server;
            m_loginHandler.Init(server);
            return true;
        }

        // 새 연결 통지 - 로그인 서버는 연결 시점에 잡아둘 상태가 없다(토큰은 CS_LOGIN 성공 후 발급).
        void LoginDispatcher::EnqueueConnect(UINT64 sid)
        {
            (void)sid;   // /W4 미사용 인자 경고 억제 - 첫 CS_LOGIN 패킷에서 비로소 상태가 생김
        }

        // 완성 패킷 통지 - opcode를 보고 로그인 / 서버선택으로 갈래 짓는다.
        //   data : 헤더 4B + payload (길이 규약으로 라이브러리가 경계를 이미 잘라 줌)
        //   len  : 패킷 전체 바이트
        void LoginDispatcher::EnqueuePacket(UINT64 sid, const BYTE* data, int len)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (len <= headerSize)
            {
                // 헤더만 있거나 payload 가 없는(len<=4) 패킷 - CS_LOGIN/CS_SERVER_SELECT 는 payload>=1B 라 규약 위반(변조 가능).
                //   payload 0 통과 시 Handle* 가 CPacket(0) 을 만들어 SerializationBuffer 의 capacity>0 가드를 Debug 에서 건드림 - 신뢰 경계서 막는다(InterServerDispatcher 와 대칭).
                m_server->DisconnectTo(sid, EDisconnectReason::PROTOCOL_VIOLATION);
                return;
            }

            // 헤더 type 필드(2번째 2B, BE)를 peek해 opcode 판별. PacketHeader{size 2B, type 2B}.
            const PacketType opcode =
                static_cast<PacketType>(::ntohs(*reinterpret_cast<const UINT16*>(data + sizeof(UINT16))));

            switch (opcode)
            {
            case PacketType::CS_LOGIN:
                m_loginHandler.HandleLogin(sid, data, len);
                break;
            case PacketType::CS_REGISTER:
                m_loginHandler.HandleRegister(sid, data, len);
                break;
            case PacketType::CS_SERVER_SELECT:
                m_loginHandler.HandleServerSelect(sid, data, len);
                break;
            default:
            {
                // 로그인 서버가 처리할 수 없는 opcode - 잘못된/적대적 클라로 보고 종료.
                m_server->DisconnectTo(sid, EDisconnectReason::PROTOCOL_VIOLATION);
                break;
            }
            }
        }

        // 연결 종료 통지 - LoginHandler가 세션->토큰 항목을 정리하게 한다.
        void LoginDispatcher::EnqueueDisconnect(UINT64 sid, EDisconnectReason reason)
        {
            (void)reason;   // 사유는 로그인 정리에 무관 (/W4 경고 억제)
            m_loginHandler.OnClientGone(sid);
        }
    }
}
