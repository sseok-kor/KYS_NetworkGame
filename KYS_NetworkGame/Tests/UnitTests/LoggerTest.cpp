#include "pch_tests.h"

#include "Core/Log/Logger.h"

#include <io.h>       // _dup / _dup2 / _close / _fileno
#include <cstdio>     // fflush / _wfreopen_s / fclose
#include <cwchar>     // wcsstr
#include <string>     // std::wstring (테스트 코드 - C++17 허용)
#include <fstream>    // std::wifstream
#include <sstream>    // std::wstringstream

using namespace KYS::GAMESERVER::LOG;

namespace
{
	// 파일 전체를 wide 문자열로 읽는다 (UTF-8 바이트를 ASCII 범위만 쓰므로 그대로 넓힌다).
	std::wstring ReadAll(const wchar_t* path)
	{
		std::ifstream in(path, std::ios::binary);
		std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		return std::wstring(bytes.begin(), bytes.end());
	}

	// Logs\<app>_<pid>_<channel>_*.log 가운데 첫 것을 찾는다 (이 프로세스가 방금 만든 파일 - pid 로 좁힌다).
	bool FindOwnLogFile(const wchar_t* app, const wchar_t* channel, wchar_t* outPath, size_t cap)
	{
		wchar_t pattern[MAX_PATH];
		swprintf_s(pattern, L"Logs\\%s_%u_%s_*.log", app, ::GetCurrentProcessId(), channel);
		WIN32_FIND_DATAW fd;
		HANDLE h = ::FindFirstFileW(pattern, &fd);
		if (h == INVALID_HANDLE_VALUE) { return false; }
		swprintf_s(outPath, cap, L"Logs\\%s", fd.cFileName);
		::FindClose(h);
		return true;
	}
}

// ==========================================================================
// 콘솔 병행 출력은 SERVER 채널만 - USER 채널(로그인 IP, 캐릭터 생성 등)은 파일 전용.
//   시연 화면(서버 콘솔)에 접속자 IP 가 뜨지 않게 하는 규칙이라, 로거 스레드가 echo 하는 stdout 을 파일로 돌려 잡는다.
// ==========================================================================

TEST(LoggerTest, ConsoleEchoIsServerChannelOnlyAndUserGoesToFile)
{
	// stdout 을 임시 파일로 돌린다 - 로거 스레드가 같은 프로세스의 stdout 에 wprintf 하므로 여기서 잡힌다.
	wchar_t tempDir[MAX_PATH];
	wchar_t capturePath[MAX_PATH];
	ASSERT_NE(::GetTempPathW(MAX_PATH, tempDir), 0u);
	ASSERT_NE(::GetTempFileNameW(tempDir, L"kys", 0, capturePath), 0u);

	fflush(stdout);
	const int savedStdout = _dup(_fileno(stdout));
	ASSERT_NE(savedStdout, -1);
	FILE* redirected = nullptr;
	ASSERT_EQ(_wfreopen_s(&redirected, capturePath, L"w", stdout), 0);

	Logger& logger = Logger::GetInstance();
	logger.Initialize(L"Tests");   // Logs\Tests_<pid>_server_*.log / _user_*.log 를 연다 (파일을 못 열면 콘솔 강등 - 그래도 echo 검사는 유효)
	logger.Log(LogChannel::SERVER, LogLevel::LL_INFO, L"echo", L"SERVER-CHANNEL-MARK");
	logger.Log(LogChannel::USER,   LogLevel::LL_INFO, L"echo", L"USER-CHANNEL-MARK ip=1.2.3.4");
	logger.Shutdown();             // 큐 잔량을 파일에 쓰고 로거 스레드를 join - 이 뒤에는 echo 가 더 나오지 않는다

	// stdout 복원 (gtest 출력이 다시 콘솔로 가게)
	fflush(stdout);
	_dup2(savedStdout, _fileno(stdout));
	_close(savedStdout);

	const std::wstring console = ReadAll(capturePath);
	::DeleteFileW(capturePath);
	EXPECT_NE(console.find(L"SERVER-CHANNEL-MARK"), std::wstring::npos) << "SERVER 채널 INFO 는 콘솔에 보여야 한다";
	EXPECT_EQ(console.find(L"USER-CHANNEL-MARK"),   std::wstring::npos) << "USER 채널은 콘솔에 나오면 안 된다 (접속자 IP 노출)";

	// USER 채널은 파일에는 남아야 한다 - 감사 자료가 사라진 것이 아니라 콘솔에서만 빠진 것
	wchar_t userLog[MAX_PATH];
	if (FindOwnLogFile(L"Tests", L"user", userLog, MAX_PATH))
	{
		const std::wstring userFile = ReadAll(userLog);
		EXPECT_NE(userFile.find(L"USER-CHANNEL-MARK"), std::wstring::npos) << "USER 채널 로그가 user 파일에 없다";
	}
	else
	{
		ADD_FAILURE() << "Logs\\Tests_<pid>_user_*.log 가 없다 - Logger 가 파일을 못 열고 콘솔 강등됐다면 그 자체가 검사 대상";
	}

	// 테스트가 만든 로그 파일 둘을 지운다 - 서버 로그 폴더에 테스트 흔적을 쌓지 않는다 (Shutdown 뒤라 핸들은 닫혀 있다).
	wchar_t ownLog[MAX_PATH];
	if (FindOwnLogFile(L"Tests", L"user", ownLog, MAX_PATH))   { ::DeleteFileW(ownLog); }
	if (FindOwnLogFile(L"Tests", L"server", ownLog, MAX_PATH)) { ::DeleteFileW(ownLog); }
}
