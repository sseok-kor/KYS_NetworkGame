#include "pch_gameserver.h"
#include "Core/Config/ServerConfig.h"
#include "Core/Config/ConfigParse.h"   // Trim / SkipUtf8Bom / OpenConfigFile / ParseU32 / ParseIpv4 / AppendMissingKey / ReportError (클라 로더와 공용)

#include <cstdio>      // fgets / fclose / FILE
#include <cstring>     // strncpy_s / strcmp / strchr
#include <cstdlib>     // _countof

namespace
{
	const wchar_t* const CONFIG_TAG  = L"[Server_Config] ";     // 콘솔 진단 접두어 - 부팅 로그에서 설정 관련 줄만 눈으로 걸러낸다
	const wchar_t* const CONFIG_FILE = L"Server_Config.ini";

	// 줄 다듬기 / 폴더 탐색 / 값 검증은 클라 로더(ClientConfig)와 한 벌을 나눠 쓴다 - ConfigParse.h
	using KYS::GAMESERVER::CONFIG::CONFIG_LINE_MAX;
	using KYS::GAMESERVER::CONFIG::Trim;
	using KYS::GAMESERVER::CONFIG::SkipUtf8Bom;
	using KYS::GAMESERVER::CONFIG::OpenConfigFile;
	using KYS::GAMESERVER::CONFIG::ParseU32;
	using KYS::GAMESERVER::CONFIG::ParseIpv4;
	using KYS::GAMESERVER::CONFIG::AppendMissingKey;

	// 현재 읽고 있는 줄이 어느 섹션에 속하는가. 파일의 [database]/[server]/[interserver] 헤더가 이 값을 바꾼다.
	enum ESection
	{
		SECTION_NONE,          // 아직 어떤 [섹션] 도 안 나왔다 (파일 첫 섹션 이전)
		SECTION_DATABASE,      // [database]    -> DbConfig 로 담는다
		SECTION_SERVER,        // [server]      -> ServerConfig 로 담는다
		SECTION_INTERSERVER,   // [interserver] -> InterServerConfig 로 담는다
		SECTION_UNKNOWN        // 우리가 모르는 [섹션] - 다음 헤더까지 통째로 건너뛴다
	};

	// 이 파일의 진단은 전부 [Server_Config] 접두어 - 공용 ReportError 에 태그를 고정해 부른다.
	template <typename... Args>
	void ReportError(const wchar_t* fmt, Args... args)
	{
		KYS::GAMESERVER::CONFIG::ReportError(CONFIG_TAG, fmt, args...);
	}

	// 진단 메시지에 찍을 섹션 이름.
	const wchar_t* SectionName(ESection section)
	{
		switch (section)
		{
		case SECTION_DATABASE:    return L"[database]";
		case SECTION_SERVER:      return L"[server]";
		case SECTION_INTERSERVER: return L"[interserver]";
		default:                  return L"[?]";
		}
	}

	// 아는 섹션 안의 모르는 키 - 오타를 숨기지 않도록 한 줄 알리고 계속 간다.
	//   섹션명을 함께 찍어야 "키를 엉뚱한 섹션에 넣음"도 같은 한 줄로 드러난다.
	void WarnUnknownKey(ESection section, const char* key)
	{
		ReportError(L"%s 섹션의 알 수 없는 키 - 무시한다 (key: \"%hs\")", SectionName(section), key);
	}
}

bool LoadServerConfig(DbConfig* outDb, ServerConfig* outServer, InterServerConfig* outInter)
{
	// 기본값은 진입 즉시 심는다 - 기본값의 출처를 이 한 곳으로 모아 "설정 파일이 일부만 주는" 경우를 여기서 흡수한다.
	//   nullptr 인 구조체는 그 프로세스가 안 쓰는 섹션이므로 손대지 않는다.
	if (outDb != nullptr)
	{
		strncpy_s(outDb->host, sizeof(outDb->host), "127.0.0.1", _TRUNCATE);
		outDb->user[0] = '\0';
		outDb->password[0] = '\0';
		outDb->dbname[0] = '\0';
		outDb->port = 3306;
	}
	if (outServer != nullptr)
	{
		outServer->publicIp = 0;
		outServer->publicPort = 0;
	}
	if (outInter != nullptr)
	{
		outInter->secret = 0;
	}

	wchar_t tried[MAX_PATH * 4];
	FILE* fp = OpenConfigFile(CONFIG_FILE, CONFIG_TAG, tried, _countof(tried));
	if (fp == nullptr)
	{
		ReportError(L"파일을 찾을 수 없다 - 다음 위치를 확인했다:");
		wprintf(L"%s", tried);
		return false;
	}

	bool loadOk = true;   // 형식 오류를 만나도 즉시 끊지 않는다 - 한 번 실행에 모든 문제를 보여준 뒤 실패로 끝낸다

	bool haveUser = false, havePassword = false, haveDbname = false;
	bool havePublicIp = false, havePublicPort = false;
	bool haveSecret = false;

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

			if (strcmp(name, "database") == 0)          { section = SECTION_DATABASE; }
			else if (strcmp(name, "server") == 0)       { section = SECTION_SERVER; }
			else if (strcmp(name, "interserver") == 0)  { section = SECTION_INTERSERVER; }
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
			// 구 db_config.txt 를 섹션 헤더 없이 그대로 옮겼을 때 여기 걸린다. 원인이 곧 처방이 되도록 첫 키에서 말한다.
			char* eq = strchr(p, '=');
			if (eq != nullptr) { *eq = '\0'; }
			ReportError(L"섹션 밖에 키가 있다 - 파일 첫 줄에 [database] 가 있는지 확인한다 (key: \"%hs\")", Trim(p));
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

		if (section == SECTION_DATABASE)
		{
			if (outDb == nullptr) { continue; }   // 이 프로세스는 DB 를 안 쓴다. 읽지도 검증하지도 않는다

			if (strcmp(key, "host") == 0)
			{
				// 키를 아예 안 쓰면 기본값 127.0.0.1 이지만, 쓰다 만 빈 값은 형식 오류다(port 와 같은 규칙).
				//   빈 문자열을 그대로 넘기면 libmysqlclient 가 로컬로 해석해 연결이 "성공"한다 - 문서가
				//   보장하는 동작이 아니고, 원격 DB 주소를 지우다 만 사고가 조용히 로컬 접속으로 덮인다.
				if (val[0] == '\0')
				{
					ReportError(L"host 형식 오류 - 값이 비어 있다. 주소를 적거나 host 줄 자체를 지운다(줄이 없으면 127.0.0.1)");
					loadOk = false;
				}
				else
				{
					strncpy_s(outDb->host, sizeof(outDb->host), val, _TRUNCATE);
				}
			}
			else if (strcmp(key, "port") == 0)
			{
				UINT32 parsed = 0;
				if (!ParseU32(val, &parsed, 10))
				{
					ReportError(L"port 형식 오류 - 값 전체가 10진 숫자가 아니다 (입력: \"%hs\")", val);
					loadOk = false;
				}
				else if (parsed == 0 || parsed > 65535)
				{
					ReportError(L"port 범위 오류 - 1..65535 가 아니다 (입력: \"%hs\")", val);
					loadOk = false;
				}
				else
				{
					outDb->port = static_cast<int>(parsed);   // 성공했을 때만 반영 - 파손 값은 기본값으로 조용히 넘어가지 않는다
				}
			}
			else if (strcmp(key, "user") == 0)
			{
				strncpy_s(outDb->user, sizeof(outDb->user), val, _TRUNCATE);
				haveUser = (val[0] != '\0');
			}
			else if (strcmp(key, "password") == 0)
			{
				strncpy_s(outDb->password, sizeof(outDb->password), val, _TRUNCATE);
				havePassword = (val[0] != '\0');
			}
			else if (strcmp(key, "dbname") == 0)
			{
				strncpy_s(outDb->dbname, sizeof(outDb->dbname), val, _TRUNCATE);
				haveDbname = (val[0] != '\0');
			}
			else
			{
				WarnUnknownKey(section, key);
			}
		}
		else if (section == SECTION_SERVER)
		{
			if (outServer == nullptr) { continue; }   // 이 프로세스는 게임 서버 주소를 안 쓴다

			if (strcmp(key, "public_ip") == 0)
			{
				UINT32 parsed = 0;
				const wchar_t* reason = nullptr;
				if (!ParseIpv4(val, &parsed, &reason))
				{
					ReportError(L"public_ip 형식 오류 - %s (입력: \"%hs\")", reason, val);
					loadOk = false;
				}
				else if (parsed == 0)
				{
					// 0.0.0.0 은 형식상 통과하지만 클라에 광고할 주소가 못 된다. 게다가 코드가 publicIp==0 을
					//   "미설정" 센티널로 쓰므로(LoginHandler / ClientSocket), 통과시키면 전 클라가 접속 실패한다.
					//   listen 은 어차피 INADDR_ANY 라 "모든 인터페이스" 의도를 이 키로 표현할 일도 없다.
					ReportError(L"public_ip 가 0.0.0.0 이다 - 클라에 광고할 주소가 없다. 이 머신의 실제 IP 를 적는다(로컬 개발은 127.0.0.1)");
					loadOk = false;
				}
				else
				{
					outServer->publicIp = parsed;
					havePublicIp = true;
				}
			}
			else if (strcmp(key, "public_port") == 0)
			{
				UINT32 parsed = 0;
				if (!ParseU32(val, &parsed, 10))
				{
					ReportError(L"public_port 형식 오류 - 값 전체가 10진 숫자가 아니다 (입력: \"%hs\")", val);
					loadOk = false;
				}
				else if (parsed == 0 || parsed > 65535)
				{
					ReportError(L"public_port 범위 오류 - 1..65535 가 아니다 (입력: \"%hs\")", val);
					loadOk = false;
				}
				else
				{
					outServer->publicPort = static_cast<USHORT>(parsed);
					havePublicPort = true;
				}
			}
			else
			{
				WarnUnknownKey(section, key);
			}
		}
		else if (section == SECTION_INTERSERVER)
		{
			if (outInter == nullptr) { continue; }   // 이 프로세스는 인터서버 링크를 안 쓴다

			if (strcmp(key, "secret") == 0)
			{
				UINT32 parsed = 0;
				if (!ParseU32(val, &parsed, 0))   // base 0 = 16진(0x..) 표기를 받는 유일한 키
				{
					ReportError(L"secret 형식 오류 - 값 전체가 숫자가 아니다 (입력: \"%hs\")", val);
					loadOk = false;
				}
				else if (parsed == 0)
				{
					ReportError(L"secret 이 0 이다 - 인터서버 인증이 무력화되므로 허용하지 않는다");
					loadOk = false;
				}
				else
				{
					outInter->secret = parsed;
					haveSecret = true;
				}
			}
			else
			{
				WarnUnknownKey(section, key);
			}
		}
	}
	fclose(fp);

	// 필수 키 누락을 섹션과 함께 한 줄로 모아 알린다. 키를 엉뚱한 섹션에 넣었으면 위 WARN 줄과 짝을 이뤄 원인을 가리킨다.
	wchar_t missing[512];
	missing[0] = L'\0';

	if (outDb != nullptr)
	{
		bool sectionOpen = false;
		if (!haveUser)     { AppendMissingKey(missing, _countof(missing), L"[database]", L"user", &sectionOpen); }
		if (!havePassword) { AppendMissingKey(missing, _countof(missing), L"[database]", L"password", &sectionOpen); }
		if (!haveDbname)   { AppendMissingKey(missing, _countof(missing), L"[database]", L"dbname", &sectionOpen); }
	}
	if (outServer != nullptr)
	{
		bool sectionOpen = false;
		if (!havePublicIp)   { AppendMissingKey(missing, _countof(missing), L"[server]", L"public_ip", &sectionOpen); }
		if (!havePublicPort) { AppendMissingKey(missing, _countof(missing), L"[server]", L"public_port", &sectionOpen); }
	}
	if (outInter != nullptr)
	{
		bool sectionOpen = false;
		if (!haveSecret) { AppendMissingKey(missing, _countof(missing), L"[interserver]", L"secret", &sectionOpen); }
	}

	if (missing[0] != L'\0')
	{
		ReportError(L"필수 키 누락: %s", missing);
		loadOk = false;
	}

	return loadOk;
}
