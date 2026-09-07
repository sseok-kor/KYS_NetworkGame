#pragma once
#include "../../GameServer/Core/Thread/IJob.h"
#include "../../GameCommon/Protocol/GamePackets.h"   // CS_WHISPER / SC_WHISPER (귓속말 송수신 패킷)
#include "../../GameCommon/GameDefines.h"   // WHISPER_NAME_MAX

namespace KYS
{
    namespace SERVERAPP
    {
        class SharedServicesThread;   // 전방 선언 (디렉터리 resolve용 owner 포인터)
        class ChannelManager;         // 전방 선언 (대상 Channel mailbox 도달)

        // 귓속말 도메인 라우팅 (SST=인프라와 책임 분리). 라우팅 책임을 실제로 보유한다.
        class CrossChannelChat
        {
        public:
            CrossChannelChat();    // 무인자 (멤버 없음 - Route가 GetInstance 직접 사용)
            ~CrossChannelChat();

            CrossChannelChat(const CrossChannelChat&) = delete;             // 복사 2줄 (이동 자동 미선언)
            CrossChannelChat& operator=(const CrossChannelChat&) = delete;

            // SST 스레드에서 호출 (WhisperRouteJob::Execute): targetName resolve ->
            //   SC_WHISPER 값 구성 -> 대상 Channel mailbox에 전달 Job 위임. 미발견(오프라인)은 발신 채널에 SC_WHISPER_FAIL{OFFLINE} 통지.
            void Route(const wchar_t* senderName, UINT64 senderSid, const CS_WHISPER& req);

        private:
            // 멤버 없음 - Route는 SharedServicesThread::GetInstance().ResolveName / ChannelManager::GetInstance().PostToChannel 직접 사용
        };

        // 라우팅 Job (1) - 발신 Channel -> SST 큐 (값 스냅샷, cross-thread)
        class WhisperRouteJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            WhisperRouteJob(const wchar_t* senderName, UINT64 senderSid, const CS_WHISPER& req);
            ~WhisperRouteJob() override = default;
            void Execute() override;
            // 복사/이동 = IJob base 4줄 =delete 상속 (재선언 안 함)
        private:
            wchar_t           m_senderName[WHISPER_NAME_MAX];   // 값 스냅샷
            UINT64            m_senderSid;                       // 발신자 sid (오프라인 실패 시 SC_WHISPER_FAIL route-back 대상)
            CS_WHISPER        m_req;                            // 값 스냅샷 (targetName + message)
        };

        // 소유권 검증 딜리버리 Job 베이스 - 대상 세션의 현 소유 채널로 라우팅되지만, execute 시점에 대상이 그 채널을
        //   여전히 소유하는지 재검증해야 하는 cross-channel send Job (귓속말 전달/실패통지). PostToChannel 이 resolve 한
        //   채널 id 를 stamp 하고, 파생 Execute 가 gs->GetChannelId() 와 비교해 불일치(이관됨)면 send 를 drop 한다
        //   - 옛 채널 스레드가 대상 S2C m_sendKey 를 현 소유 채널과 동시에 advance(single-writer 위반->keystream desync)하는 것을 봉인.
        class OwnedDeliveryJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            OwnedDeliveryJob() : m_routedChannelId(-1) {}   // -1 = INVALID_CHANNEL_ID (PostToChannel 이 enqueue 전 실제 채널로 stamp)
            ~OwnedDeliveryJob() override = default;
            void SetRoutedChannel(int chId) { m_routedChannelId = chId; }   // PostToChannel 이 resolve 한 채널을 박음
            // 복사/이동 = IJob base 4줄 =delete 상속
        protected:
            int m_routedChannelId;   // 이 Job 이 라우팅(enqueue)된 채널. Execute 에서 대상 현 소유와 비교해 stale 이면 drop.
        };

        // 라우팅 Job (2) - SST -> 대상 Channel mailbox (값 스냅샷)
        class WhisperDeliverJob : public OwnedDeliveryJob
        {
        public:
            WhisperDeliverJob(UINT64 targetSid, const SC_WHISPER& packet);
            ~WhisperDeliverJob() override = default;
            void Execute() override;   // GetSession 재조회 -> 소유권 재검증 -> CPacket 직렬화 -> Send
        private:
            UINT64          m_targetSid;
            SC_WHISPER      m_packet;   // 값 스냅샷 - 대상 스레드에서 직렬화
        };

        // 라우팅 Job (3) - SST -> 발신 Channel mailbox: 오프라인 대상 귓속말 실패를 발신자에게 통지 (WhisperDeliverJob 거울상)
        class WhisperFailJob : public OwnedDeliveryJob
        {
        public:
            WhisperFailJob(UINT64 senderSid, const SC_WHISPER_FAIL& packet);
            ~WhisperFailJob() override = default;
            void Execute() override;   // GetSession(발신자) 재조회 -> 소유권 재검증 -> SC_WHISPER_FAIL Send
        private:
            UINT64            m_senderSid;
            SC_WHISPER_FAIL   m_packet;   // 값 스냅샷 (targetName + reason)
        };
    }
}
