#pragma once
#include "../GameServer/Types/Defines.h"   // BYTE, UINT16, UINT64

namespace KYS { namespace GAMECOMMON { namespace PROTOCOL {

    // 송/수신 방향. 방향별로 시드를 흩뜨려 두 방향 키스트림이 겹치지 않게 한다.
    enum class ObfDirection { CLIENT_TO_SERVER, SERVER_TO_CLIENT };

    // 한 방향의 rolling 키 상태 - LCG 시드 하나. 매 패킷 advance 되며 opcode 2B를 뒤섞는다.
    struct ObfKeyState
    {
        UINT64 seed;   // LCG 상태 (InitObfKey가 심고, Obfuscate/Deobfuscate가 매 패킷 갱신)
    };

    // token으로 방향별 시드를 심는다 (양끝이 같은 token + 같은 방향이면 같은 시드 = 대칭).
    void InitObfKey(ObfKeyState& key, UINT64 token, ObfDirection dir);

    // pkt의 opcode 2바이트(offset = sizeof(UINT16))를 키스트림과 XOR + 키 1회 advance. 송신 직전 호출.
    void ObfuscateOpcode(BYTE* pkt, ObfKeyState& key);

    // 같은 XOR 연산(자기역원 대칭) - 수신 직후 호출. 호출부 의도를 이름으로 드러내려 분리.
    void DeobfuscateOpcode(BYTE* pkt, ObfKeyState& key);

}}}
