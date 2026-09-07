#pragma once
#include "../GameServer/Types/Defines.h"                 // UINT64 / BYTE
#include "../GameServer/Core/Thread/SRWLockWrapper.h"    // 세션별 토큰 맵 보호 (IOCP 워커 여럿이 동시 접근)
#include <unordered_map>

namespace KYS
{
    namespace GAMESERVER
    {
        namespace NETWORK
        {
            class IOCPServer;   // SendTo / DisconnectTo 용 (전방 선언)
        }
    }
}

namespace KYS
{
    namespace LOGINSERVER
    {
        // 클라 대면 로그인 처리 - 2단계 핸드셰이크의 앞단을 담당한다.
        //   1) CS_LOGIN(id/pwHash) 검증 -> 토큰 발급/적재 -> 게임 서버 목록(SC_SERVER_LIST) 회신
        //   2) CS_SERVER_SELECT(serverId) -> 그 세션에 발급해 둔 토큰 + 게임 서버 주소(SC_LOGIN_TOKEN) 회신
        //   토큰 검증/소비(인터서버 IS_*)는 다음 단계라 여기선 TokenStore에 적재만 한다.
        //   IOCP 워커 스레드 여럿이 서로 다른 세션을 동시에 처리하므로 세션->토큰 맵은 SRWLock으로 지킨다.
        class LoginHandler
        {
        public:
            LoginHandler();
            ~LoginHandler();

            LoginHandler(const LoginHandler&) = delete;            // 복사 2줄 (락/맵 단일 소유, 이동 자동 미선언)
            LoginHandler& operator=(const LoginHandler&) = delete;

            void Init(KYS::GAMESERVER::NETWORK::IOCPServer* server);   // 송신/종료에 쓸 서버 핸들 주입 (비소유)

            static LONG GetAttemptBlockCount();   // 연결당 시도 cap 차단 누적 (flood 방어 관측 - 모니터 표출)
            static void SetGameServerAddress(UINT32 ip, USHORT port);   // 부팅 1회(워커 시작 전) - conf/Server_Config.ini 의 [server] public_ip/public_port 를 SC_SERVER_LIST/SC_LOGIN_TOKEN 응답에 실을 값으로 주입

            // 디스패처가 opcode로 갈래를 정해 호출 (data = 헤더 4B + payload, size = 전체 바이트)
            void HandleLogin(UINT64 sid, const BYTE* data, int size);          // CS_LOGIN
            void HandleRegister(UINT64 sid, const BYTE* data, int size);       // CS_REGISTER (계정 생성 - 검증 후 LoginDbThread 위임)
            void HandleServerSelect(UINT64 sid, const BYTE* data, int size);   // CS_SERVER_SELECT

            void OnClientGone(UINT64 sid);   // 연결 종료 - 세션->토큰 항목 정리 (떠도는 항목 방지)

        private:
            void SendToClient(UINT64 sid, const BYTE* data, int size);   // sid의 소켓으로 한 패킷 송신 (풀의 SendTo - KEEP_CONNECTION)
            void SendLoginFail(UINT64 sid);                              // 인증 실패 응답 (SC_LOGIN_RESULT{FAIL})
            void SendRegisterResult(UINT64 sid, BYTE result);            // 계정 생성 결과 응답 (검증 실패 즉시 회신용 - 성공/중복은 CreateAccountCommand 가 보냄)
            void Kick(UINT64 sid, EDisconnectReason reason);             // 잘못된 순서/요청 - 연결 종료

            bool AllowLoginAttempt(UINT64 sid);      // 이 연결의 CS_LOGIN 시도 +1, cap 이하면 true (초과 시 Verify/PBKDF2 스킵 - 증폭 방어)
            bool AllowRegisterAttempt(UINT64 sid);   // 이 연결의 CS_REGISTER 시도 +1, cap 이하면 true (초과 시 DB 위임 스킵)

            KYS::GAMESERVER::NETWORK::IOCPServer* m_server;   // 송신/종료 진입점 (비소유, Init에서 주입)

            std::unordered_map<UINT64, UINT64> m_sessionTokens;   // sid -> CS_LOGIN에서 발급한 토큰 (CS_SERVER_SELECT가 되읽음)
            KYS::GAMESERVER::THREAD::SRWLockWrapper m_tokenLock;   // read=조회 / write=발급 insert, 종료 erase

            static volatile LONG s_attemptBlockCount;   // 시도 cap 차단 누적 (IOCP 워커 다수 - Interlocked)
            static UINT32 s_gameServerIp;      // 클라에 알릴 게임 서버 IP (host-order) - 부팅 1회 set(워커 시작 전), 이후 read-only. 기본값 없음(0 이면 서버 오설정)
            static USHORT s_gameServerPort;    // 클라에 알릴 게임 서버 포트 - 부팅 1회 set(워커 시작 전), 이후 read-only. 기본값 없음(0 이면 서버 오설정)

            // per-connection 로그인/가입 시도 카운터 - PBKDF2 증폭/DB 큐 적체를 소켓 단위로만 막는다(계정 잠금 없음 - lockout DoS 회피)
            struct ConnAttempts { UINT32 loginCount = 0; UINT32 registerCount = 0; };
            std::unordered_map<UINT64, ConnAttempts> m_connAttempts;   // sid -> 이 연결의 시도 수 (OnClientGone에서 정리, m_sessionTokens와 동일 수명)
            KYS::GAMESERVER::THREAD::SRWLockWrapper m_rateLock;        // m_connAttempts 보호 (IOCP 워커 여럿 동시 접근)
        };
    }
}
