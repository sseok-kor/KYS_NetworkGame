#include "pch_gameserver.h"
#include "IOCPServer.h"

#include <MSWSock.h>
#include <mstcpip.h>
#pragma comment(lib,"mswsock.lib")


// Winsock 초기화 + 리슨 소켓을 만들어 bind/listen까지 한다. 실패하면 false.
//   port : 받을 포트 번호
bool KYS::GAMESERVER::NETWORK::IOCPServer::SetupListenSocket(USHORT port)
{
	WSADATA wsaData;

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		return false;
	}

	m_listenSock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

	if (m_listenSock == INVALID_SOCKET)
	{
		return false;
	}
	sockaddr_in addr = { AF_INET
						,htons(port)
						,{INADDR_ANY} };
	if (bind(m_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
	{
		return false;
	}

	if (listen(m_listenSock, SOMAXCONN) == SOCKET_ERROR)
	{
		return false;
	}

	return true;
}

// workerCount가 -1이면 자동으로 CPU 코어 수 x 2로 정한다. 그 외엔 그대로 둔다.
static int ResolveWorkerCount(int workerCount)
{
	if (workerCount == -1)
	{
		SYSTEM_INFO si;
		GetNativeSystemInfo(&si);
		return static_cast<int>(si.dwNumberOfProcessors) * 2;
	}
	return workerCount;
}

// 모든 워커/accept 스레드를 깨워 종료시키고, 종료를 기다린 뒤 스레드 HANDLE을 닫는다.
void KYS::GAMESERVER::NETWORK::IOCPServer::ShutdownThreads()
{
	// 모든 Worker, Accept 스레드를 nullptr completion으로 깨움
	// (각 ThreadProc의 GQCS가 pOverlapped==nullptr이면 break)
	for (int i = 0; i < m_workerCount; ++i)
	{
		PostQueuedCompletionStatus(m_ioCP, 0, 0, nullptr);
	}
	PostQueuedCompletionStatus(m_acceptCP, 0, 0, nullptr);


	// 스레드 종료 대기
	if (m_workerCount > 0)
	{
		WaitForMultipleObjects(m_workerCount, m_workerThreads, TRUE, INFINITE);  // m_workerCount <= 64 = MAXIMUM_WAIT_OBJECTS
	}
	WaitForSingleObject(m_acceptThread, INFINITE);

	// HANDLE 정리
	for (int i = 0; i < m_workerCount; ++i)
	{
		CloseHandle(m_workerThreads[i]); m_workerThreads[i] = nullptr;
	}
	CloseHandle(m_acceptThread); m_acceptThread = nullptr;
}

// 완료가 안 온 pending AcceptEx 소켓들과 listen 소켓을 닫는다.
void KYS::GAMESERVER::NETWORK::IOCPServer::CloseListenAndAcceptSockets()
{
	// pending AcceptEx 소켓(완료 안 온 사전 풀) + listen 소켓 정리
	for (int i = 0; i < MAX_ACCEPT_CONTEXT; ++i)
	{
		if (m_acceptContexts[i].m_acceptSock != INVALID_SOCKET)
		{
			closesocket(m_acceptContexts[i].m_acceptSock);
			m_acceptContexts[i].m_acceptSock = INVALID_SOCKET;
		}
	}
	closesocket(m_listenSock); m_listenSock = INVALID_SOCKET;
}

// AcceptEx 버퍼에서 상대(remote) 주소를 뽑아낸다. 반환 포인터는 ctx 버퍼 안을 가리킨다.
static sockaddr_in* ParseRemoteAddress(KYS::GAMESERVER::NETWORK::AcceptContext* ctx)
{
	sockaddr_in* localAddr = nullptr;
	sockaddr_in* remoteAddr = nullptr;
	int localLen = 0;
	int	remoteLen = 0;
	GetAcceptExSockaddrs(ctx->m_acceptBuffer, 0,
		sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16,
		reinterpret_cast<sockaddr**>(&localAddr), &localLen,
		reinterpret_cast<sockaddr**>(&remoteAddr), &remoteLen);
	return remoteAddr;
}

// 수락된 연결 소켓을 입출력 포트에 연결하고 소켓 옵션(SO_UPDATE_ACCEPT_CONTEXT/TCP_NODELAY)을 건다.
//   connSock : 방금 수락된 연결 소켓
//   sid      : 이 연결에 배정된 세션 핸들 (completionKey로 실어 보냄)
void KYS::GAMESERVER::NETWORK::IOCPServer::ConfigureAcceptedSocket(SOCKET connSock, UINT64 sid)
{
	CreateIoCompletionPort(reinterpret_cast<HANDLE>(connSock),m_ioCP, static_cast<ULONG_PTR>(sid), 0);   // completionKey=sid(idx<<32|gen): 완료가 incarnation 정보를 운반 (x64 ULONG_PTR 무손실)
	setsockopt(connSock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
		reinterpret_cast<char*>(&m_listenSock), sizeof(m_listenSock));

	// TCP_NODELAY - Nagle 비활성화 (작은 broadcast 패킷 즉시 전송, 게임서버 보편표준. 앱이 RingBuffer per-session 배칭으로 송신횟수 관리).
	BOOL nodelay = TRUE;
	setsockopt(connSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nodelay), sizeof(nodelay));

	// 연결 생존 감지는 게임 컨텐츠 로직(app-level CS_PING heartbeat + 채널 idle sweep)이 담당한다.
	// OS TCP keepalive는 회선 생존만 보고 게임 프로세스 freeze를 못 잡으며 NAT가 probe를 걸러 게임서버 실무에서 app-level로 대체.
}

// 멤버를 빈/기본 상태로 초기화. 실제 가동은 Start에서.
KYS::GAMESERVER::NETWORK::IOCPServer::IOCPServer()
	: m_listenSock(INVALID_SOCKET)
	, m_acceptCP(nullptr)
	, m_ioCP(nullptr)
	, m_acceptThread(nullptr)
	, m_netHandler(nullptr)
	, m_workerCount(0)
	, m_sessionPool()

{
	ZeroMemory(m_workerThreads, sizeof(m_workerThreads));
}

// 소멸 시 안전하게 정리 (Stop 재호출은 무해).
KYS::GAMESERVER::NETWORK::IOCPServer::~IOCPServer()
{
	Stop();
}

// 서버 가동: Winsock 준비 -> 리슨 소켓 -> 완료 통지 포트 2개 -> 스레드 기동 -> AcceptEx 사전 등록.
//   port            : 받을 포트 번호
//   netHandler      : 네트워크 사건을 넘길 게임측 통로 (필수)
//   workerCount     : 워커 스레드 수 (-1이면 CPU x 2)
//   acceptPoolSize  : 미리 걸어둘 AcceptEx 개수
//   maxSessionCount : 세션 슬롯 풀 크기
bool KYS::GAMESERVER::NETWORK::IOCPServer::Start(USHORT port, INetEventHandler* netHandler
, int workerCount, int acceptPoolSize, int maxSessionCount)
{
	_ASSERTE(netHandler != nullptr);
	_ASSERTE(acceptPoolSize <= MAX_ACCEPT_CONTEXT);
	m_netHandler = netHandler;

	// Winsock 초기화 + 리슨 소켓 만들어 bind/listen 시작
	if (!SetupListenSocket(port))
	{
		return false;
	}

	// 완료 통지 포트 생성 시작 (accept용 1개 + 입출력용 1개, 워커 수만큼 동시성 허용)
	m_acceptCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE,NULL,0,1);
	workerCount = ResolveWorkerCount(workerCount);
	m_ioCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, workerCount);

	if (m_acceptCP == nullptr || m_ioCP == nullptr)
	{
		return false;
	}

	CreateIoCompletionPort((HANDLE)m_listenSock, m_acceptCP, 0, 0);   // 리슨 소켓을 accept용 포트에 연결

	// 세션 풀 준비 시작
	if (!m_sessionPool.Init(maxSessionCount,m_netHandler))
	{
		return false;
	}

	m_workerCount = workerCount;

	// 스레드 기동 + AcceptEx 사전 풀 등록 시작
	m_acceptThread = (HANDLE)_beginthreadex(NULL, 0, AcceptThreadProc, this, 0, NULL);
	for (int i = 0; i < workerCount; i++)
	{
		m_workerThreads[i] = (HANDLE)_beginthreadex(NULL, 0, WorkerThreadProc, this, 0, NULL);
	}

	for (int i = 0; i < acceptPoolSize; i++)
	{
		PostAcceptEx(&m_acceptContexts[i]);
	}

	return true;

}

// 서버 정지: 스레드 깨워 종료 -> HANDLE 정리 -> 남은 AcceptEx 소켓/리슨 소켓 정리 -> 포트/Winsock 정리.
void KYS::GAMESERVER::NETWORK::IOCPServer::Stop()
{
	if (m_listenSock == INVALID_SOCKET)
	{
		return;            // 이미 Stop됨
	}

	ShutdownThreads();

	CloseListenAndAcceptSockets();

	// 완료 통지 포트 + Winsock 정리
	CloseHandle(m_acceptCP); m_acceptCP = nullptr;
	CloseHandle(m_ioCP);     m_ioCP = nullptr;
	WSACleanup();
	// in-flight 연결 소켓은 Session::Disconnect/Reset이 닫음(또는 CP 종료로 정리) - Stop은 listen, accept 풀, 포트, Winsock 담당.
}

// accept 전담 스레드 루프. accept 완료 통지를 꺼내 OnAcceptComplete로 넘긴다 (종료 신호 받으면 빠져나감).
//   arg : IOCPServer* (this)
unsigned __stdcall KYS::GAMESERVER::NETWORK::IOCPServer::AcceptThreadProc(void* arg)
{
	IOCPServer* self = reinterpret_cast<IOCPServer*>(arg);
	while (true)
	{
		DWORD       transferred = 0;
		ULONG_PTR   completionKey = 0;
		OVERLAPPED* pOverlapped = nullptr;

		GetQueuedCompletionStatus(
			self->m_acceptCP, &transferred, &completionKey, &pOverlapped, INFINITE);

		if (pOverlapped == nullptr)
		{
			break;   // 종료 신호 (Stop이 PostQueuedCompletionStatus(m_acceptCP, 0, 0, nullptr))
		}

		// m_acceptCP엔 AcceptEx 완료(AcceptContext)만 도착 -> AcceptContext로 복원.
		AcceptContext* ctx = CONTAINING_RECORD(pOverlapped, AcceptContext, m_overlapped);
		self->OnAcceptComplete(ctx);
	}
	return 0;
}

// 워커 스레드 루프. recv/send 완료 통지를 꺼내 살아있는 세션에만 넘긴다 (종료 신호 받으면 빠져나감).
//   arg : IOCPServer* (this)
unsigned __stdcall KYS::GAMESERVER::NETWORK::IOCPServer::WorkerThreadProc(void* arg)
{
	IOCPServer* self = reinterpret_cast<IOCPServer*>(arg);
	while (true)
	{
		DWORD       transferred = 0;
		ULONG_PTR   completionKey = 0;
		OVERLAPPED* pOverlapped = nullptr;

		BOOL ok = GetQueuedCompletionStatus(
			self->m_ioCP, &transferred, &completionKey, &pOverlapped, INFINITE);

		if (pOverlapped == nullptr)
		{
			break;   // 종료 신호 (Stop이 PostQueuedCompletionStatus(m_ioCP, 0, 0, nullptr))
		}

		// completion key에 sid(idx<<32|gen)가 실려 온다. 슬롯이 재사용된 뒤에도 옛 연결의 늦은 완료 통지가
		// 같은 슬롯에 도착할 수 있어서, generation을 비교해 지금 살아있는 연결의 완료만 처리한다.
		UINT64 sid = static_cast<UINT64>(completionKey);
		Session* s = self->m_sessionPool.FindLockFree(sid);
		if (s == nullptr)
		{
			continue;   // 이미 회수된 옛 슬롯에 뒤늦게 온 완료 통지 - OnXxxComplete와 dec를 통째로 건너뜀
			            // (Reset이 진행중 입출력 카운트(m_pendingIoCount)를 0으로 정산했으니 갚을 빚 없음. dec하면 재사용된 슬롯만 오염)
		}

		// OVERLAPPED* -> IOCPContext 로 되돌림 (overlapped가 첫 멤버라 가능). m_opType으로 분기.
		IOCPContext* ctx = CONTAINING_RECORD(pOverlapped, IOCPContext, m_overlapped);

		// ok==FALSE여도 (pOverlapped!=nullptr) 완료 통지는 온 것 - 각 메서드가 카운트를 정산
		switch (ctx->m_opType)
		{
		case EOpType::RECV:
			s->OnRecvComplete(static_cast<int>(transferred), ok);  // 운전은 Session
			break;
		case EOpType::SEND:
			s->OnSendComplete(static_cast<int>(transferred), ok);
			break;
		default:
			_ASSERTE(false);   // ACCEPT는 m_acceptCP로 따로 처리됨 - 여기로 오면 버그
			break;
		}


		s->DecPendingIo();   // 진행중 입출력 카운트 -1. 모든 감소가 여기로 모임. 0이 되면 Session이 자가 통지 + 풀 반납
	}
	return 0;
}

// ctx 하나로 AcceptEx를 한 건 걸어둔다 (받을 준비). 완료되면 m_acceptCP로 통지된다.
//   ctx : 재사용할 accept 슬롯
void KYS::GAMESERVER::NETWORK::IOCPServer::PostAcceptEx(AcceptContext* ctx)
{
	// 게시 실패 시 이 accept 슬롯이 사전 풀에서 영구 이탈하면 시간이 지나며 accept 동시성이 0으로 줄어든다.
	//   전이적 자원 부족(소켓 핸들 일시 고갈 등)은 짧게 재시도하면 회복되고, 지속 실패는 OnError로 표면화한다.
	const int MAX_POST_ATTEMPTS = 3;
	for (int attempt = 0; attempt < MAX_POST_ATTEMPTS; ++attempt)
	{
		ctx->Reset();   // m_overlapped ZeroMemory + m_acceptSock = INVALID_SOCKET

		// AcceptEx는 사전 풀 이용
		ctx->m_acceptSock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,
			nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (ctx->m_acceptSock == INVALID_SOCKET)
		{
			OnError(ErrorCode::SOCKET_CREATE_FAILED, L"AcceptEx 대기 소켓 생성 실패 - 재시도");
			Sleep(1);
			continue;
		}

		DWORD bytesReceived = 0;   // overlapped(ERROR_IO_PENDING) 시 미갱신이나 유효 포인터는 필요
		// dwReceiveDataLength=0 -> 수락만(데이터 0 byte 대기, 첫 데이터는 OnAcceptComplete , RegisterRecv가 받음).
		// 주소 길이 = sizeof(sockaddr_in)+16 each -> m_acceptBuffer[64]에 [local][remote] (OnAcceptComplete의 GetAcceptExSockaddrs와 동일해야 파싱)
		BOOL ok = AcceptEx(m_listenSock, ctx->m_acceptSock,
			ctx->m_acceptBuffer, 0,
			sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16,
			&bytesReceived, &ctx->m_overlapped);

		if (ok || WSAGetLastError() == ERROR_IO_PENDING)
		{
			return;   // 게시 성공 - 완료 시 m_acceptCP에 통지 -> AcceptThreadProc -> OnAcceptComplete
		}

		// AcceptEx 즉시 실패 - 완료 통지가 안 오므로 이 소켓을 직접 정리(누수 방지)하고 재시도
		closesocket(ctx->m_acceptSock);
		ctx->m_acceptSock = INVALID_SOCKET;
		OnError(ErrorCode::FAILED, L"AcceptEx 게시 즉시 실패 - 재시도");
		Sleep(1);
	}
	// 재시도 소진 = 지속적 자원 고갈. 이 슬롯은 완료 통지가 오지 않아 재발행 경로(OnAcceptComplete)에 닿지 못하므로
	//   프로세스가 사는 동안 사전 풀에서 영구 이탈한다 - accept 동시성이 그만큼 영구히 줄어든다.
	//   여기서는 OnError로 표면화만 하고, 이후 수용은 남은 정상 슬롯들이 담당한다.
	OnError(ErrorCode::SOCKET_CREATE_FAILED, L"AcceptEx 게시 재시도 소진 - accept 용량 일시 감소");
}

// 연결 수락 한 건 마무리: 주소 파싱 -> 거부 판단 -> 세션 배정 -> 포트 연결/소켓 옵션 -> 첫 수신 등록 -> ctx 재투입.
//   ctx : 방금 AcceptEx가 완료된 accept 슬롯
void KYS::GAMESERVER::NETWORK::IOCPServer::OnAcceptComplete(AcceptContext* ctx)
{
	SOCKET connSock = ctx->m_acceptSock;

	sockaddr_in* remoteAddr = ParseRemoteAddress(ctx);

	//  거부/허용 (Session 만들기 전 - 거부 시 회수 0)
	if (!OnConnectionRequest(*remoteAddr))
	{
		closesocket(connSock);
		PostAcceptEx(ctx);          // 슬롯 재사용
		return;
	}

	UINT64 sid = m_sessionPool.Allocate();
	if (sid == 0)
	{
		closesocket(connSock);
		PostAcceptEx(ctx);
		return;
	}

	Session* s = m_sessionPool.Find(sid);
	s->SetSocket(connSock);
	s->SetRemoteIp((UINT32)remoteAddr->sin_addr.S_un.S_addr);   // 접속자 IPv4 저장 (진단/로깅용 전송 사실, network byte order)
	ConfigureAcceptedSocket(connSock, sid);

	// 첫 수신 등록
	if (!s->BeginSession())
	{
		m_sessionPool.Release(sid);
	}

	// AcceptContext 재투입 (풀 재사용)
	PostAcceptEx(ctx);
}
