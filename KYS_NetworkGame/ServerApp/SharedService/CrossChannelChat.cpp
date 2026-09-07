#include "pch_serverapp.h"
#include "CrossChannelChat.h"
#include "SharedServicesThread.h"                 // ResolveName (owner 포인터)
#include "../Game/Channel/ChannelManager.h"       // PostToChannel / GetSession
#include "../Game/Session/GameSession.h"                  // GameSession::Send
#include "../../GameCommon/Protocol/CPacket.h"
#include "../../GameCommon/Protocol/PacketType.h"          // PacketType::SC_WHISPER
#include "../../GameCommon/GameDefines.h"         // WHISPER_NAME_MAX / CHAT_MSG_MAX
#include "../../GameServer/Core/Log/Logger.h"   // 직렬화 실패(End false) 파일 로그
#include <cwchar>                                 // wcscpy_s

namespace KYS
{
    namespace SERVERAPP
    {
        CrossChannelChat::CrossChannelChat()
        {
            // 멤버 없음 - Route가 SharedServicesThread::GetInstance() / ChannelManager::GetInstance() 직접 사용
        }

        CrossChannelChat::~CrossChannelChat()
        {
        }

        void CrossChannelChat::Route(const wchar_t* senderName, UINT64 senderSid, const CS_WHISPER& req)
        {
            // (1) 이름 -> sid (디렉터리는 SST가 단독 소유, 이 스레드에서 읽기).
            const UINT64 targetSid = SharedServicesThread::GetInstance().ResolveName(req.targetName);
            if (targetSid == 0)
            {
                // 오프라인/없는 이름 -> 발신자에게 실패 통지 (발신 채널로 route-back, WhisperFailJob 이 소유권 재검증).
                SC_WHISPER_FAIL fail;
                wcscpy_s(fail.targetName, WHISPER_NAME_MAX, req.targetName);
                fail.reason = static_cast<BYTE>(EWhisperFail::OFFLINE);
                ChannelManager::GetInstance().PostToChannel(senderSid, new WhisperFailJob(senderSid, fail));
                return;
            }

            // (2) 전달 패킷 값 구성 (대상 스레드로 넘길 값 스냅샷, 포인터 금지).
            SC_WHISPER packet;
            wcscpy_s(packet.senderName, WHISPER_NAME_MAX, senderName);
            wcscpy_s(packet.message, CHAT_MSG_MAX, req.message);

            // (3) 대상 Channel mailbox에 전달 Job 위임 (그 Channel 스레드가 자기 손으로 Send).
            ChannelManager::GetInstance().PostToChannel(targetSid,
                new WhisperDeliverJob(targetSid, packet));
        }

        // ===== WhisperRouteJob (SST 스레드에서 실행) =====
        WhisperRouteJob::WhisperRouteJob(const wchar_t* senderName, UINT64 senderSid, const CS_WHISPER& req)
            : m_senderSid(senderSid)
            , m_req(req)
        {
            wcscpy_s(m_senderName, WHISPER_NAME_MAX, senderName);   // 값 스냅샷
        }

        void WhisperRouteJob::Execute()
        {
            SharedServicesThread::GetInstance().GetRouter()->Route(m_senderName, m_senderSid, m_req);   // 라우터=SST 값멤버 CrossChannelChat
        }

        // ===== WhisperDeliverJob (대상 Channel 스레드에서 실행) =====
        WhisperDeliverJob::WhisperDeliverJob(UINT64 targetSid, const SC_WHISPER& packet)
            : m_targetSid(targetSid)
            , m_packet(packet)   // 값 스냅샷
        {
        }

        void WhisperDeliverJob::Execute()
        {
            // 재조회 - 전달 직전 로그아웃이면 슬롯 세대 불일치 -> null -> 드롭.
            GameSession* gs = ChannelManager::GetInstance().GetSession(m_targetSid);
            if (gs == nullptr)
            {
                return;   // 대상이 이미 사라짐 (정상 race라 조용히 버림)
            }

            // 소유권 재검증 - 라우팅(PostToChannel resolve)된 이후 대상이 다른 채널로 이관됐으면 drop.
            //   여기서 SendScrambled 하면 옛 채널 스레드가 대상 S2C m_sendKey 를 현 소유 채널과 동시에 advance
            //   -> single-writer 위반 -> rolling opcode keystream desync. 길이 2B 는 평문이라 클라는 패킷 경계를 계속
            //   정확히 자르고 어긋난 opcode 만 조용히 버린다 -> 종료가 아니라 이후 S2C 무증상 먹통(로그 흔적 0). best-effort 귓속말이라
            //   reroute 대신 drop (다음 귓속말은 새 소유 채널로 정상 라우팅). Channel 의 소유권 가드들과 동일 불변식.
            if (gs->GetChannelId() != m_routedChannelId)
            {
#ifdef _DEBUG
                static volatile LONG64 s_b3StaleDrops = 0;
                const LONG64 n = ::InterlockedIncrement64(&s_b3StaleDrops);
                if (n % 50 == 1) { ::wprintf(L"[B3-REPRO] whisper stale-ownership drop count=%lld\n", n); }   // 재현 관측: 위험 조건(옛 채널로 라우팅) 발생 횟수
#endif
                return;
            }

            KYS::GAMECOMMON::PROTOCOL::CPacket whisperPacket(MAX_PACKET_SIZE);
            whisperPacket.Begin(static_cast<USHORT>(PacketType::SC_WHISPER));
            m_packet.Serialize(whisperPacket);   // senderName + message (WriteString)
            if (!whisperPacket.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", whisperPacket.GetType(), whisperPacket.GetSize()); return; }

            gs->SendScrambled(whisperPacket.GetBuffer(), whisperPacket.GetSize());   // 대상 클라에게 귓속말 (channel 스레드 실행 - send 키 단일 writer)
        }

        // ===== WhisperFailJob (발신 Channel 스레드에서 실행) =====
        WhisperFailJob::WhisperFailJob(UINT64 senderSid, const SC_WHISPER_FAIL& packet)
            : m_senderSid(senderSid)
            , m_packet(packet)   // 값 스냅샷
        {
        }

        void WhisperFailJob::Execute()
        {
            // 발신자 재조회 - 실패 통지 직전 로그아웃이면 null -> 드롭.
            GameSession* gs = ChannelManager::GetInstance().GetSession(m_senderSid);
            if (gs == nullptr)
            {
                return;   // 발신자가 이미 사라짐 (정상 race)
            }

            // 소유권 재검증 - 발신자가 route-back 사이 다른 채널로 이관됐으면 drop (WhisperDeliverJob 과 동일 불변식 - 자기 자신에게도 TOCTOU 성립).
            if (gs->GetChannelId() != m_routedChannelId)
            {
                return;
            }

            KYS::GAMECOMMON::PROTOCOL::CPacket failPacket(MAX_PACKET_SIZE);
            failPacket.Begin(static_cast<USHORT>(PacketType::SC_WHISPER_FAIL));
            m_packet.Serialize(failPacket);   // targetName + reason
            if (!failPacket.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", failPacket.GetType(), failPacket.GetSize()); return; }

            gs->SendScrambled(failPacket.GetBuffer(), failPacket.GetSize());   // 발신자에게 실패 통지 (발신 채널 스레드 실행 - send 키 단일 writer)
        }
    }
}
