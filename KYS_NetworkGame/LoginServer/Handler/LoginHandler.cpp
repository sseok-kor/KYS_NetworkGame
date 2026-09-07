#include "pch_loginserver.h"
#include "LoginHandler.h"
#include "Auth/AccountManager.h"   // 계정 검증 (id/비번 대조) - ServerApp에서 이전된 LoginServer 전용 소유
#include "Auth/TokenIssuer.h"      // 고엔트로피 UINT64 토큰 발급 (BCryptGenRandom)
#include "Auth/TokenStore.h"       // 발급 토큰 적재 (token -> loginName, TTL) - 다음 단계 인터서버 검증이 소비
#include "DataBase/LoginDbThread.h"        // 계정 생성 INSERT 위임 (PBKDF2 해시 + INSERT 를 워커 밖에서)
#include "DataBase/CreateAccountCommand.h" // CS_REGISTER 처리 명령
#include "../GameServer/Network/IOCP/IOCPServer.h"   // SendTo / DisconnectTo / GetSessionRemoteIp
#include "../GameServer/Network/IOCP/Session.h"      // Session.Send / Session.Disconnect
#include "../GameCommon/Protocol/CPacket.h"
#include "../GameCommon/Protocol/PacketHeader.h"
#include "../GameCommon/Protocol/PacketType.h"
#include "../GameCommon/Protocol/GamePackets.h"
#include "../GameServer/Core/Log/Logger.h"   // 로그인 실패 이력 파일 로그 (보안 이벤트 - IP 포함)
#include <cwchar>   // wcslen (계정 생성 입력 검증)

// per-connection 시도 상한 (PBKDF2 증폭 / DB 큐 적체 방어 - 계정 잠금 없이 소켓 단위로만). 정상 baseline 대비 여유 있는 시작값, 측정 후 조정.
static const UINT32 MAX_LOGIN_ATTEMPTS_PER_CONN = 10;      // 한 연결의 CS_LOGIN 시도 상한 - 초과 시 PBKDF2(Verify) 스킵. 정상 유저는 몇 회면 성공, 도배는 즉시 초과
static const UINT32 MAX_REGISTER_ATTEMPTS_PER_CONN = 5;    // 한 연결의 CS_REGISTER 시도 상한 - 초과 시 DB 위임 스킵(무제한 DB 큐 적체 차단)

namespace
{
    // 계정 생성 입력 검증: id/pw 길이 + 빈 값 + 제어문자. 통과해야 DB 로 보낸다(저질 입력을 DB 스레드 앞에서 거름).
    bool IsValidCredential(const wchar_t* id, const wchar_t* pw)
    {
        if (id == nullptr || pw == nullptr) { return false; }
        const size_t idLen = wcslen(id);
        const size_t pwLen = wcslen(pw);
        if (idLen < 1 || idLen >= LoginReq::ID_MAX) { return false; }   // 1..15 (널 자리 확보)
        if (pwLen < 1 || pwLen >= PASSWORD_MAX)     { return false; }   // 1..31
        for (size_t i = 0; i < idLen; ++i) { if (id[i] < L' ' || id[i] == L',') { return false; } }   // 제어문자/콤마 거부
        for (size_t i = 0; i < pwLen; ++i) { if (pw[i] < L' ') { return false; } }                    // 제어문자 거부
        return true;
    }
}

namespace KYS
{
    namespace LOGINSERVER
    {
        // 단일 게임 서버 가정 - id 는 상수(단일 서버), 주소(ip/port)는 conf/Server_Config.ini 의 [server] public_ip/public_port 로 주입(SetGameServerAddress).
        //   serverIp/ip 는 패킷 규약상 호스트 정수(UINT32). 127.0.0.1 = 0x7F000001.
        static const BYTE GAME_SERVER_ID = 0;

        volatile LONG LoginHandler::s_attemptBlockCount = 0;
        UINT32 LoginHandler::s_gameServerIp   = 0;   // 부팅이 반드시 주입 - 기본값 없음(0 이면 서버 오설정)
        USHORT LoginHandler::s_gameServerPort = 0;   // 부팅이 반드시 주입 - 기본값 없음(0 이면 서버 오설정)

        // 부팅 1회 - main 이 conf/Server_Config.ini 값을 워커 스레드 시작(Start) 전에 주입한다(happens-before -> 이후 read-only, 락 불요).
        void LoginHandler::SetGameServerAddress(UINT32 ip, USHORT port)
        {
            s_gameServerIp   = ip;
            s_gameServerPort = port;
        }

        LONG LoginHandler::GetAttemptBlockCount()
        {
            return s_attemptBlockCount;   // 워커 Interlocked 증가 / main 무락 read (관측 근사)
        }

        LoginHandler::LoginHandler()
            : m_server(nullptr)
        {
        }

        LoginHandler::~LoginHandler()
        {
            // m_server 는 비소유. m_sessionTokens 는 컨테이너 소멸로 정리.
        }

        // 송신/종료에 쓸 서버 핸들을 받아둔다 (디스패처 Init이 전달).
        void LoginHandler::Init(KYS::GAMESERVER::NETWORK::IOCPServer* server)
        {
            m_server = server;
        }

        // CS_LOGIN 처리 - 계정 검증 -> 토큰 발급/적재 -> 게임 서버 목록 회신.
        //   sid  : 요청 세션
        //   data : 헤더 4B + payload (LoginReq)
        //   size : 전체 바이트
        void LoginHandler::HandleLogin(UINT64 sid, const BYTE* data, int size)
        {
            // 헤더(size 2B + type 2B = 4B)는 디스패처가 opcode 판별에 이미 썼다. payload만 떼어 역직렬화한다.
            const int headerSize = static_cast<int>(sizeof(PacketHeader));   // = 4
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size - headerSize);
            pkt.Write(data + headerSize, size - headerSize);   // payload만 적재 (읽기 위치 0부터)

            LoginReq req;
            req.Deserialize(pkt);   // id 문자열 + 비번 평문 복원

            // per-connection 로그인 시도 상한 - 초과 시 PBKDF2(Verify)를 건너뛰어 증폭을 막는다. 계정 잠금 없음(lockout DoS 회피, 증폭 방어는 per-connection + per-IP가 담당)
            if (!AllowLoginAttempt(sid))
            {
                SendLoginFail(sid);   // 실패와 구분 불가(cap 노출 안 함)
                return;
            }

            // 계정 검증 (accounts 메모리맵 조회 + PBKDF2 비교). 성공 = accountId(1+), 실패 = 0.
            const UINT32 accountId = AccountManager::GetInstance().Verify(req.id, req.pw);
            if (accountId == 0)
            {
                // 실패 이력 - 보안 이벤트라 계정명 + 접속 IP 를 남긴다 (brute-force 사후 분석의 1차 자료).
                UINT32 ip = 0;
                {
                    ip = m_server->GetSessionRemoteIp(sid);   // 값으로 읽는다 - 풀 락 안(Reset 과 상호배제). 없으면 0
                }
                const BYTE* ipb = reinterpret_cast<const BYTE*>(&ip);
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_WARN, L"login",
                    L"로그인 실패 (계정 검증) id=%s ip=%u.%u.%u.%u",
                    req.id, static_cast<unsigned>(ipb[0]), static_cast<unsigned>(ipb[1]),
                    static_cast<unsigned>(ipb[2]), static_cast<unsigned>(ipb[3]));
                SendLoginFail(sid);   // 거부 응답 (연결은 클라가 닫음)
                return;
            }

            // 일회용 토큰 발급 (CSPRNG 실패면 0 -> 발급 실패로 거부).
            const UINT64 token = TokenIssuer::GetInstance().Make();
            if (token == 0)
            {
                // 보안 이벤트가 아니라 서버 결함(CSPRNG 실패) - ERROR 로 분리.
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"login",
                    L"토큰 발급 실패 (CSPRNG) - 로그인 거부");
                SendLoginFail(sid);
                return;
            }

            // 토큰 적재 (token -> accountId/loginName/TTL). 다음 단계 인터서버 검증이 한 번 꺼내 소비한다.
            //   loginName = req.id (인터서버 응답에 동봉하는 표시용 - 캐릭터 로드 키는 charId+accountId 다). TTL = LOGIN_TOKEN_TTL_MS(60s).
            TokenStore::GetInstance().Put(token, accountId, req.id, LOGIN_TOKEN_TTL_MS);

            // 이 세션이 CS_SERVER_SELECT 때 돌려줄 토큰을 기억해 둔다 (sid -> token).
            {
                KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_tokenLock);
                m_sessionTokens[sid] = token;   // 같은 세션 재로그인 시 최신 토큰으로 덮어씀
            }

            // 게임 서버 목록 회신 (현재 = 서버 1개).
            SC_SERVER_LIST list{};   // 안 채우는 servers[1..] 이 스택 잔재를 wire 로 흘리지 않게 0으로
            list.count = 1;
            list.servers[0].serverId   = GAME_SERVER_ID;
            list.servers[0].ip         = s_gameServerIp;
            list.servers[0].port       = s_gameServerPort;
            list.servers[0].population = 0;   // 혼잡도는 인터서버 보고(다음 단계) 전엔 0
            wcscpy_s(list.servers[0].name, WHISPER_NAME_MAX, L"GameServer01");

            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_SERVER_LIST));
            list.Serialize(out);
            // 쓰는 동안 버퍼가 넘쳤으면 내용이 잘려 있다 - 길이 자리가 0 인 채라 보내면 받는 쪽이 끊으므로 보내지 않는다.
            //   여기서 걸리면 서버 쪽 버그다(목록이 버퍼보다 커졌다). 클라가 할 수 있는 일은 없으니 로그만 남긴다 (80곳 공통 규약 - CPacket::End 선언 주석).
            if (!out.End())
            {
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet",
                    L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize());
                return;
            }
            SendToClient(sid, out.GetBuffer(), out.GetSize());
        }

        // CS_REGISTER 처리 - 입력 검증 후 계정 생성을 DB 스레드에 위임.
        //   PBKDF2 해시(~수백 ms)와 INSERT 는 IOCP 워커를 막으면 안 되므로 LoginDbThread 가 수행(결과 회신도 그쪽).
        //   검증 실패만 여기서 즉시 INVALID 회신(DB 까지 안 감).
        void LoginHandler::HandleRegister(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size - headerSize);
            pkt.Write(data + headerSize, size - headerSize);

            CS_REGISTER req;
            req.Deserialize(pkt);   // id + 비번 평문 복원

            // per-connection 가입 시도 상한 - 이 연결 하나가 DB 위임(PBKDF2 해시 + INSERT)을 무한정 밀어넣는 것을 막는다.
            //   (연결을 갈아끼우며 여러 sid로 집계 범람하는 것은 per-IP 연결 레이트로 별도 방어 - 뒤 배치)
            if (!AllowRegisterAttempt(sid))
            {
                SendRegisterResult(sid, static_cast<BYTE>(ECreateAccountResult::INVALID));
                return;
            }

            if (!IsValidCredential(req.id, req.pw))
            {
                SendRegisterResult(sid, static_cast<BYTE>(ECreateAccountResult::INVALID));
                return;
            }

            // 해시 + INSERT + 결과 Send 는 DB 스레드. 명령 동적할당 -> LoginDbThread 가 처리 후 delete(OnRecvJob 정합).
            CreateAccountCommand* cmd = new CreateAccountCommand(sid, req.id, req.pw, m_server);
            LoginDbThread::GetInstance().Enqueue(cmd);
        }

        // CS_SERVER_SELECT 처리 - 그 세션에 발급해 둔 토큰 + 게임 서버 주소를 내려준다.
        //   sid  : 요청 세션
        //   data : 헤더 4B + payload (CS_SERVER_SELECT)
        //   size : 전체 바이트
        void LoginHandler::HandleServerSelect(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size - headerSize);
            pkt.Write(data + headerSize, size - headerSize);

            CS_SERVER_SELECT sel;
            sel.Deserialize(pkt);   // serverId 파싱 (현재 단일 서버라 값은 무시)
            (void)sel.serverId;     // /W4 - 파싱했으나 단일 서버라 미사용

            // 이 세션에 발급된 토큰을 찾는다 (CS_LOGIN을 먼저 통과해야 존재).
            UINT64 token = 0;
            {
                KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_tokenLock);
                std::unordered_map<UINT64, UINT64>::const_iterator it = m_sessionTokens.find(sid);
                if (it != m_sessionTokens.end())
                {
                    token = it->second;
                }
            }

            if (token == 0)
            {
                // 로그인 전에 서버 선택을 보낸 잘못된 순서 - 연결 종료.
                Kick(sid, EDisconnectReason::PROTOCOL_VIOLATION);
                return;
            }

            // 토큰 + 재접속할 게임 서버 주소를 내려준다. 클라는 이걸 받고 로그인 서버를 끊고 게임 서버로 재접속한다.
            SC_LOGIN_TOKEN tok;
            tok.token      = token;
            tok.serverIp   = s_gameServerIp;
            tok.serverPort = s_gameServerPort;

            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_LOGIN_TOKEN));
            tok.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            SendToClient(sid, out.GetBuffer(), out.GetSize());
        }

        // 연결 종료 - 세션->토큰 항목과 시도 카운터를 지운다 (회수 단일점, 중복 종료도 안전).
        void LoginHandler::OnClientGone(UINT64 sid)
        {
            {
                KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_tokenLock);
                m_sessionTokens.erase(sid);   // 발급 안 한 세션이면 no-op
            }
            {
                KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_rateLock);
                m_connAttempts.erase(sid);    // 이 연결의 시도 카운터 정리 (떠도는 항목 방지)
            }
        }

        // 이 연결의 CS_LOGIN 시도를 +1 하고 상한 이하면 true - 초과면 Verify(PBKDF2)를 건너뛰게 해 증폭을 막는다.
        //   상한 도달 후엔 더 세지 않는다(포화) - 무한 증가로 UINT32 wrap(0xFFFFFFFF->0)되어 재허용되는 경로 차단.
        bool LoginHandler::AllowLoginAttempt(UINT64 sid)
        {
            KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_rateLock);
            UINT32& count = m_connAttempts[sid].loginCount;   // 없으면 0으로 생성
            if (count >= MAX_LOGIN_ATTEMPTS_PER_CONN)
            {
                ::InterlockedIncrement(&s_attemptBlockCount);   // flood 방어 관측 (모니터 표출 - 이벤트 로그는 폭주 방지 위해 생략)
                return false;   // 이미 상한 - 증가 없이 거부(포화, wrap 방지)
            }
            ++count;
            return true;
        }

        // 이 연결의 CS_REGISTER 시도를 +1 하고 상한 이하면 true - 초과면 DB 위임을 건너뛴다. (포화: 상한 후 증가 안 함)
        bool LoginHandler::AllowRegisterAttempt(UINT64 sid)
        {
            KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_rateLock);
            UINT32& count = m_connAttempts[sid].registerCount;
            if (count >= MAX_REGISTER_ATTEMPTS_PER_CONN)
            {
                ::InterlockedIncrement(&s_attemptBlockCount);   // flood 방어 관측 (login 시도와 합산 1카운터)
                return false;   // 이미 상한 - 증가 없이 거부(포화, wrap 방지)
            }
            ++count;
            return true;
        }

        // sid의 소켓으로 한 패킷을 보낸다 (Fire-and-Forget). 이미 끊긴 세션이면 no-op.
        void LoginHandler::SendToClient(UINT64 sid, const BYTE* data, int size)
        {
            m_server->SendTo(sid, data, size, ESendDropPolicy::KEEP_CONNECTION);   // 찾기/세대 검증/lease/송신/반납은 풀 안에서. 로그인 응답은 넘쳐도 안 끊는다
        }

        // 인증 실패 응답을 보낸다 (SC_LOGIN_RESULT{FAIL}). 연결 종료는 클라가 판단.
        void LoginHandler::SendLoginFail(UINT64 sid)
        {
            SC_LOGIN_RESULT res;
            res.result = static_cast<BYTE>(ELoginResult::FAIL);
            res.chId   = -1;   // 로그인 서버는 채널을 배정하지 않음

            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_LOGIN_RESULT));
            res.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            SendToClient(sid, out.GetBuffer(), out.GetSize());
        }

        // 계정 생성 결과를 보낸다 (SC_REGISTER_RESULT). 검증 실패(INVALID) 즉시 회신용 - 성공/중복은 CreateAccountCommand 가 직접 Send.
        void LoginHandler::SendRegisterResult(UINT64 sid, BYTE result)
        {
            SC_REGISTER_RESULT res;
            res.result = result;

            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_REGISTER_RESULT));
            res.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            SendToClient(sid, out.GetBuffer(), out.GetSize());
        }

        // 잘못된 순서/요청을 보낸 세션을 끊는다.
        void LoginHandler::Kick(UINT64 sid, EDisconnectReason reason)
        {
            m_server->DisconnectTo(sid, reason);   // 없는/옛 핸들이면 no-op
        }
    }
}
