#pragma once

// MySQL 접속 설정 - conf/Server_Config.ini 의 [database] 섹션에서 부팅 시 1회 적재한다.
//   값이 char(UTF-8)인 이유: mysql_real_connect 가 char* 를 받는 C API 경계라 여기만 예외적으로 char.
//   (host/user/password/dbname 은 ASCII. 게임 도메인 문자열은 여전히 wchar_t.)
//   비밀번호를 들고 있으므로 실제 Server_Config.ini 는 .gitignore - 견본만 커밋(Server_Config.sample.ini).
struct DbConfig
{
	char host[64];        // [database] host      예: 127.0.0.1  (키 없으면 127.0.0.1 / 값이 있는데 파손이면 부팅 거부)
	char user[64];        // [database] user      예: kys_app    (필수)
	char password[128];   // [database] password  예: AppUserStrongPw!  (필수)
	char dbname[64];      // [database] dbname    예: kys_game   (필수)
	int  port;            // [database] port      예: 3306       (키 없으면 3306 / 값이 있는데 숫자가 아니면 부팅 거부)
};
