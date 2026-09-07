#pragma once
#include "../GameServer/Types/Defines.h"

// 네트워크 계층(Session/IOCPServer)이 게임 로직에 사건을 넘기기 위한 통로 추상화.
// 라이브러리는 게임을 모르므로 접속/패킷/종료 세 사건만 던지고, 라우팅과 Job 생성은 게임측 구현이 맡는다.

namespace KYS
{
    namespace GAMESERVER
    {
        namespace NETWORK
        {
            class INetEventHandler
            {
            public:
                virtual ~INetEventHandler() = default;

                INetEventHandler(const INetEventHandler&) = delete;
                INetEventHandler& operator=(const INetEventHandler&) = delete;
                INetEventHandler(INetEventHandler&&) = delete;
                INetEventHandler& operator=(INetEventHandler&&) = delete;

                // accept 직후 1회 호출 - 접속 사실만 알린다. 인증 전이라 구현들은 아직 게임 상태를 만들지 않고, 첫 패킷에서 비로소 검증이 시작된다.
                virtual void EnqueueConnect(UINT64 sid) = 0;
                // 완성된 패킷 한 덩어리가 도착할 때마다 호출 - data/len은 길이 규약으로 잘린 byte 블록(패킷 종류는 라이브러리가 모름).
                virtual void EnqueuePacket(UINT64 sid, const BYTE* data, int len) = 0;
                // 연결이 끊겼을 때 호출 - reason은 끊긴 사유.
                virtual void EnqueueDisconnect(UINT64 sid, EDisconnectReason reason) = 0;

            protected:
                INetEventHandler() = default;
            };
        }
    }
}
