#pragma once
#include "Types/Defines.h"   // UINT64 ($(SolutionDir)GameServer 검색경로)

namespace KYS
{
    namespace LOGINSERVER
    {
        // 고엔트로피 일회용 로그인 토큰 발급기.
        //   인증을 통과한 클라에게 게임 서버 재접속용 증표(UINT64)를 발급한다. 클라가 게임 서버에 제출하면
        //   게임 서버가 로그인 서버에 검증을 질의한다 - 추측 불가해야 하므로 rand() 가 아니라
        //   BCryptGenRandom(시스템 CSPRNG)으로 8바이트를 채운다.
        //   0 은 '토큰 없음' 표식이라 발급하지 않는다(0 이 뽑히면 재시도). 이미 쓰는 토큰과의 충돌 회피는
        //   호출자(TokenStore 에 적재하는 쪽)가 재발급으로 처리한다 - 여기는 난수 생성만 책임진다.
        class TokenIssuer
        {
        public:
            static TokenIssuer& GetInstance() { static TokenIssuer instance; return instance; }   // Meyers static

            TokenIssuer(const TokenIssuer&) = delete;            // 복사 2줄 (단일 소유, 이동은 자동 미선언)
            TokenIssuer& operator=(const TokenIssuer&) = delete;

            UINT64 Make();   // 8바이트 CSPRNG 토큰(0 제외) 발급. CSPRNG 실패 시 0 반환(호출측이 발급 실패로 처리).

        private:
            TokenIssuer() = default;     // 싱글턴 - GetInstance 로만 생성
            ~TokenIssuer() = default;
        };
    }
}
