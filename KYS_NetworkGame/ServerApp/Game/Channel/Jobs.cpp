#include "pch_serverapp.h"
#include "Jobs.h"

void KYS::SERVERAPP::OnChannelEnterJob::Execute()
{
    m_channel->OnChannelEnter(m_sessionId);
}

void KYS::SERVERAPP::OnClientLeaveJob::Execute()
{
    m_channel->OnClientLeave(m_sessionId, m_reason);
}

void KYS::SERVERAPP::OnChannelChangeEnterJob::Execute()
{
    m_channel->OnChannelChangeEnter(m_sessionId);
}


// 받은 바이트를 복사 보관한다 (Channel은 Worker 버퍼가 아닌 복사본만 다룬다).
//   ch   : 대상 채널
//   sid  : 보낸 세션
//   data : 받은 패킷 바이트 (Worker의 m_recvBuffer)
//   size : 바이트 수
KYS::SERVERAPP::OnRecvJob::OnRecvJob(IChannel* ch, UINT64 sid, const BYTE* data, int size)
    : m_channel(ch), m_sessionId(sid), m_packetBytes(nullptr), m_size(size)
{
    m_packetBytes = new BYTE[size];
    memcpy(m_packetBytes, data, size);   // Worker 버퍼에서 복사 (Channel은 복사본만)
}

KYS::SERVERAPP::OnRecvJob::~OnRecvJob()
{
    delete[] m_packetBytes;
    m_packetBytes = nullptr;
}

void KYS::SERVERAPP::OnRecvJob::Execute()
{
    m_channel->OnRecv(m_sessionId, m_packetBytes, m_size);   // 여기서 PacketHeader -> PacketType 분기 (형식 파싱)
}
