#include "pch_gameserver.h"
#include "IOCPContext.h"


// overlapped와 버퍼 포인터를 빈 상태로 초기화.
KYS::GAMESERVER::NETWORK::IOCPContext::IOCPContext()

{
	ZeroMemory(&m_overlapped, sizeof(OVERLAPPED));
	m_wsaBuf.buf = nullptr;
	m_wsaBuf.len = 0;
}

// 다음 입출력 전에 overlapped와 버퍼 포인터를 다시 비움.
void KYS::GAMESERVER::NETWORK::IOCPContext::Reset()
{
	ZeroMemory(&m_overlapped, sizeof(OVERLAPPED));
	m_wsaBuf.buf = nullptr;
	m_wsaBuf.len = 0;
}

// 수락 소켓을 INVALID로 두고 overlapped 초기화.
KYS::GAMESERVER::NETWORK::AcceptContext::AcceptContext()
	:m_acceptSock(INVALID_SOCKET)
{
	ZeroMemory(&m_overlapped, sizeof(OVERLAPPED));
}

// 다음 AcceptEx 전에 overlapped 비우고 수락 소켓을 다시 INVALID로.
void KYS::GAMESERVER::NETWORK::AcceptContext::Reset()
{
	ZeroMemory(&m_overlapped, sizeof(OVERLAPPED));
	m_acceptSock = INVALID_SOCKET;
}
