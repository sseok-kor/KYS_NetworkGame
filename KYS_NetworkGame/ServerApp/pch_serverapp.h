#pragma once

#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <Windows.h>
#include <timeapi.h>   // timeBeginPeriod/timeEndPeriod (WIN32_LEAN_AND_MEAN 이 mmsystem 을 제외 -> 명시 포함)
#include <WS2tcpip.h>
#include <mysql.h>

#include <stdio.h>
#include <crtdbg.h>

#include <new>

#include "Types/Defines.h"