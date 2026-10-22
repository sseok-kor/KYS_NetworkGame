#pragma once
#include "Types/Defines.h"   // UINT32

#include <cstdio>            // FILE (OpenConfigFile 반환)
#include <cwchar>            // wprintf (ReportError - variadic template 이라 헤더에 몸체가 있다)

// conf/*.ini 로더 둘(ServerConfig / ClientConfig)이 함께 쓰는 도구 - 줄 다듬기, 설정 폴더 탐색, 값 검증, 진단 출력.
//   두 로더는 파일과 섹션만 다르고 "한 줄 = key=value / 형식 오류는 거부 / 기본값 폴백 없음" 규칙이 같다.
//   그 규칙을 지키는 함수를 한 벌만 두어 두 로더가 갈라지지 않게 한다 (한쪽만 고치면 다른 쪽이 낡는다).
//   [!] 이 헤더는 GameServer.lib 안에 있지만 라이브러리 내부 코드는 부르지 않는다 - 호출은 앱(서버 2 + 봇 + 클라)만 한다.
namespace KYS
{
	namespace GAMESERVER
	{
		namespace CONFIG
		{
			const int CONFIG_LINE_MAX = 256;   // 한 줄 최대 바이트(key=value + 여유)

			// 설정 진단을 콘솔에 남긴다. tag 는 "[Server_Config] " 처럼 어느 파일의 진단인지 알리는 접두어 -
			//   부팅 로그에서 설정 관련 줄만 눈으로 걸러낸다. 여러 앱이 함께 쓰므로 특정 앱 로거에 묶지 않고 wprintf 를 쓴다.
			template <typename... Args>
			void ReportError(const wchar_t* tag, const wchar_t* fmt, Args... args)
			{
				wprintf(L"%s", tag);
				wprintf(fmt, args...);
				wprintf(L"\n");
			}

			// 문자열 앞뒤 공백/개행 제거 후 시작 포인터 반환(끝은 널 삽입). in-place.
			char* Trim(char* s);

			// 파일 선두 UTF-8 BOM(EF BB BF)을 벗긴다 - 에디터가 붙였을 때 첫 줄 [섹션] 이 형식 오류로 오진되는 것 방지.
			//   첫 줄에만 적용한다. 중간 줄에 나온 BOM 은 진짜 형식 오류다.
			char* SkipUtf8Bom(char* s);

			// (1) <exe 폴더>\conf\<fileName> (2) <작업 폴더>\conf\<fileName> 순으로 찾아 연다.
			//   두 후보 모두 절대경로로 만들어 outTried 에 남긴다(tag 접두어 + 한 줄씩) - 실패 메시지가 "설정이 없다"가 아니라
			//   "여기와 여기를 봤는데 없다"가 되어야 배포/작업폴더 사고를 즉시 진단할 수 있다.
			FILE* OpenConfigFile(const wchar_t* fileName, const wchar_t* tag, wchar_t* outTried, size_t triedCap);

			// 전량 소비 검사 - 값 전체가 숫자여야 한다. "7777x" 같은 부분 파싱을 거부한다.
			//   base = 10 이면 10진만, 0 이면 진법 자동 판별(0x..=16진 / 0..=8진). 포트는 반드시 base 10 으로 부른다.
			bool ParseU32(const char* val, UINT32* out, int base);

			// 점표기 IPv4 -> host-order UINT32. 계약 3가지: (1) 정확히 4옥텟 (2) 각 옥텟 0..255 (3) 문자열 전량 소비.
			//   outReason = 거부 사유 문구. 진단 메시지가 세 계약 중 어느 것을 어겼는지 그대로 말하게 한다.
			bool ParseIpv4(const char* val, UINT32* out, const wchar_t** outReason);

			// 누락 필수 키를 "[database] user, password / [server] public_port" 형태로 이어 붙인다.
			//   sectionOpen = 이 섹션에서 이미 한 키를 적었는가(호출측 지역 변수). 구분자를 ", " 로 할지 " / " 로 할지 가른다.
			void AppendMissingKey(wchar_t* out, size_t cap, const wchar_t* section, const wchar_t* key, bool* sectionOpen);
		}
	}
}
