#include "pch_gameserver.h"
#include "Core/Config/ClientConfig.h"
#include "Core/Config/ConfigParse.h"   // Trim / SkipUtf8Bom / OpenConfigFile / ParseIpv4 / AppendMissingKey / ReportError (서버 로더와 공용)

#include <cstdio>      // fgets / fclose / FILE
#include <cstring>     // strcmp / strchr
#include <cstdlib>     // _countof

namespace
{
	const wchar_t* const CONFIG_TAG  = L"[Client_Config] ";   // 콘솔 진단 접두어 - 서버 설정 진단([Server_Config])과 한눈에 구분
	const wchar_t* const CONFIG_FILE = L"Client_Config.ini";

	using KYS::GAMESERVER::CONFIG::CONFIG_LINE_MAX;
	using KYS::GAMESERVER::CONFIG::Trim;
	using KYS::GAMESERVER::CONFIG::SkipUtf8Bom;
	using KYS::GAMESERVER::CONFIG::OpenConfigFile;
	using KYS::GAMESERVER::CONFIG::ParseIpv4;
	using KYS::GAMESERVER::CONFIG::AppendMissingKey;

	// 현재 읽고 있는 줄이 어느 섹션에 속하는가. 이 파일이 아는 섹션은 [login] 하나다.
	enum ESection
	{
		SECTION_NONE,      // 아직 어떤 [섹션] 도 안 나왔다
		SECTION_LOGIN,     // [login]  -> ClientConfig 로 담는다
		SECTION_UNKNOWN    // 모르는 [섹션] - 다음 헤더까지 통째로 건너뛴다
	};

	// 이 파일의 진단은 전부 [Client_Config] 접두어 - 공용 ReportError 에 태그를 고정해 부른다.
	template <typename... Args>
	void ReportError(const wchar_t* fmt, Args... args)
	{
		KYS::GAMESERVER::CONFIG::ReportError(CONFIG_TAG, fmt, args...);
	}
}

bool LoadClientConfig(ClientConfig* out)
{
	out->loginIp = 0;   // 진입 즉시 0 - 성공했을 때만 값이 실린다

	wchar_t tried[MAX_PATH * 4];
	FILE* fp = OpenConfigFile(CONFIG_FILE, CONFIG_TAG, tried, _countof(tried));
	if (fp == nullptr)
	{
		ReportError(L"파일을 찾을 수 없다 - 다음 위치를 확인했다:");
		wprintf(L"%s", tried);
		ReportError(L"견본 Client_Config.sample.ini 를 위 폴더에 Client_Config.ini 로 복사한다");
		return false;
	}

	bool loadOk = true;   // 형식 오류를 만나도 즉시 끊지 않는다 - 한 번 실행에 모든 문제를 보여준 뒤 실패로 끝낸다
	bool haveHost = false;

	ESection section = SECTION_NONE;
	bool firstLine = true;

	char line[CONFIG_LINE_MAX];
	while (fgets(line, CONFIG_LINE_MAX, fp) != nullptr)
	{
		char* text = line;
		if (firstLine)
		{
			text = SkipUtf8Bom(text);
			firstLine = false;
		}

		char* p = Trim(text);
		if (p[0] == '\0') { continue; }                    // 빈 줄
		if (p[0] == '#' || p[0] == ';') { continue; }      // 주석 - 줄 전체만 주석이다(인라인 주석 미지원)

		// 섹션 헤더
		if (p[0] == '[')
		{
			char* close = strchr(p, ']');
			if (close == nullptr)
			{
				ReportError(L"섹션 헤더 형식 오류 - 닫는 ] 가 없다 (입력: \"%hs\")", p);
				loadOk = false;
				continue;
			}
			*close = '\0';
			const char* name = p + 1;

			if (strcmp(name, "login") == 0) { section = SECTION_LOGIN; }
			else
			{
				// 모르는 섹션은 여기서 한 번만 알린다. 그 안의 키까지 "알 수 없는 키"로 도배하지 않는다.
				ReportError(L"알 수 없는 섹션 - 다음 섹션까지 무시한다 (section: \"%hs\")", name);
				section = SECTION_UNKNOWN;
			}
			continue;
		}

		if (section == SECTION_UNKNOWN) { continue; }      // 모르는 섹션 안 - 조용히 건너뛴다

		if (section == SECTION_NONE)
		{
			char* eq = strchr(p, '=');
			if (eq != nullptr) { *eq = '\0'; }
			ReportError(L"섹션 밖에 키가 있다 - 파일 첫 줄에 [login] 이 있는지 확인한다 (key: \"%hs\")", Trim(p));
			loadOk = false;
			continue;
		}

		char* eq = strchr(p, '=');
		if (eq == nullptr)
		{
			ReportError(L"'=' 가 없는 줄이다 - key=value 형식이어야 한다 (입력: \"%hs\")", p);
			loadOk = false;
			continue;
		}
		*eq = '\0';
		char* key = Trim(p);
		char* val = Trim(eq + 1);

		// section == SECTION_LOGIN
		if (strcmp(key, "host") == 0)
		{
			UINT32 parsed = 0;
			const wchar_t* reason = nullptr;
			if (!ParseIpv4(val, &parsed, &reason))
			{
				ReportError(L"host 형식 오류 - %s (입력: \"%hs\")", reason, val);
				loadOk = false;
			}
			else if (parsed == 0)
			{
				ReportError(L"host 가 0.0.0.0 이다 - 붙을 로그인 서버 주소가 없다. 로그인 서버 머신의 IP 를 적는다(로컬 개발은 127.0.0.1)");
				loadOk = false;
			}
			else
			{
				out->loginIp = parsed;
				haveHost = true;
			}
		}
		else
		{
			// 아는 섹션 안의 모르는 키 - 오타를 숨기지 않도록 한 줄 알리고 계속 간다.
			ReportError(L"[login] 섹션의 알 수 없는 키 - 무시한다 (key: \"%hs\")", key);
		}
	}
	fclose(fp);

	wchar_t missing[128];
	missing[0] = L'\0';
	{
		bool sectionOpen = false;
		if (!haveHost) { AppendMissingKey(missing, _countof(missing), L"[login]", L"host", &sectionOpen); }
	}
	if (missing[0] != L'\0')
	{
		ReportError(L"필수 키 누락: %s", missing);
		loadOk = false;
	}

	return loadOk;
}
