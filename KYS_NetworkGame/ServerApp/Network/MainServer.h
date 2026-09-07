#pragma once
#include "../GameServer/Network/IOCP/IOCPServer.h"
#include "../GameServer/Core/Thread/SpinLock.h"   // per-IP 연결 레이트 맵 보호 (accept 훅 동시 접근)
#include <unordered_map>

namespace KYS
{
    namespace SERVERAPP
    {
        // IOCPServer를 상속한 서버 본체 - 라이브러리가 부르는 두 콜백(연결 수락 여부 / 오류 보고)을 게임측에서 구현한다.
        class MainServer : public KYS::GAMESERVER::NETWORK::IOCPServer
        {
        public:
            static MainServer& GetInstance() { static MainServer instance; return instance; }   // Meyers static

            MainServer(const MainServer&) = delete;            // 복사 2줄 (싱글턴 - 이동 자동 미선언)
            MainServer& operator=(const MainServer&) = delete;

            // 라이브러리 콜백 구현 (IOCPServer 순수가상 override)
            bool OnConnectionRequest(const sockaddr_in& addr) override;   // accept 직전 - 이 연결을 받을지 결정 (true면 수락)
            void OnError(ErrorCode code, const wchar_t* msg) override;    // 라이브러리가 알린 오류를 로그로 남김

            UINT64 GetIpRejectCount() const { return m_ipRejectCount; }   // per-IP 레이트 거부 누적 (accept 스레드가 락 안 증가 - 무락 근사 read)

        private:
            MainServer();              // 싱글턴 - GetInstance로만 생성
            ~MainServer() override;

            // per-IP 연결 레이트 (고정창) - 외부 IP 연결 폭주/DDoS 를 accept 단계(Session 배정 전)에서 거부. loopback 은 면제.
            struct IpConnectWindow { UINT64 windowStartMs = 0; UINT32 count = 0; };
            std::unordered_map<UINT32, IpConnectWindow> m_ipConnects;   // ip(net order) -> 현재 창 연결 수
            KYS::GAMESERVER::THREAD::SpinLock m_connectRateLock;        // m_ipConnects 보호
            UINT64 m_ipRejectCount = 0;                                 // 거부 누적 (m_connectRateLock 편승 - 모니터/export 표출)
        };
    }
}
