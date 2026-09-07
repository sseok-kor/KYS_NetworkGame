#include "pch_serverapp.h"
#include "MainServer.h"
#include "../GameCommon/GameDefines.h"   // CONNECT_RATE_WINDOW_MS 등 per-IP 레이트 상수
#include "../GameServer/Core/Log/Logger.h"   // 보안 이벤트(거부/오류) 파일 로그 - 디버거 전용 ODS 에서 승격

namespace KYS
{
    namespace SERVERAPP
    {
        MainServer::MainServer()
        {
            // base IOCPServer 기본 생성
        }

        MainServer::~MainServer()
        {
            // base ~IOCPServer()가 정리
        }

        // 새 연결을 받을지 결정한다 - loopback 은 면제, 외부 IP 는 per-IP 고정창 연결 레이트로 폭주를 거른다.
        //   addr : 접속한 상대의 IP/포트. 거부(false)면 Session 배정 전이라 자원 소모 0.
        bool MainServer::OnConnectionRequest(const sockaddr_in& addr)
        {
            const UINT32 ip = addr.sin_addr.S_un.S_addr;   // network byte order
            if (ip == ::htonl(INADDR_LOOPBACK))
            {
                return true;   // loopback(로컬 봇/부하테스트/개발) - 레이트 면제. 방어 대상은 외부 IP.
            }

            const UINT64 now = ::GetTickCount64();
            KYS::GAMESERVER::THREAD::SpinLockGuard g(m_connectRateLock);

            // 맵이 커지면(스푸핑 다수 IP) stale 창을 정리해 메모리 상한을 둔다.
            if (m_ipConnects.size() > static_cast<size_t>(CONNECT_IP_TABLE_SOFT_CAP))
            {
                for (auto it = m_ipConnects.begin(); it != m_ipConnects.end(); )
                {
                    if (now - it->second.windowStartMs >= CONNECT_RATE_WINDOW_MS * 2) { it = m_ipConnects.erase(it); }
                    else { ++it; }
                }
            }

            // 고정창(fixed-window) - 창 경계에서 최대 2x 버스트(창 끝 30 + 롤오버 직후 30) 가능. coarse connect 게이트엔 허용(정확한 상한은 sliding-window/token-bucket 필요 = 과설계).
            IpConnectWindow& w = m_ipConnects[ip];   // 없으면 {0,0} 생성
            if (now - w.windowStartMs >= CONNECT_RATE_WINDOW_MS)
            {
                w.windowStartMs = now;   // 새 창 시작
                w.count = 0;
            }
            ++w.count;
            if (w.count > MAX_CONNECTS_PER_IP_PER_WINDOW)
            {
                ++m_ipRejectCount;   // 거부 누적 (m_connectRateLock 안 - 모니터/export 가 무락 근사 read)
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_WARN, L"net",
                    L"연결 거부 (per-IP rate) ip=%u.%u.%u.%u",
                    ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
                return false;   // 레이트 초과 - Session 배정 전 거부
            }
            return true;
        }

        // 라이브러리가 알린 오류를 파일 로그에 남긴다 (디버거 전용 ODS 에서 승격 - 운영 중에도 이력이 남게).
        //   code : 오류 종류 (enum class : int)
        //   msg  : 오류 메시지 (없으면 null)
        void MainServer::OnError(ErrorCode code, const wchar_t* msg)
        {
            // ErrorCode가 enum class : int 라 static_cast<int>로 값 출력.
            KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"net",
                L"라이브러리 오류 code=%d msg=%s", static_cast<int>(code), (msg != nullptr ? msg : L"(null)"));
        }
    }
}
