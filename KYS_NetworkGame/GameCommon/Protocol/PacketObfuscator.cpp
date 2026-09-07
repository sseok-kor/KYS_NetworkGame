#include "pch_gamecommon.h"
#include "PacketObfuscator.h"

namespace KYS { namespace GAMECOMMON { namespace PROTOCOL {

    // 방향 분리용 상수 - S2C 시드를 C2S와 다르게 흩뜨려 두 방향 키스트림이 겹치지 않게 한다.
    //   비밀이 아니라 섞기용 (64비트 golden-ratio 상수).
    static const UINT64 DIRECTION_SPLIT = 0x9E3779B97F4A7C15ULL;

    // LCG 상수 - seed = seed * MULT + ADD. 게임서버 실무 rAthena packet_keys와 같은 선형 합동 생성기 계열.
    static const UINT64 LCG_MULT = 6364136223846793005ULL;   // 잘 알려진 64비트 LCG 곱수
    static const UINT64 LCG_ADD  = 1442695040888963407ULL;   // 증분 (홀수) - 전주기 2^64 조건은 홀수 증분 + 곱수 mod 4 == 1 두 가지이며 나머지 조건은 위 MULT 가 만족한다

    // 키를 1회 advance 하고 opcode에 XOR할 상위 16비트 키스트림을 돌려준다.
    //   상위 비트만 쓰는 이유: LCG는 하위 비트 주기가 짧아 품질이 낮다 -> 상위 16비트 채택.
    static UINT16 NextKeystream(ObfKeyState& key)
    {
        key.seed = key.seed * LCG_MULT + LCG_ADD;   // 다음 상태로 진행
        return static_cast<UINT16>(key.seed >> 48); // 상위 16비트
    }

    void InitObfKey(ObfKeyState& key, UINT64 token, ObfDirection dir)
    {
        // C2S 는 token 그대로, S2C 는 상수로 흩뜨린 값 - 두 방향이 독립 키스트림을 갖는다.
        key.seed = (dir == ObfDirection::SERVER_TO_CLIENT) ? (token ^ DIRECTION_SPLIT) : token;
    }

    void ObfuscateOpcode(BYTE* pkt, ObfKeyState& key)
    {
        const UINT16 ks = NextKeystream(key);                       // 이 패킷 키스트림 (키 1회 advance)
        pkt[sizeof(UINT16)]     ^= static_cast<BYTE>(ks >> 8);      // opcode 상위 바이트
        pkt[sizeof(UINT16) + 1] ^= static_cast<BYTE>(ks & 0xFF);   // opcode 하위 바이트
    }

    void DeobfuscateOpcode(BYTE* pkt, ObfKeyState& key)
    {
        // XOR 는 자기역원이라 Obfuscate 와 연산이 같다 - 같은 방향 키로 다시 XOR 하면 원복.
        const UINT16 ks = NextKeystream(key);
        pkt[sizeof(UINT16)]     ^= static_cast<BYTE>(ks >> 8);
        pkt[sizeof(UINT16) + 1] ^= static_cast<BYTE>(ks & 0xFF);
    }

}}}
