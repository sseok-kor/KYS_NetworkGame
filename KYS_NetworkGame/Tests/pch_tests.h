#pragma once

// 기존 러너가 쓰던 3개 (콘솔 로캘 + 기본 Win32)
// [!] WIN32_LEAN_AND_MEAN: Windows.h가 구식 winsock v1을 끌고 오지 않게 차단.
//     검증 대상 헤더(ObjectPool 등)가 pch_gameserver.h 경유로 <WinSock2.h>를
//     포함하므로, v1이 먼저 들어오면 타입 재정의 충돌이 난다.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <locale.h>

// 스트레스 하니스가 쓰는 것
#include <process.h>        // _beginthreadex (스레드 생성 - std::thread 대신)
#include <vector>           // 스레드 핸들 배열, 보존 집합 버킷
#include <unordered_set>    // unique-id 보존 검사

// 컴파일 계약/경계값 검증
#include <type_traits>      // static_assert 트레이트 (Rule-of-Five/explicit 계약)
#include <climits>          // LLONG_MAX 등 경계값

// GoogleTest (vendored ThirdParty/googletest v1.16.0 - 헤더는 external include로 경고 격리)
#include <gtest/gtest.h>
