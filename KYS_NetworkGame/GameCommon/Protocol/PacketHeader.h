#pragma once
#include "../GameServer/Types/Defines.h"   // UINT16

// 모든 패킷 맨 앞 4바이트 - 받는 쪽이 길이로 경계를 자르고 종류로 갈래를 정함
struct PacketHeader
{
	UINT16 size;   // 패킷 전체 길이 (헤더 4B + 페이로드)
	UINT16 type;   // 패킷 종류 (PacketType 값)
};
