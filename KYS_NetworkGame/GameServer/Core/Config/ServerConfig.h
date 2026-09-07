#pragma once
#include "Types/Defines.h"          // UINT32 / USHORT (기본 타입)
#include "Core/Config/DbConfig.h"   // DbConfig (로더가 세 구조체를 함께 채운다)

// 서버 운영 설정 - conf/Server_Config.ini 의 [server] 섹션. 게임 서버가 외부에 여는 주소 한 쌍이다.
//   ServerApp 이 이 포트로 listen 하고, LoginServer 가 이 주소를 클라에게 광고하고,
//   DummyClient 봇이 이 포트로 접속하는 "같은 한 주소"다.
struct ServerConfig
{
	UINT32 publicIp;      // [server] public_ip   점표기 a.b.c.d -> host-order. 필수
	USHORT publicPort;    // [server] public_port 1..65535. 필수
};

// 인터서버 링크 설정 - conf/Server_Config.ini 의 [interserver] 섹션.
//   LoginServer <-> ServerApp 전용 링크(7002)의 인증 재료. 클라는 이 값을 영원히 보지 않는다.
//   앞으로 링크 포트/타임아웃/백오프가 설정으로 나오면 이 구조체가 그 자리다.
struct InterServerConfig
{
	UINT32 secret;        // [interserver] secret (16진 0x.. 또는 10진). 필수 non-zero - 0 이면 인터서버 인증 fail-open
};

// conf/Server_Config.ini 를 1회 읽어 세 구조체를 채운다. 파일 경로는 내부 고정(<exe>/conf/ -> cwd/conf/ 순 탐색).
//   [!] 이 함수 하나가 세 구조체를 모두 채운다 - 파일을 한 번만 열기 위해서다. 파일명이 ServerConfig 인 것은
//       "서버 설정 전체"라는 뜻이고, DbConfig.h 는 DB 계층이 이 헤더 없이 쓰라고 분리해 둔 것이다.
//   nullptr 을 넘긴 구조체는 그 섹션을 읽지도 검증하지도 않는다(그 프로세스가 안 쓰는 섹션).
//     - DummyClient 는 MySQL 도 인터서버 링크도 쓰지 않으므로 LoadServerConfig(nullptr, &serverConfig, nullptr).
//   반환 = 성공 여부. 실패 사유(어느 섹션의 어느 키가 없는지 / 어느 값의 형식이 틀린지)는 콘솔에 남긴다.
//   실패면 호출측이 fail-fast(부팅 중단) - 기본값 폴백은 없다.
bool LoadServerConfig(DbConfig* outDb, ServerConfig* outServer, InterServerConfig* outInter);
