#include "pch_gameserver.h"
#include "Core/Config/ConfigParse.h"

#include <cstdio>      // _wfopen_s / FILE
#include <cstring>     // strlen
#include <cstdlib>     // strtoul / _countof
#include <cerrno>      // errno / ERANGE (ParseU32 오버플로 감지)
#include <cwchar>      // wcsrchr / swprintf_s / wcsncat_s

namespace
{
	const wchar_t* const CONFIG_DIR = L"conf";   // 전용 설정 폴더 - 두 설정 파일이 같은 폴더에 산다

	// 파일을 못 찾았을 때 "여기와 여기를 봤다"고 말하기 위해 시도한 절대경로를 한 줄씩 쌓아 둔다.
	void AppendTriedPath(const wchar_t* tag, wchar_t* outTried, size_t triedCap, const wchar_t* path)
	{
		wcsncat_s(outTried, triedCap, tag, _TRUNCATE);
		wcsncat_s(outTried, triedCap, L"  ", _TRUNCATE);
		wcsncat_s(outTried, triedCap, path, _TRUNCATE);
		wcsncat_s(outTried, triedCap, L"\n", _TRUNCATE);
	}
}

char* KYS::GAMESERVER::CONFIG::Trim(char* s)
{
	while (*s == ' ' || *s == '\t') { ++s; }   // 앞 공백
	char* end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
	{
		--end;
	}
	*end = '\0';
	return s;
}

char* KYS::GAMESERVER::CONFIG::SkipUtf8Bom(char* s)
{
	if (static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
		static_cast<unsigned char>(s[2]) == 0xBF)
	{
		return s + 3;
	}
	return s;
}

FILE* KYS::GAMESERVER::CONFIG::OpenConfigFile(const wchar_t* fileName, const wchar_t* tag, wchar_t* outTried, size_t triedCap)
{
	outTried[0] = L'\0';

	wchar_t candidate[MAX_PATH * 2];

	wchar_t exePath[MAX_PATH];
	const DWORD exeLen = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	if (exeLen > 0 && exeLen < _countof(exePath))
	{
		wchar_t* lastSlash = wcsrchr(exePath, L'\\');
		if (lastSlash != nullptr)
		{
			lastSlash[1] = L'\0';   // 파일명을 잘라 폴더 경로만 남긴다(끝에 역슬래시 유지)
			swprintf_s(candidate, _countof(candidate), L"%s%s\\%s", exePath, CONFIG_DIR, fileName);
			AppendTriedPath(tag, outTried, triedCap, candidate);

			FILE* fp = nullptr;
			if (_wfopen_s(&fp, candidate, L"rt") == 0 && fp != nullptr) { return fp; }
		}
	}

	wchar_t workDir[MAX_PATH];
	const DWORD workLen = ::GetCurrentDirectoryW(MAX_PATH, workDir);
	if (workLen > 0 && workLen < _countof(workDir))
	{
		swprintf_s(candidate, _countof(candidate), L"%s\\%s\\%s", workDir, CONFIG_DIR, fileName);
		AppendTriedPath(tag, outTried, triedCap, candidate);

		FILE* fp = nullptr;
		if (_wfopen_s(&fp, candidate, L"rt") == 0 && fp != nullptr) { return fp; }
	}

	return nullptr;
}

bool KYS::GAMESERVER::CONFIG::ParseU32(const char* val, UINT32* out, int base)
{
	if (val == nullptr || val[0] == '\0') { return false; }   // 빈 값 = 키를 쓰다 만 것
	if (val[0] == '+' || val[0] == '-') { return false; }     // 부호는 형식 오류 - strtoul 은 "-1" 을 4294967295 로 되돌리고 전량 소비까지 만족시켜, 그대로 두면 secret=-1 이 non-zero 검증을 통과한다
	char* end = nullptr;
	errno = 0;                                                // 오버플로 감지 준비 - strtoul 은 범위를 넘으면 ULONG_MAX 를 돌려주면서 전량 소비도 만족시킨다
	const unsigned long v = strtoul(val, &end, base);
	if (end == val || *end != '\0') { return false; }         // 한 글자도 못 읽었거나, 뒤에 뭔가 남았다
	if (errno == ERANGE) { return false; }                    // 32비트 범위 초과 - 적어 둔 값과 다른 값이 쓰이는 것을 막는다
	*out = static_cast<UINT32>(v);
	return true;
}

// sscanf_s 를 쓰지 않는 이유 = 반환값이 "몇 개 읽었는가"라서 "127.0.0.1x" 와 "1.2.3.4.5" 가 둘 다 4 를 준다.
bool KYS::GAMESERVER::CONFIG::ParseIpv4(const char* val, UINT32* out, const wchar_t** outReason)
{
	*outReason = L"점표기 a.b.c.d 가 아니다";
	if (val == nullptr) { return false; }

	UINT32 addr = 0;
	const char* cur = val;
	for (int i = 0; i < 4; ++i)
	{
		if (*cur < '0' || *cur > '9') { return false; }        // 옥텟은 반드시 숫자로 시작(공백/부호/빈칸 거부)

		char* end = nullptr;
		const unsigned long octet = strtoul(cur, &end, 10);     // base 10 고정 - "010" 을 8진으로 읽지 않는다
		if (end == cur) { return false; }
		if (octet > 255)
		{
			*outReason = L"각 옥텟은 0..255 여야 한다";
			return false;
		}

		addr = (addr << 8) | static_cast<UINT32>(octet);
		cur = end;

		if (i < 3)
		{
			if (*cur != '.') { return false; }                  // 옥텟 사이에는 점이 정확히 하나
			++cur;
		}
	}
	if (*cur != '\0')
	{
		*outReason = L"값 뒤에 남은 문자가 있다";                // "127.0.0.1x" 와 "1.2.3.4.5" 를 여기서 거부
		return false;
	}

	*out = addr;
	return true;
}

void KYS::GAMESERVER::CONFIG::AppendMissingKey(wchar_t* out, size_t cap, const wchar_t* section, const wchar_t* key, bool* sectionOpen)
{
	if (*sectionOpen)
	{
		wcsncat_s(out, cap, L", ", _TRUNCATE);
	}
	else
	{
		if (out[0] != L'\0') { wcsncat_s(out, cap, L" / ", _TRUNCATE); }
		wcsncat_s(out, cap, section, _TRUNCATE);
		wcsncat_s(out, cap, L" ", _TRUNCATE);
		*sectionOpen = true;
	}
	wcsncat_s(out, cap, key, _TRUNCATE);
}
