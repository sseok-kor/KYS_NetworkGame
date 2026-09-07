#include "pch_loginserver.h"
#include "InterServerDispatcher.h"
#include "Auth/TokenStore.h" // 토큰 1회 소비 + 중복 로그인 online 표
#include "../GameServer/Network/IOCP/IOCPServer.h"   // SendTo / DisconnectTo
#include "../GameServer/Network/IOCP/Session.h"      // Session.Send / Session.Disconnect
#include "../GameCommon/Protocol/CPacket.h"
#include "../GameCommon/Protocol/PacketHeader.h"
#include "../GameCommon/Protocol/PacketType.h"
#include "../GameCommon/Protocol/GamePackets.h"
#include "../GameCommon/GameDefines.h"   // 게임 공용 상수 (WHISPER_NAME_MAX 등)
#include "../GameServer/Core/Log/Logger.h"   // authKey 불일치 등 보안 이벤트 파일 로그

namespace KYS
{
    namespace LOGINSERVER
    {
        using KYS::GAMESERVER::LOG::LogLevel;

        // 파일-로컬 로그 헬퍼 - 인터서버 이벤트를 server 채널에 남긴다.
        template<typename... Args>
        static void ServerLog(LogLevel level, const wchar_t* tag, const wchar_t* format, Args&&... args)
        {
            KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                KYS::GAMESERVER::LOG::LogChannel::SERVER, level, tag, format, std::forward<Args>(args)...);
        }
        volatile LONG InterServerDispatcher::s_verifyReq = 0;
        volatile LONG InterServerDispatcher::s_verifyOk  = 0;
        volatile LONG InterServerDispatcher::s_kick      = 0;
        volatile LONG InterServerDispatcher::s_leave     = 0;
        volatile LONG InterServerDispatcher::s_onlineSync = 0;
        volatile LONG InterServerDispatcher::s_sendDrop   = 0;

        InterServerDispatcher::InterServerDispatcher()
            : m_server(nullptr)
            , m_authSecret(0)   // SetAuthSecret 전 기본 0 (부팅 시 conf/Server_Config.ini 의 [interserver] secret 에서 non-zero 주입)
        {
        }

        InterServerDispatcher::~InterServerDispatcher()
        {
            // m_server 비소유 - 정리할 멤버 없음.
        }

        // 인터서버 서버 핸들을 받아둔다 (응답 Send 진입점). null 이면 부팅 중단.
        bool InterServerDispatcher::Init(KYS::GAMESERVER::NETWORK::IOCPServer* server)
        {
            if (server == nullptr)
            {
                return false;   // 응답을 보낼 진입점이 없으면 동작 불가
            }
            m_server = server;
            return true;
        }

        // 새 인터서버 연결 통지 - 등록(IS_REGISTER) 전엔 잡아둘 상태가 없다.
        void InterServerDispatcher::EnqueueConnect(UINT64 linkSid)
        {
            (void)linkSid;   // /W4 - 첫 IS_REGISTER 패킷에서 비로소 검사가 시작됨
        }

        // 완성 패킷 통지 - opcode 를 보고 IS_* 갈래를 정한다.
        //   data : 헤더 4B + payload (길이 규약으로 라이브러리가 경계를 이미 잘라 줌)
        //   len  : 패킷 전체 바이트
        void InterServerDispatcher::EnqueuePacket(UINT64 linkSid, const BYTE* data, int len)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (len <= headerSize)
            {
                // 헤더만 있고 payload 가 없는(len<=4) 패킷 - 모든 IS_* 는 payload 가 1B 이상 있으므로 규약 위반(변조 가능).
                //   payload 0 을 통과시키면 아래 Handle* 가 CPacket(0) 을 만들어 SerializationBuffer 의 capacity>0 가드를
                //   Debug 에서 건드린다(Read 경로처럼 신뢰 경계 입력을 여기서 막는다) - 링크 종료.
                m_server->DisconnectTo(linkSid, EDisconnectReason::PROTOCOL_VIOLATION);
                return;
            }

            // 헤더 type 필드(2번째 2B, BE)를 peek 해 opcode 판별. PacketHeader{size 2B, type 2B}.
            const PacketType opcode =
                static_cast<PacketType>(::ntohs(*reinterpret_cast<const UINT16*>(data + sizeof(UINT16))));

            switch (opcode)
            {
            case PacketType::IS_REGISTER:
                HandleRegister(linkSid, data, len);
                break;
            case PacketType::IS_TOKEN_VERIFY_REQ:
                HandleTokenVerify(linkSid, data, len);
                break;
            case PacketType::IS_PLAYER_LEAVE:
                HandlePlayerLeave(linkSid, data, len);
                break;
            case PacketType::IS_ONLINE_SYNC:
                HandleOnlineSync(linkSid, data, len);
                break;
            default:
            {
                // 인터서버 포트가 처리할 수 없는 opcode - 잘못된/적대적 접속으로 보고 종료.
                m_server->DisconnectTo(linkSid, EDisconnectReason::PROTOCOL_VIOLATION);
                break;
            }
            }
        }

        // 인터서버 링크 종료 통지. 이 링크가 올린 online 계정들은 더 이상 IS_ONLINE_SYNC refresh 를 못 받아
        //   TokenSweepThread 의 SweepOnline 이 grace(ONLINE_GRACE_MS) 후 일괄 정리한다 - 별도 즉시 clear 불요
        //   (단일 backstop, transient blip 에 dup-login 보호 오clear 회피, 재연결 시 새 스냅샷이 산 계정 복원).
        void InterServerDispatcher::EnqueueDisconnect(UINT64 linkSid, EDisconnectReason reason)
        {
            (void)linkSid;
            (void)reason;
        }

        // IS_REGISTER 처리 - 공유 비밀(authKeyHash)을 검사해 가짜 게임 서버 등록을 막고 결과를 회신한다.
        //   listenIp/listenPort 는 SC_LOGIN_TOKEN redirect 에 쓸 게임 서버 주소이나, 현재 LoginHandler 가 주소를
        //   conf/Server_Config.ini 의 [server] public_ip/public_port 로 부팅 때 주입받아 쓰므로 아직 소비하지 않는다
        //   (추후 등록 주소를 redirect 에 연결 예정).
        void InterServerDispatcher::HandleRegister(UINT64 linkSid, const BYTE* data, int len)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(len - headerSize);
            pkt.Write(data + headerSize, len - headerSize);   // payload 만 적재

            IS_REGISTER req;
            req.Deserialize(pkt);   // serverId / authKeyHash / listenIp / listenPort

            const bool ok = (req.authKeyHash == m_authSecret);

            IS_REGISTER_ACK ack;
            ack.result = ok ? static_cast<BYTE>(1) : static_cast<BYTE>(0);

            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::IS_REGISTER_ACK));
            ack.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            SendOnLink(linkSid, out.GetBuffer(), out.GetSize());

            if (!ok)
            {
                // 공유 비밀 불일치 = 신뢰할 수 없는 접속. 두 프로세스는 같은 폴더의 같은 conf/Server_Config.ini 를 읽으므로
                //   정상 배치라면 값이 어긋날 수 없다 - 그래서 이 경고는 "설정 파일을 하나 더 찾아라"가 아니라 배치/기동 사고를 가리킨다.
                //   큰 소리로 로그해 진단 가능하게 한다 - 비밀값 자체는 절대 찍지 않는다.
                ServerLog(LogLevel::LL_WARN, L"inter", L"IS_REGISTER authKey 불일치 - 접속 거부. (1) Debug 와 Release 를 교차 기동했는지 (2) [interserver] secret 을 고친 뒤 한쪽만 재기동했는지 (3) 두 머신에 나눠 배포했다면 양쪽 값이 같은지 확인");
                m_server->DisconnectTo(linkSid, EDisconnectReason::PROTOCOL_VIOLATION);
            }
        }

        // IS_TOKEN_VERIFY_REQ 처리 - 토큰을 1회 소비하고 계정 식별을 응답에 실어 보낸다.
        //   요청 안의 sid(게임 세션 sid)를 응답에 그대로 되실어 ServerApp 이 어느 대기 세션의 결과인지 상관하게 한다.
        void InterServerDispatcher::HandleTokenVerify(UINT64 linkSid, const BYTE* data, int len)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(len - headerSize);
            pkt.Write(data + headerSize, len - headerSize);

            IS_TOKEN_VERIFY_REQ req;
            req.Deserialize(pkt);   // sid(게임 세션) + token

            IS_TOKEN_VERIFY_RES res;
            res.sid       = req.sid;   // 상관 키 그대로 반사
            res.accountId = 0;
            res.loginName[0] = L'\0';

            ::InterlockedIncrement(&s_verifyReq);

            TokenStore::TokenInfo info;
            if (!TokenStore::GetInstance().Consume(req.token, info))
            {
                // 토큰 없음(위조/이미 소비) 또는 만료 - Consume 이 둘을 false 로 합쳐 돌려주므로 NOT_FOUND 로 보고.
                res.result = static_cast<BYTE>(EVerifyResult::NOT_FOUND);
            }
            else
            {
                // 검증 성공 - online 표에 신규 sid 등록. 이미 접속 중이면(중복 로그인) 기존 세션을 IS_KICK 으로 축출하고 신규 수용(kick 정책).
                //   online[accountId]=req.sid 로 갈아끼움(신규가 현 holder) - 이후 그 sid 의 leave 만 해제를 반영.
                const bool wasOnline = TokenStore::GetInstance().PutOnline(info.accountId, req.sid);
                if (wasOnline)
                {
                    ::InterlockedIncrement(&s_kick);
                    IS_KICK kick;
                    kick.accountId = info.accountId;
                    KYS::GAMECOMMON::PROTOCOL::CPacket kickPkt(MAX_PACKET_SIZE);
                    kickPkt.Begin(static_cast<USHORT>(PacketType::IS_KICK));
                    kick.Serialize(kickPkt);
                    const bool kickBuilt = kickPkt.End(); if (!kickBuilt) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", kickPkt.GetType(), kickPkt.GetSize()); }
                    // RES 보다 먼저 보낸다 - 같은 링크 + TCP in-order 라 ServerApp 이 IS_KICK(기존 축출)을 RES(신규 수용)보다 먼저 소화.
                    if (kickBuilt) { SendOnLink(linkSid, kickPkt.GetBuffer(), kickPkt.GetSize()); }
                }
                res.result    = static_cast<BYTE>(EVerifyResult::OK);
                res.accountId = info.accountId;
                wcscpy_s(res.loginName, WHISPER_NAME_MAX, info.loginName);
                ::InterlockedIncrement(&s_verifyOk);
            }

            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::IS_TOKEN_VERIFY_RES));
            res.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            SendOnLink(linkSid, out.GetBuffer(), out.GetSize());
        }

        // IS_PLAYER_LEAVE 처리 - online 표에서 계정을 비운다(같은 계정 재로그인 가능). 응답 없음(fire-and-forget).
        void InterServerDispatcher::HandlePlayerLeave(UINT64 linkSid, const BYTE* data, int len)
        {
            (void)linkSid;   // leave 는 회신이 없으므로 링크 sid 불요

            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(len - headerSize);
            pkt.Write(data + headerSize, len - headerSize);

            IS_PLAYER_LEAVE req;
            req.Deserialize(pkt);   // accountId + sid

            ::InterlockedIncrement(&s_leave);
            // 현 holder(online[accountId]==sid)일 때만 해제 - 같은 계정이 재접속해 sid 갈아탄 뒤 늦게 도는 leave 는 무시(ghost 봉인).
            TokenStore::GetInstance().RemoveOnline(req.accountId, req.sid);
        }

        // IS_ONLINE_SYNC 처리 - 게임 권위 online 집합 청크를 받아 각 계정의 lastSyncMs 를 갱신(refresh). 응답 없음(fire-and-forget).
        //   여기서 갱신 못 받은 online 엔트리(ghost - RES 유실, LEAVE 드롭, 링크끊김, shutdown)는 TokenSweepThread 의 SweepOnline 이 grace 후 정리한다.
        void InterServerDispatcher::HandleOnlineSync(UINT64 linkSid, const BYTE* data, int len)
        {
            (void)linkSid;   // 스냅샷은 회신이 없고 sweep 이 link-agnostic 이라 링크 sid 불요

            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(len - headerSize);
            pkt.Write(data + headerSize, len - headerSize);

            IS_ONLINE_SYNC req;
            req.Deserialize(pkt);   // count + [accountId, sid]... (Deserialize 가 count 를 MAX_ENTRIES 로 클램프)

            ::InterlockedIncrement(&s_onlineSync);
            const UINT64 nowMs = ::GetTickCount64();
            for (USHORT i = 0; i < req.count; ++i)
            {
                // accountId 0 = 인증 실패 sentinel(이 리포 규약). 절단된 스냅샷에서 읽기 가드가 채운 0 이
                //   여기 들어오면 RefreshOnline 이 m_online[0] 에 유령 엔트리를 만든다(upsert 라 없으면 생성).
                //   그 유령은 sweep grace 까지 남아 계정 0 을 "접속 중" 으로 보이게 한다.
                if (req.accountIds[i] == 0) { continue; }
                TokenStore::GetInstance().RefreshOnline(req.accountIds[i], req.sids[i], nowMs);
            }
        }

        // linkSid 의 소켓으로 한 패킷을 보낸다 (Fire-and-Forget). 끊긴 링크/백프레셔 drop 은 s_sendDrop 로 관측화(silent 유실 방지).
        void InterServerDispatcher::SendOnLink(UINT64 linkSid, const BYTE* data, int size)
        {
            if (!m_server->SendTo(linkSid, data, size, ESendDropPolicy::KEEP_CONNECTION)) { ::InterlockedIncrement(&s_sendDrop); }   // 끊긴 링크/백프레셔 drop 관측 - 링크는 유지(KEEP)
        }
    }
}
