#pragma once
// GameServer.lib 미리 컴파일 헤더 (모든 .cpp 첫 줄에 포함, 공통 헤더를 한 번만 컴파일)

#define WIN32_LEAN_AND_MEAN  // 잘 안 쓰는 Windows 헤더를 빼서 컴파일 가볍게
#include <WinSock2.h>        // 소켓 API
#include <Windows.h>         // Win32 API

#include <stdio.h>           // C 표준 입출력
#include <process.h>         // _beginthreadex (스레드 생성)

#include <new>               // placement new

#include "Types/Defines.h"   // 기본 타입/네트워크 상수/에러 코드


