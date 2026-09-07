#pragma once
#include <WinSock2.h>

namespace KYS
{
    namespace GAMESERVER
    {
        namespace NETWORK
        {
            enum class EOpType : int   // 이 완료 통지가 무슨 입출력이었는지 (GQCS 후 switch 분기)
            {
                RECV,   // 수신 완료
                SEND    // 송신 완료
            };

            // recv/send 한 건에 따라다니는 overlapped 묶음. Session이 멤버로 영구 보유(풀 안 씀).
            class IOCPContext
            {
            public:
                IOCPContext();
                ~IOCPContext()=default;

                IOCPContext(const IOCPContext&) = delete;
                IOCPContext& operator=(const IOCPContext&) = delete;
                IOCPContext(IOCPContext&&) = delete;
                IOCPContext& operator=(IOCPContext&&) = delete;

                void Reset();  // 다음 입출력에 재사용하려고 overlapped/버퍼 비움

                OVERLAPPED m_overlapped;        // 첫 멤버여야 함 (CONTAINING_RECORD offsetof = 0)
                EOpType    m_opType;             // RECV / SEND - 완료 후 switch 분기에 사용

                WSABUF     m_wsaBuf;             // recv/send가 가리키는 버퍼 (포인터 + 길이)
            };

            // AcceptEx 한 건에 따라다니는 overlapped + 수락 소켓/주소 버퍼. accept 사전 풀에 쓰임.
            class AcceptContext
            {
            public:
                AcceptContext();
                ~AcceptContext()=default;

                AcceptContext(const AcceptContext&) = delete;
                AcceptContext& operator=(const AcceptContext&) = delete;
                AcceptContext(AcceptContext&&) = delete;
                AcceptContext& operator=(AcceptContext&&) = delete;

                void Reset();   // 다음 AcceptEx에 재사용하려고 overlapped/소켓 비움

                OVERLAPPED m_overlapped;        // 첫 멤버여야 함 (CONTAINING_RECORD offsetof = 0)
                SOCKET     m_acceptSock;         // AcceptEx가 채울 새 연결 소켓
                BYTE       m_acceptBuffer[64];   // AcceptEx가 적는 [로컬주소][원격주소] (GetAcceptExSockaddrs가 파싱)
            };
        }
    }
}
