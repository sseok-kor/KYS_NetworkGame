#pragma once
#include <WinSock2.h>                          // SOCKET / 소켓 API (헤더 자족 - DummySession.h 패턴)
#include "../../GameServer/Types/Defines.h"    // BYTE / UINT32 / UINT64 / RECV_BUFFER_SIZE
#include "../../GameCommon/Protocol/GamePackets.h"       // IS_TOKEN_VERIFY_RES (응답 파싱 결과 타입)

namespace KYS
{
    namespace SERVERAPP
    {
        // 게임 서버(ServerApp)가 로그인 서버로 거는 인터서버 링크의 raw 소켓 전송 계층.
        //   라이브러리 Session 이 아니라 blocking 소켓을 직접 들고 길이 규약 framing 을 손코딩한다 - 엔진(IOCPServer)은
        //   accept 전용이라 outbound connect 능력이 없기 때문(GameServer.lib 무침해). DummySession 의 raw 소켓 framing +
        //   DBThread 의 blocking 모델을 합친 형태이고, 단일 LoginLinkThread 스레드만 이 객체를 만진다(자체 락 없음).
        //   recv 는 타임아웃(SO_RCVTIMEO)을 둬 로그인 서버 지연이 검증 스레드를 무한정 막지 않게 한다.
        class LoginLinkClient
        {
        public:
            LoginLinkClient();
            ~LoginLinkClient();

            // raw 소켓 보유 - 복사/이동 전면 차단(Rule of Five 4줄, DummySession 정합).
            LoginLinkClient(const LoginLinkClient&) = delete;
            LoginLinkClient& operator=(const LoginLinkClient&) = delete;
            LoginLinkClient(LoginLinkClient&&) = delete;
            LoginLinkClient& operator=(LoginLinkClient&&) = delete;

            // 로그인 서버 인터서버 포트로 연결. ipHostOrder = 호스트 정수 IPv4(예 127.0.0.1=0x7F000001).
            //   recvTimeoutMs = recv/send 타임아웃(응답 지연 시 링크 실패로 판정). 성공 true.
            bool Connect(UINT32 ipHostOrder, USHORT port, int recvTimeoutMs);
            void Close();
            bool IsConnected() const { return m_sock != INVALID_SOCKET; }

            // 링크 핸드셰이크: 공유 비밀과 게임 서버 listen 주소를 등록하고 결과를 받는다.
            bool SendRegister(USHORT serverId, UINT32 authKeyHash, UINT32 listenIp, USHORT listenPort);
            bool RecvRegisterAck(BYTE& outResult);   // IS_REGISTER_ACK.result (1=수락 / 0=거부)

            // 토큰 검증 1왕복: 요청 송신 후 응답 프레임 1개를 blocking 으로 받는다(serial - 동시 in-flight 1개).
            bool SendVerify(UINT64 sid, UINT64 token);
            // 응답 프레임 1개 수신 + opcode 분기. IS_KICK(중복 로그인 축출, RES 직전 동봉)이면 outIsKick=true+계정번호,
            //   IS_TOKEN_VERIFY_RES면 outIsKick=false+검증결과. 둘 다 아니거나 recv 실패면 false(링크 teardown).
            bool RecvVerifyOrKick(IS_TOKEN_VERIFY_RES& outVerify, UINT32& outKickAccountId, bool& outIsKick);

            // 접속 종료 통지(fire-and-forget) - sid 동봉(로그인 서버가 현 holder 검증에 사용). 응답 없음.
            bool SendSessionGone(UINT32 accountId, UINT64 sid);

            // 권위 online 집합 스냅샷 청크 송신(fire-and-forget) - online ghost 수렴. 응답 없음.
            bool SendOnlineSnapshot(const IS_ONLINE_SYNC& chunk);

        private:
            bool SendFrame(const BYTE* data, int size);                      // 한 프레임 전부 송신(부분 송신 이어보냄). 실패 false
            bool RecvFrame(BYTE* outFrame, int outCap, int& outLen);         // 완성 프레임 1개 수신(길이 규약, 부분 수신 누적). 실패/타임아웃 false

            SOCKET m_sock;                          // 인터서버 링크 소켓 (INVALID_SOCKET=미연결)
            BYTE   m_recvBuf[RECV_BUFFER_SIZE];     // recv framing 누적 버퍼 (TCP 스트림 경계 자르기)
            int    m_recvLen;                       // 누적 길이 (이전 RecvFrame 의 잔여 - 다음 프레임 선두)
        };
    }
}
