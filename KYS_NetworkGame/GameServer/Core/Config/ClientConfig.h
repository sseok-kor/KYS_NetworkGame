#pragma once
#include "Types/Defines.h"   // UINT32

// 클라이언트 접속 설정 - conf/Client_Config.ini 의 [login] 섹션. GameClient 가 처음 붙는 로그인 서버 주소다.
//   게임 서버 주소는 여기 없다 - 로그인 서버가 SC_LOGIN_TOKEN 에 실어 내려주고 클라는 그 주소로만 붙는다.
//   Server_Config.ini(DB 비밀번호 / 인터서버 비밀)와 파일을 가른 이유: 클라 배포본에 서버 비밀이 실리면 안 된다.
//   이 파일에는 비밀이 없으므로 견본(Client_Config.sample.ini)을 그대로 복사해 써도 된다.
//   [!] 코드에 주소 기본값은 없다 - 이 파일이 유일한 출처다. 파일이 없거나 값이 깨지면 클라는 뜨지 않는다(조용한 폴백 없음).
struct ClientConfig
{
	UINT32 loginIp;   // [login] host   점표기 a.b.c.d -> host-order. 필수 (0.0.0.0 은 거부)
};

// conf/Client_Config.ini 를 1회 읽어 채운다. 경로 탐색은 서버 로더와 같다(<exe>/conf/ -> cwd/conf/ 순).
//   반환 = 성공 여부. 실패 사유(파일 없음 / 키 없음 / 값 형식)는 콘솔에 [Client_Config] 접두어로 남긴다.
//   실패면 호출측이 종료한다 - 기본값 폴백은 없다.
bool LoadClientConfig(ClientConfig* out);
