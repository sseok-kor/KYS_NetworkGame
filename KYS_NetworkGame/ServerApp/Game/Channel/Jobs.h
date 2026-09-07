#pragma once
#include "IChannel.h"
#include "../GameServer/Types/Defines.h"
#include "../GameServer/Core/Thread/IJob.h"


namespace KYS
{
    namespace SERVERAPP
    {
        // 채널 진입 job - Worker가 만들고 Channel 스레드가 Execute (Channel::OnChannelEnter 호출)
        class OnChannelEnterJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            OnChannelEnterJob(IChannel* ch, UINT64 sid)
                : m_channel(ch), m_sessionId(sid)
            {
            }
            void Execute() override;   // m_channel->OnChannelEnter(m_sessionId)
        private:
            IChannel* m_channel;
            UINT64    m_sessionId;
        };

        // 연결 종료 job - Channel::OnClientLeave 호출 (게임 상태 회수 트리거)
        class OnClientLeaveJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            OnClientLeaveJob(IChannel* ch, UINT64 sid, EDisconnectReason reason)
                : m_channel(ch), m_sessionId(sid), m_reason(reason)
            {
            }
            void Execute() override;   // m_channel->OnClientLeave(m_sessionId, m_reason)
        private:
            IChannel* m_channel;
            UINT64            m_sessionId;
            EDisconnectReason m_reason;
        };

        // 인게임 채널 변경 도착 job - 대상 채널 스레드가 Execute (Channel::OnChannelChangeEnter 호출).
        //   출발 채널 스레드가 MigrateChannel 안에서 대상 채널 mailbox로 post (cross-thread 핸드오프). 절대 드롭 금지.
        class OnChannelChangeEnterJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            OnChannelChangeEnterJob(IChannel* ch, UINT64 sid)
                : m_channel(ch), m_sessionId(sid)
            {
            }
            void Execute() override;   // m_channel->OnChannelChangeEnter(m_sessionId)
        private:
            IChannel* m_channel;
            UINT64    m_sessionId;
        };

        // 수신 패킷 job - 받은 바이트를 복사 보관, Execute에서 Channel::OnRecv로 형식 파싱/분기
        class OnRecvJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            OnRecvJob(IChannel* ch, UINT64 sid, const BYTE* data, int size);
            ~OnRecvJob();
            void Execute() override;   // m_channel->OnRecv(...) - PacketType 분기 (Channel 스레드)
            BYTE* PacketBytes() { return m_packetBytes; }   // ctor가 복사한 패킷 바이트 (recv seam이 in-place opcode 복호 대상으로 넘김)
        private:
            IChannel* m_channel;
            UINT64    m_sessionId;
            BYTE* m_packetBytes;
            int       m_size;
        };

    }
}
