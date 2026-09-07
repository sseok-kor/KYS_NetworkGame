#include "pch_loginserver.h"
#include "InterServerIocpServer.h"
#include "../GameServer/Core/Log/Logger.h"   // 인터서버 보안 이벤트 파일 로그 (디버거 전용 ODS 에서 승격)

namespace KYS
{
	namespace LOGINSERVER
	{
		InterServerIocpServer::InterServerIocpServer()
		{
			// base IOCPServer 기본 생성
		}

		InterServerIocpServer::~InterServerIocpServer()
		{
			// base ~IOCPServer() 가 정리
		}

		// 인터서버 연결을 받을지 결정한다 - loopback(127.0.0.1)만 수락한다(단일머신 배포).
		//   신뢰 경계 1차 = accept 단계 IP allow-list(Session 배정 전 거부), 2차 = 첫 패킷 IS_REGISTER 공유 비밀.
		//   loopback은 게임 서버가 상시 이 주소로 접속하므로 항상 허용. multi-host 배포로 확장 시 등록 게임서버 IP를
		//   config 리스트로 받아 여기서 대조(현재 스코프는 loopback 단일).
		bool InterServerIocpServer::OnConnectionRequest(const sockaddr_in& addr)
		{
			if (addr.sin_addr.S_un.S_addr != ::htonl(INADDR_LOOPBACK))
			{
				// 외부 IP 가 인터서버 포트를 두드림 = 진짜 보안 이벤트 - WARN 으로 파일에 남긴다 (B2 allow-list 차단 관측).
				const UINT32 ip = addr.sin_addr.S_un.S_addr;
				KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
					KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_WARN, L"inter",
					L"인터서버 포트 외부 접속 거부 (non-loopback) ip=%u.%u.%u.%u",
					ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
				return false;   // loopback 아님 - 인터서버 포트에 외부 접속 차단 (Session 배정 전)
			}
			// 정상 loopback 수락은 링크 (재)수립마다 1건 - DEBUG 강등 (link up 로그가 INFO 로 이미 남는다).
			KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
				KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_DEBUG, L"inter",
				L"인터서버 연결 수락 (loopback)");
			return true;
		}

		// 라이브러리가 알린 오류를 파일 로그에 남긴다.
		//   code : 오류 종류 (enum class : int)
		//   msg  : 오류 메시지 (없으면 null)
		void InterServerIocpServer::OnError(ErrorCode code, const wchar_t* msg)
		{
			KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
				KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"inter",
				L"라이브러리 오류 code=%d msg=%s", static_cast<int>(code), (msg != nullptr ? msg : L"(null)"));
		}
	}
}
