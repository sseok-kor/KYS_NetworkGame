#include "pch_gamecommon.h"
#include "CPacket.h"

namespace KYS
{
    namespace GAMECOMMON
    {
        namespace PROTOCOL
        {

            // capacity 바이트 버퍼를 가진 빈 패킷 생성.
            //   capacity : 확보할 버퍼 크기(바이트)
            CPacket::CPacket(int capacity)
                : SerializationBuffer(capacity)
                , m_type(0)
            {
            }

            // 패킷 헤더(길이 2B + 종류 2B)를 깔고 페이로드 쓰기를 준비.
            //   type : 이 패킷의 종류 (PacketType 값)
            void CPacket::Begin(USHORT type)
            {
                Write(static_cast<USHORT>(0));   // 길이 자리 2B를 0으로 비워둠 (End가 나중에 채움)
                Write(type);                     // 종류 2B 기록 (내부에서 네트워크 바이트순서로 변환)
                m_type = type;                   // 호스트 값 보관 (GetType용)
            }

            // 페이로드까지 다 쓴 뒤 맨 앞 길이 자리에 전체 길이를 채운다.
            //   반환 false = 쓰는 동안 버퍼가 넘쳤다(내용이 잘렸다). 호출자가 그 패킷을 버릴지 정한다.
            bool CPacket::End()
            {
                Put(0, static_cast<USHORT>(GetSize()));   // 0번 위치에 현재 전체 길이 덮어쓰기 (쓰기 위치는 안 변함)
                return IsGood();                          // 쓰는 동안 버퍼가 넘쳤으면 false - 이 패킷은 내용이 잘려 있어 보내면 안 된다
            }

            // 이 패킷의 종류를 반환.
            USHORT CPacket::GetType() const
            {
                return m_type;
            }

            // 64비트 값을 상위 32비트 -> 하위 32비트 순으로 기록 (각 32비트는 SerializationBuffer가 네트워크 바이트순서로 변환).
            //   SerializationBuffer가 BYTE/USHORT/UINT/int까지만 operator를 가져서, 64비트는 GameCommon에서 나눠 처리한다 (GameServer.lib 무침해).
            void CPacket::WriteU64(UINT64 value)
            {
                (*this) << static_cast<UINT>(value >> 32);           // 상위 32비트 먼저
                (*this) << static_cast<UINT>(value & 0xFFFFFFFF);    // 하위 32비트
            }

            // WriteU64와 같은 순서(상위 -> 하위)로 읽어 64비트로 합침.
            void CPacket::ReadU64(UINT64& out)
            {
                UINT hi = 0;
                UINT lo = 0;
                (*this) >> hi;
                (*this) >> lo;
                out = (static_cast<UINT64>(hi) << 32) | static_cast<UINT64>(lo);
            }

        }
    }
}
