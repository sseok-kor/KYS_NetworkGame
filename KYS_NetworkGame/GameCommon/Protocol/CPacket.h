#pragma once
#include "../GameServer/Protocol/SerializationBuffer.h"
#include "../GameServer/Types/Defines.h"

namespace KYS
{
    namespace GAMECOMMON
    {
        namespace PROTOCOL
        {

            // 직렬화 버퍼에 패킷 헤더(길이+종류)를 씌워 한 패킷을 만드는 도우미.
            class CPacket : public KYS::GAMESERVER::PROTOCOL::SerializationBuffer
            {
            public:
                explicit CPacket(int capacity);          // capacity 바이트 버퍼 확보

                // 패킷 틀 만들기
                void   Begin(USHORT type);               // 맨 앞 길이 자리 비우고 종류 기록 (페이로드 쓰기 시작점)
                [[nodiscard]] bool End();               // 다 쓴 뒤 맨 앞 길이 자리에 전체 길이 채움. false = 쓰는 동안 버퍼가 넘쳤다(길이 자리가 0 인 채라 보내면 받는 쪽이 끊는다).
                                                         //   반환값을 버리면 C4834 - 호출부는 반드시 검사하고, false 면 로그를 남기고 그 패킷을 보내지 않는다 (규약의 정본은 이 두 줄).

                // 64비트 값 입출력 (SerializationBuffer는 32비트까지만 지원 -> 상위/하위 32비트로 나눠 BE 기록)
                void   WriteU64(UINT64 value);           // 토큰/sid 같은 64비트 값을 상위->하위 순으로 기록
                void   ReadU64(UINT64& out);             // 상위->하위 순으로 읽어 64비트 복원

                // 조회
                USHORT GetType() const;                  // 이 패킷의 종류

            private:
                USHORT m_type;                           // 이 패킷의 종류 (Begin이 기록, GetType이 반환)
            };

        }
    }
}
