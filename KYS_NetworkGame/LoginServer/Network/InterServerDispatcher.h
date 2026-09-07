#pragma once
#include "../GameServer/Network/IOCP/INetEventHandler.h"

namespace KYS
{
    namespace GAMESERVER
    {
        namespace NETWORK
        {
            class IOCPServer;   // 전방 선언 (응답 SendTo + 잘못된 패킷 DisconnectTo)
        }
    }
}

namespace KYS
{
    namespace LOGINSERVER
    {
        // 게임 서버(ServerApp)가 인터서버 포트로 보내는 IS_* 질의를 받는 LoginServer 측 유일 통로.
        //   클라 대면 LoginDispatcher 와 같은 INetEventHandler 구현이지만 인터서버 트래픽만 처리한다(2nd 포트라 섞이지 않음).
        //   - IS_REGISTER       : 링크 등록 - 공유 비밀(authKeyHash) 검사 후 IS_REGISTER_ACK 회신
        //   - IS_TOKEN_VERIFY_REQ : 토큰 검증 - TokenStore.Consume(일회용) + 중복 로그인 판정 후 IS_TOKEN_VERIFY_RES 회신
        //   - IS_PLAYER_LEAVE   : 접속 종료 통지 - online 표에서 계정 해제 (응답 없음)
        //   - IS_ONLINE_SYNC    : 게임 권위 online 스냅샷 refresh (ghost 정리용, 응답 없음)
        //   응답은 받은 그 링크 세션(linkSid)으로 돌려보낸다. 검증 요청 안의 sid(게임 세션 sid)는 페이로드에 실어
        //   ServerApp 이 어느 대기 세션의 응답인지 상관하게 한다(linkSid != 게임 sid).
        //   토큰 발급(클라 스레드)과 토큰 소비(여기, 인터서버 워커 스레드)가 서로 다른 스레드라 TokenStore 가 SRWLock 으로 보호한다.
        class InterServerDispatcher : public KYS::GAMESERVER::NETWORK::INetEventHandler
        {
        public:
            static InterServerDispatcher& GetInstance() { static InterServerDispatcher instance; return instance; }   // Meyers static (LoginDispatcher 미러)

            InterServerDispatcher(const InterServerDispatcher&) = delete;            // 복사 2줄 (싱글턴 - 이동 자동 미선언)
            InterServerDispatcher& operator=(const InterServerDispatcher&) = delete;

            bool Init(KYS::GAMESERVER::NETWORK::IOCPServer* server);   // 인터서버 서버 핸들 주입 (응답 Send + 잘못된 패킷 종료용). null 이면 false.
            void SetAuthSecret(UINT32 secret) { m_authSecret = secret; }   // conf/Server_Config.ini 의 [interserver] secret 주입 (Init 후 Start 전, 부팅 검증으로 non-zero 보장)

            // INetEventHandler 구현 (라이브러리가 호출)
            void EnqueueConnect(UINT64 linkSid) override;
            void EnqueuePacket(UINT64 linkSid, const BYTE* data, int len) override;
            void EnqueueDisconnect(UINT64 linkSid, EDisconnectReason reason) override;

            // 모니터 카운터 (인터서버 통계 - LoginServer 모니터가 읽음). 인터서버 워커 다수 -> Interlocked.
            static volatile LONG s_verifyReq;   // 검증 요청 수신 수
            static volatile LONG s_verifyOk;    // 검증 OK(토큰 유효) 수
            static volatile LONG s_kick;        // 중복 로그인 축출(IS_KICK 송신) 수
            static volatile LONG s_leave;       // 접속 종료 통지(IS_PLAYER_LEAVE 수신) 수
            static volatile LONG s_onlineSync;  // online 스냅샷(IS_ONLINE_SYNC) 수신 수 (수렴 흐름 확인)
            static volatile LONG s_sendDrop;    // SendOnLink 실패 수 (IS_KICK/RES 무점검 silent drop 관측화)

            static LONG GetSendDropCount() { return s_sendDrop; }   // 인터서버 링크 송신 실패/백프레셔 누적 (모니터 표출 - write-only 해소, 0=정상)

        private:
            InterServerDispatcher();     // 싱글턴 - GetInstance 로만 생성
            ~InterServerDispatcher() override;

            void HandleRegister(UINT64 linkSid, const BYTE* data, int len);      // IS_REGISTER -> IS_REGISTER_ACK
            void HandleTokenVerify(UINT64 linkSid, const BYTE* data, int len);   // IS_TOKEN_VERIFY_REQ -> IS_TOKEN_VERIFY_RES
            void HandlePlayerLeave(UINT64 linkSid, const BYTE* data, int len);   // IS_PLAYER_LEAVE (응답 없음)
            void HandleOnlineSync(UINT64 linkSid, const BYTE* data, int len);    // IS_ONLINE_SYNC (게임 권위 online 스냅샷 refresh, 응답 없음)

            void SendOnLink(UINT64 linkSid, const BYTE* data, int size);   // linkSid 의 소켓으로 한 패킷 송신 (풀의 SendTo - KEEP_CONNECTION · drop 은 s_sendDrop 관측)

            KYS::GAMESERVER::NETWORK::IOCPServer* m_server;   // 응답 Send + 잘못된 패킷 종료 진입점 (비소유, Init 에서 주입)
            UINT32 m_authSecret;   // 인터서버 공유 비밀 (conf/Server_Config.ini 의 [interserver] secret 에서 주입) - IS_REGISTER authKeyHash 대조. 0이면 부팅에서 이미 걸러짐
        };
    }
}
