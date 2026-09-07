#include "pch_serverapp.h"
#include "LoginLinkClient.h"
#include "../../GameCommon/Protocol/CPacket.h"
#include "../../GameCommon/Protocol/PacketType.h"
#include "../../GameCommon/Protocol/PacketHeader.h"
#include "../../GameCommon/Protocol/GamePackets.h"
#include "../../GameServer/Core/Log/Logger.h"   // 직렬화 실패(End false) 파일 로그
#include <string.h>                          // memcpy / memmove

namespace KYS
{
    namespace SERVERAPP
    {
        // 인터서버 프레임은 작다(가장 큰 IS_TOKEN_VERIFY_RES 도 ~50B) - 타입별 수신용 스택 버퍼 상한.
        static const int LINK_FRAME_BUFFER = 512;

        LoginLinkClient::LoginLinkClient()
            : m_sock(INVALID_SOCKET)
            , m_recvBuf{}
            , m_recvLen(0)
        {
        }

        LoginLinkClient::~LoginLinkClient()
        {
            Close();
        }

        // 로그인 서버 인터서버 포트로 blocking connect + recv/send 타임아웃 설정.
        bool LoginLinkClient::Connect(UINT32 ipHostOrder, USHORT port, int recvTimeoutMs)
        {
            Close();   // 재연결 안전 - 이전 소켓 정리

            m_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (m_sock == INVALID_SOCKET)
            {
                return false;
            }

            // recv/send 타임아웃(ms) - 로그인 서버 지연이 검증 스레드를 무한정 막지 않게. 둘 다 SOCKET_ERROR 로 끊는다.
            DWORD timeout = static_cast<DWORD>(recvTimeoutMs);
            ::setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            ::setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

            // raw 소켓은 라이브러리의 keepalive 설정을 상속 못 하므로 직접 켠다(half-open 감지).
            DWORD keepAlive = 1;
            ::setsockopt(m_sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepAlive), sizeof(keepAlive));

            sockaddr_in addr;
            ::ZeroMemory(&addr, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_port        = ::htons(port);
            addr.sin_addr.s_addr = ::htonl(ipHostOrder);   // 호스트 정수 -> 네트워크 바이트순서

            if (::connect(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            {
                Close();
                return false;
            }

            m_recvLen = 0;   // 새 연결 - framing 누적 초기화(이전 연결 잔여 차단)
            return true;
        }

        void LoginLinkClient::Close()
        {
            if (m_sock != INVALID_SOCKET)
            {
                ::closesocket(m_sock);
                m_sock = INVALID_SOCKET;
            }
            m_recvLen = 0;
        }

        // 한 프레임을 전부 보낸다(부분 송신 시 남은 만큼 이어보냄). 실패/타임아웃 false.
        bool LoginLinkClient::SendFrame(const BYTE* data, int size)
        {
            int sent = 0;
            while (sent < size)
            {
                const int n = ::send(m_sock, reinterpret_cast<const char*>(data + sent), size - sent, 0);
                if (n == SOCKET_ERROR || n == 0)
                {
                    return false;
                }
                sent += n;
            }
            return true;
        }

        // 완성 프레임 1개를 받는다(길이 규약, 부분 수신 누적). 실패/타임아웃/원격 종료 시 false.
        //   outFrame 에 [헤더 4B + payload] 통째를 복사하고 outLen 에 전체 길이를 채운다.
        bool LoginLinkClient::RecvFrame(BYTE* outFrame, int outCap, int& outLen)
        {
            outLen = 0;
            for (;;)
            {
                // (1) 이미 쌓인 바이트에서 완성 프레임 1개를 떼어낼 수 있는지 본다.
                if (m_recvLen >= static_cast<int>(sizeof(PacketHeader)))
                {
                    const UINT16 pktSize = ::ntohs(*reinterpret_cast<const UINT16*>(m_recvBuf));   // 앞 2B = 전체 길이(BE)
                    if (pktSize < sizeof(PacketHeader) ||
                        static_cast<int>(pktSize) > RECV_BUFFER_SIZE ||
                        static_cast<int>(pktSize) > outCap)
                    {
                        return false;   // 비정상/과대 프레임 = wire 깨짐 -> 링크 실패
                    }
                    if (m_recvLen >= static_cast<int>(pktSize))
                    {
                        ::memcpy(outFrame, m_recvBuf, static_cast<size_t>(pktSize));   // 완성 프레임 복사
                        outLen = static_cast<int>(pktSize);
                        m_recvLen -= static_cast<int>(pktSize);                          // 소비
                        if (m_recvLen > 0)
                        {
                            ::memmove(m_recvBuf, m_recvBuf + pktSize, static_cast<size_t>(m_recvLen));   // 잔여(다음 프레임 선두) 앞으로
                        }
                        return true;
                    }
                }

                // (2) 더 받아야 한다. 버퍼가 꽉 찼는데도 프레임이 안 떨어지면 비정상.
                if (m_recvLen >= RECV_BUFFER_SIZE)
                {
                    return false;
                }
                const int n = ::recv(m_sock, reinterpret_cast<char*>(m_recvBuf + m_recvLen),
                    RECV_BUFFER_SIZE - m_recvLen, 0);
                if (n == 0)             // 원격 graceful close
                {
                    return false;
                }
                if (n == SOCKET_ERROR)  // 오류 또는 타임아웃(WSAETIMEDOUT)
                {
                    return false;
                }
                m_recvLen += n;
            }
        }

        // ===== 타입별 송수신 헬퍼 =====

        bool LoginLinkClient::SendRegister(USHORT serverId, UINT32 authKeyHash, UINT32 listenIp, USHORT listenPort)
        {
            using namespace KYS::GAMECOMMON::PROTOCOL;
            CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::IS_REGISTER));
            IS_REGISTER req;
            req.serverId    = serverId;
            req.authKeyHash = authKeyHash;
            req.listenIp    = listenIp;
            req.listenPort  = listenPort;
            req.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return false; }
            return SendFrame(out.GetBuffer(), out.GetSize());
        }

        bool LoginLinkClient::RecvRegisterAck(BYTE& outResult)
        {
            BYTE frame[LINK_FRAME_BUFFER];
            int frameLen = 0;
            if (!RecvFrame(frame, LINK_FRAME_BUFFER, frameLen))
            {
                return false;
            }
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (frameLen < headerSize)
            {
                return false;
            }
            const USHORT type = ::ntohs(*reinterpret_cast<const UINT16*>(frame + sizeof(UINT16)));
            if (type != static_cast<USHORT>(PacketType::IS_REGISTER_ACK))
            {
                return false;   // 기대치 않은 프레임 - 링크 실패로 본다
            }
            if (frameLen <= headerSize)
            {
                return false;   // payload 없는 프레임 = malformed (CPacket(0) Debug abort 회피, 송수신 대칭)
            }
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(frameLen - headerSize);
            pkt.Write(frame + headerSize, frameLen - headerSize);
            IS_REGISTER_ACK ack;
            ack.Deserialize(pkt);
            outResult = ack.result;
            return true;
        }

        bool LoginLinkClient::SendVerify(UINT64 sid, UINT64 token)
        {
            using namespace KYS::GAMECOMMON::PROTOCOL;
            CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::IS_TOKEN_VERIFY_REQ));
            IS_TOKEN_VERIFY_REQ req;
            req.sid   = sid;
            req.token = token;
            req.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return false; }
            return SendFrame(out.GetBuffer(), out.GetSize());
        }

        bool LoginLinkClient::RecvVerifyOrKick(IS_TOKEN_VERIFY_RES& outVerify, UINT32& outKickAccountId, bool& outIsKick)
        {
            BYTE frame[LINK_FRAME_BUFFER];
            int frameLen = 0;
            if (!RecvFrame(frame, LINK_FRAME_BUFFER, frameLen))
            {
                return false;
            }
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (frameLen <= headerSize)
            {
                return false;   // payload 없는 프레임 = malformed (CPacket(0) Debug abort 회피)
            }
            const USHORT type = ::ntohs(*reinterpret_cast<const UINT16*>(frame + sizeof(UINT16)));
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(frameLen - headerSize);
            pkt.Write(frame + headerSize, frameLen - headerSize);

            if (type == static_cast<USHORT>(PacketType::IS_KICK))
            {
                IS_KICK kick;
                kick.Deserialize(pkt);
                outKickAccountId = kick.accountId;
                outIsKick = true;
                return true;
            }
            if (type == static_cast<USHORT>(PacketType::IS_TOKEN_VERIFY_RES))
            {
                outVerify.Deserialize(pkt);
                outIsKick = false;
                return true;
            }
            return false;   // 기대치 않은 opcode - 링크 teardown
        }

        bool LoginLinkClient::SendSessionGone(UINT32 accountId, UINT64 sid)
        {
            using namespace KYS::GAMECOMMON::PROTOCOL;
            CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::IS_PLAYER_LEAVE));
            IS_PLAYER_LEAVE req;
            req.accountId = accountId;
            req.sid       = sid;
            req.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return false; }
            return SendFrame(out.GetBuffer(), out.GetSize());
        }

        bool LoginLinkClient::SendOnlineSnapshot(const IS_ONLINE_SYNC& chunk)
        {
            using namespace KYS::GAMECOMMON::PROTOCOL;
            CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::IS_ONLINE_SYNC));
            chunk.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return false; }
            return SendFrame(out.GetBuffer(), out.GetSize());
        }
    }
}
