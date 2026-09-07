#pragma once
#include "Session.h"
#include "SessionPool.h"
#include "../GameServer/Types/Defines.h"

namespace KYS
{
	namespace GAMESERVER
	{
		namespace NETWORK
		{
            // IOCP 기반 TCP 서버 엔진. accept 스레드 1개 + 워커 스레드 풀로 비동기 입출력을 돌린다.
            // 연결을 받을지 판단/오류 보고는 순수가상으로 빼서 게임측이 채운다.
            class IOCPServer
            {
            public:
                IOCPServer();
                virtual ~IOCPServer();

                IOCPServer(const IOCPServer&) = delete;
                IOCPServer& operator=(const IOCPServer&) = delete;
                IOCPServer(IOCPServer&&) = delete;
                IOCPServer& operator=(IOCPServer&&) = delete;

                // 게임측이 채우는 hook (엔진이 호출)
                virtual bool OnConnectionRequest(const sockaddr_in& addr) = 0;   // accept 직후, 이 주소 연결을 받을지(true)/거부할지(false)
                virtual void OnError(ErrorCode code, const wchar_t* msg) = 0;    // 엔진 내부 오류를 게임에 알림

                // 서버 가동/정지
                bool Start(USHORT port,
                    INetEventHandler* netHandler,              // 게임측 통로 (필수)
                    int workerCount = -1,                  // -1 = CPU x 2 자동
                    int acceptPoolSize = 32,                  // AcceptEx 사전 풀
                    int maxSessionCount = 5000);               // 세션 슬롯 풀 크기 (idx는 32bit)
                void Stop();

                // 조회
                // sid 로 내리는 명령·조회 (게임측에 Session* 를 내주지 않는다 - 찾기/세대 검증/lease 는 전부 풀 안에서. 2026-09-02 §409)
                bool   SendTo(UINT64 sid, const BYTE* data, int size, ESendDropPolicy policy) { return m_sessionPool.SendTo(sid, data, size, policy); }   // 큐 적재 = true
                bool   DisconnectTo(UINT64 sid, EDisconnectReason reason) { return m_sessionPool.DisconnectTo(sid, reason); }   // 게임 사유로 끊기. 없는/옛 핸들 = false
                bool   IsSessionAlive(UINT64 sid) { return m_sessionPool.IsSessionAlive(sid); }                                 // 세대 검증만 (IO 없는 생존 확인)
                UINT32 GetSessionRemoteIp(UINT64 sid) { return m_sessionPool.GetSessionRemoteIp(sid); }                         // 접속자 IPv4 값 (로그용) - 없으면 0
                UINT64 GetSendDropEvictTotal() const { return m_sessionPool.GetSendDropEvictTotal(); }                          // 송신 압력 축출 누적 (모니터)
                int GetSessionPoolUsedCount() { return m_sessionPool.GetUsedCount(); }   // 지금 쓰는 소켓 슬롯 수 (서버 모니터의 접속자 표시용)
                int ReapIdleSessions(UINT64 nowTick, UINT64 deadlineMs) { return m_sessionPool.ReapIdleSessions(nowTick, deadlineMs); }   // 무수신 소켓 일괄 회수 (주기 sweep 스레드가 호출)
                int  SnapshotSendMetrics(SessionSendMetrics* out, int maxOut) const { return m_sessionPool.SnapshotSendMetrics(out, maxOut); }   // 세션별 송신 압력 스냅샷 (모니터가 정렬/상위 N/라벨)
                void GetRetiredSendTotals(RetiredSendTotals& out) { m_sessionPool.GetRetiredSendTotals(out); }   // 은퇴 세션 송신 누적 (활성 슬롯 합과 더해 단조 총량 복원)

            private:
                static unsigned __stdcall AcceptThreadProc(void* arg);   // accept 완료 통지를 받아 OnAcceptComplete 호출
                static unsigned __stdcall WorkerThreadProc(void* arg);   // recv/send 완료 통지를 받아 Session에 넘김

                void PostAcceptEx(AcceptContext* ctx);      // ctx로 AcceptEx 한 건 걸어둠 (받을 준비)
                void OnAcceptComplete(AcceptContext* ctx);  // 연결 수락 완료 시 세션 배정 + 첫 수신 등록

                // Start/Stop/OnAcceptComplete에서 분리한 단계 helper
                bool SetupListenSocket(USHORT port);                        // Winsock 초기화 + 리슨 소켓 bind/listen (실패면 false)
                void ShutdownThreads();                                     // 워커/accept 스레드 깨워 종료 + HANDLE 정리
                void CloseListenAndAcceptSockets();                         // pending AcceptEx 소켓 + listen 소켓 닫기
                void ConfigureAcceptedSocket(SOCKET connSock, UINT64 sid);  // 수락 소켓 IOCP 연결 + 소켓옵션(NODELAY)

                SOCKET m_listenSock;                     // 접속을 받는 리슨 소켓
                HANDLE m_acceptCP;                       // 리슨 소켓 전용 완료 통지 포트
                HANDLE m_ioCP;                           // 연결 소켓 전용 완료 통지 포트
                HANDLE m_acceptThread;                   // accept 전담 스레드 1개 (AcceptEx 풀 + accept 완료)
                HANDLE m_workerThreads[64];              // 워커 스레드들 (CPU x 2개, 최대 64까지 고정 배열)

                INetEventHandler* m_netHandler;          // 네트워크 사건을 게임 로직으로 넘기는 통로
                AcceptContext     m_acceptContexts[MAX_ACCEPT_CONTEXT];   // AcceptEx 사전 풀
                int               m_workerCount;         // 실제로 띄운 워커 스레드 수
                SessionPool       m_sessionPool;         // 세션 슬롯 풀 (소켓/버퍼 보관)
            };
		}
	}
}
