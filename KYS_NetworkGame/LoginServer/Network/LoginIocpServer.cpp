#include "pch_loginserver.h"
#include "LoginIocpServer.h"
#include "../GameServer/Core/Log/Logger.h"   // 네트워크 이벤트 파일 로그 (디버거 전용 ODS 에서 승격)

namespace KYS
{
	namespace LOGINSERVER
	{
		LoginIocpServer::LoginIocpServer()
		{
			// base IOCPServer 기본 생성
		}

		LoginIocpServer::~LoginIocpServer()
		{
			// base ~IOCPServer()가 정리
		}

		// 새 연결을 받을지 결정한다 (지금은 무조건 수락).
		//   addr : 접속한 상대의 IP/포트
		bool LoginIocpServer::OnConnectionRequest(const sockaddr_in& addr)
		{
			// 밴 IP/DDoS 거부는 이후 addr로 판단. 지금은 미사용.
			(void)addr;   // /W4 미사용 인자 경고 억제
			// 정상 수락은 연결마다 1건이라 DEBUG 로 강등 - 기본 레벨(INFO)에선 기록 안 됨 (필요 시 SetLogLevel 로 개방).
			KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
				KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_DEBUG, L"net",
				L"클라 연결 수락");
			return true;  // 수락
		}

		// 라이브러리가 알린 오류를 파일 로그에 남긴다 (운영 중에도 이력이 남게).
		//   code : 오류 종류 (enum class : int)
		//   msg  : 오류 메시지 (없으면 null)
		void LoginIocpServer::OnError(ErrorCode code, const wchar_t* msg)
		{
			// ErrorCode가 enum class : int 라 static_cast<int>로 값 출력.
			KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
				KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"net",
				L"라이브러리 오류 code=%d msg=%s", static_cast<int>(code), (msg != nullptr ? msg : L"(null)"));
		}
	}
}
