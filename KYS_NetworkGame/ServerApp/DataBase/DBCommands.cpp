#include "pch_serverapp.h"
#include "DBCommands.h"
#include "IDBProvider.h"                  // m_provider 작업 함수 (Load / SaveFull / 캐릭터 관리)
#include "../Game/Channel/IChannel.h"     // IChannel::EnqueueSessionEvent
#include "../Game/Channel/ChannelManager.h"  // SendRaw (pre-auth raw 소켓 회신) + GetInstance
#include "../../GameCommon/Protocol/CPacket.h"      // 회신 패킷 직렬화
#include "../../GameCommon/Protocol/GamePackets.h"  // SC_CHARACTER_LIST / SC_CHARACTER_CREATE_RESULT / SC_CHARACTER_DELETE_RESULT
#include "../../GameCommon/Protocol/PacketType.h"   // PacketType
#include "../../GameServer/Types/Defines.h"  // MAX_PACKET_SIZE
#include "../GameServer/Core/Log/Logger.h"   // 캐릭터 생성/삭제 이력 + 거래 영속 실패 파일 로그 (DBThread 문맥)
#include <cwchar>                         // wcsncpy_s

namespace KYS
{
    namespace SERVERAPP
    {
        // 계정의 현재 캐릭터 목록을 조회해 SC_CHARACTER_LIST 로 pre-auth raw 소켓에 송신.
        //   목록 화면(LoadCharListCommand)과 생성/삭제 OK 후 재푸시가 공유한다 - 서버가 갱신된 목록을 권위로 내려보내,
        //   클라가 새 캐릭터를 직접 재구성(부정확한 hp/맵 추정)하지 않게 한다.
        static void SendCharacterList(IDBProvider* provider, UINT64 sid, UINT32 accountId)
        {
            CharListResult list{};
            list.sid = sid;
            if (!provider->LoadCharacterList(accountId, list))
            {
                // backend 도달 실패 - 빈/부분 목록을 '캐릭터 없음'으로 권위 송신하면 실재 캐릭터가 있는 계정에
                //   선택지가 사라진다. 송신을 건너뛴다(provider 가 이미 실패 로그). 초기 목록은 클라가 채널 선택
                //   재클릭으로 재요청 가능, 생성/삭제 후 재푸시 실패면 목록이 stale 로 남는다(재접속 시 복구) - 빈 목록 오송신보다 낫다.
                return;
            }

            SC_CHARACTER_LIST body;
            body.count = static_cast<BYTE>(list.count);
            for (int i = 0; i < list.count && i < MAX_CHAR_SLOTS; ++i)
            {
                body.chars[i].charId = list.rows[i].charId;
                body.chars[i].slotId = list.rows[i].slotId;
                wcscpy_s(body.chars[i].name, WHISPER_NAME_MAX, list.rows[i].name);
                body.chars[i].mapId  = list.rows[i].mapId;
                body.chars[i].hp     = list.rows[i].hp;
                body.chars[i].maxHp  = list.rows[i].maxHp;
            }
            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_CHARACTER_LIST));
            body.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            // pre-auth(GameSession 없음) raw 소켓 회신 - SendRaw 가 풀의 SendTo(세대 검증+lease 안에서)로 생명주기 안전(어느 스레드든).
            ChannelManager::GetInstance().SendRaw(sid, out.GetBuffer(), out.GetSize());
        }

        // -- SaveFullCommand --
        void SaveFullCommand::Execute()
        {
            // 어떤 command 인지는 DBThread 가 골랐고, 어떤 provider 인지는 여기서 고른다.
            m_provider->SaveFull(m_snapshot);
        }

        // -- SaveDeltaCommand --
        void SaveDeltaCommand::Execute()
        {
            m_provider->SaveDelta(m_snapshot, m_dirtyMask);
        }

        // -- LoadCharacterCommand --
        LoadCharacterCommand::LoadCharacterCommand(UINT64 sid, int chId, UINT32 charId, UINT32 accountId,
            IChannel* targetChannel)
            : m_sid(sid), m_chId(chId), m_charId(charId), m_accountId(accountId), m_targetChannel(targetChannel)
        {
        }

        void LoadCharacterCommand::Execute()
        {
            // DBThread 스레드: 동기 Load (DB 조회) - 메인 게임 루프는 이 사이 안 멈춘다.
            DBResult result{};                 // 0으로 초기화 (found=false, name=L"", 숫자 0) - 미초기화 값이 응답에 새는 것 방지
            // char_id + account_id 로 로드(소유 검증). backend 도달 실패면 loadFailed, 미발견/타계정이면 found=false.
            //   둘 다 DbResultJob 이 FailLogin 으로 정리(자동생성 폐기 - 캐릭터는 CreateCharacter 로만 생긴다).
            if (!m_provider->Load(m_charId, m_accountId, result))
            {
                result.loadFailed = true;   // backend 도달 실패(MySQL 끊김 등) - found=false(미발견)와 구별
            }
            else if (result.found && result.hp < 1)
            {
                // 손상 방어: 기존 캐릭터인데 hp<1(0/음수=깨진 행)이면 maxHp 로 복구 (spawn-dead 방지).
                result.hp = result.maxHp;
            }

            // 인벤토리를 같은 왕복에서 이어서 로드(원자 로드) - 캐릭터가 정상 로드된 경우만.
            //   캐릭터 실패/미발견이면 DbResultJob 이 attach 전에 FailLogin 하므로 인벤 로드 불요.
            if (!result.loadFailed && result.found)
            {
                if (!m_provider->LoadInventory(m_charId, result.inventory))
                {
                    result.inventory.loadFailed = true;   // 인벤 backend 도달 실패 - 빈 인벤 입장은 아이템 소실 위험이라 DbResultJob 이 세션 정리
                }
            }

            // 회신 라우팅 정보 부착 (Load 는 캐릭터 데이터만 채움 - sid/chId 는 요청자가 안다).
            result.sid = m_sid;
            result.chId = m_chId;

            // 대상 Channel mailbox 로 회신 (DbResultJob 에 담아서). 값(result) 통째 복사 전달 (캐릭터 + 동봉 인벤).
            //   실제 적용(GetSession 재확인 -> AttachPlayer -> 캐릭터/인벤 채움 -> SC_ENTER_WORLD/SC_INVENTORY -> spawn, 또는 실패 시 FailLogin)은 Channel 스레드.
            m_targetChannel->EnqueueSessionEvent(new DbResultJob(result));   // 로그인이 멈추지 않도록 절대 드롭 금지
        }

        // -- LoadCharListCommand -- (캐릭터 선택 화면용 목록)
        void LoadCharListCommand::Execute()
        {
            SendCharacterList(m_provider, m_sid, m_accountId);
        }

        // -- CreateCharCommand --
        CreateCharCommand::CreateCharCommand(UINT64 sid, UINT32 accountId, const wchar_t* name, BYTE slotId)
            : m_sid(sid), m_accountId(accountId), m_slotId(slotId)
        {
            // 이름 값 복사 - 과길이 입력에도 안전하게 절단(wcscpy_s 비절단 크래시 회피, 기능 A 교훈).
            if (name == nullptr) { m_name[0] = L'\0'; }
            else { wcsncpy_s(m_name, WHISPER_NAME_MAX, name, _TRUNCATE); }
        }

        void CreateCharCommand::Execute()
        {
            UINT32 newCharId = 0;
            BYTE result = m_provider->CreateCharacter(m_accountId, m_name, m_slotId, newCharId);

            SC_CHARACTER_CREATE_RESULT body;
            body.result = result;       // ECharCreateResult 값
            body.charId = newCharId;    // OK 일 때만 유효
            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_CHARACTER_CREATE_RESULT));
            body.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            ChannelManager::GetInstance().SendRaw(m_sid, out.GetBuffer(), out.GetSize());

            // 캐릭터 생성 이력 - 결과가 판명되는 여기(DBThread)서 남겨야 실패한 생성이 "생성됨"으로 오염되지 않는다.
            if (result == static_cast<BYTE>(ECharCreateResult::OK))
            {
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::USER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"char",
                    L"캐릭터 생성 account=%u name=%s charId=%u slot=%u", m_accountId, m_name, newCharId, m_slotId);
            }
            else
            {
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::USER, KYS::GAMESERVER::LOG::LogLevel::LL_WARN, L"char",
                    L"캐릭터 생성 실패 account=%u name=%s result=%u", m_accountId, m_name, result);
            }

            // 성공 시 갱신된 목록을 서버 권위로 재푸시 - 클라가 새 캐릭터를 직접 재구성하지 않고 선택 화면에서 그대로 본다.
            if (result == static_cast<BYTE>(ECharCreateResult::OK))
                SendCharacterList(m_provider, m_sid, m_accountId);
        }

        // -- DeleteCharCommand --
        void DeleteCharCommand::Execute()
        {
            BYTE result = m_provider->DeleteCharacter(m_accountId, m_charId);

            SC_CHARACTER_DELETE_RESULT body;
            body.result = result;       // ECharDeleteResult 값
            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_CHARACTER_DELETE_RESULT));
            body.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            ChannelManager::GetInstance().SendRaw(m_sid, out.GetBuffer(), out.GetSize());

            // 캐릭터 삭제 이력 - 생성과 대칭으로 기록 (L2J의 "생성만 기록, 삭제 무기록" 비대칭 반면교사).
            if (result == static_cast<BYTE>(ECharDeleteResult::OK))
            {
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::USER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"char",
                    L"캐릭터 삭제 account=%u charId=%u", m_accountId, m_charId);
            }
            else
            {
                // NOT_OWNED 는 위조 요청 신호가 기본이나, 커밋 응답 유실 후 재실행(이미 삭제됨)도 같은 코드로
                //   돌아온다 - 직전 SERVER 로그의 "COMMIT 실패"와 인접하면 후자다(운영 판독 각주).
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::USER, KYS::GAMESERVER::LOG::LogLevel::LL_WARN, L"char",
                    L"캐릭터 삭제 실패 account=%u charId=%u result=%u", m_accountId, m_charId, result);
            }

            // 성공 시 갱신된 목록을 재푸시 - 삭제된 슬롯이 선택 화면에서 즉시 비워진다.
            //   NOT_OWNED 도 재푸시 - 위조 요청이면 목록이 그대로라 무해하고, 커밋 응답 유실 후 재실행이
            //   NOT_OWNED 로 돌아온 경우(이미 삭제됨)엔 죽은 캐릭터가 선택 화면에 남는 것을 권위 목록이 정리한다.
            if (result == static_cast<BYTE>(ECharDeleteResult::OK)
                || result == static_cast<BYTE>(ECharDeleteResult::NOT_OWNED))
                SendCharacterList(m_provider, m_sid, m_accountId);
        }

        // -- SaveInventoryCommand --
        void SaveInventoryCommand::Execute()
        {
            const bool committed = m_provider->SaveInventory(m_snapshot);   // Begin -> 전량 교체 -> Commit (실패 시 Rollback + 로그)
            // 성공 경로는 게임 스레드가 스냅샷 시점에 이미 dirty 를 껐으므로 회신 불요.
            //   롤백(실패)일 때만 회신해 낙관적으로 껐던 dirty 를 되살린다(-> 다음 주기 재저장). 발행 채널 id 동봉 = 이관 레이스 봉인.
            if (!committed && m_targetChannel != nullptr)
            {
                m_targetChannel->EnqueueSessionEvent(new InventorySaveFailedJob(m_snapshot.sid, m_targetChannel->GetChannelId()));
            }
        }

        // -- TradeCommitCommand --
        void TradeCommitCommand::Execute()
        {
            // 커밋 실패 시 인메모리(권위)는 이미 post-trade 로 확정됐는데 DB 는 거래 전이라 둘이 갈린다.
            //   양측을 dirty 로 되살려(SaveInventoryCommand 안전망과 대칭·B-1) 다음 autosave 가 둘 다 재저장하게 한다.
            //   재표시가 없으면 한쪽만 나중에 re-dirty 될 때 부분 저장 + 크래시로 복제/증발이 난다.
            //   반쪽 상태(한쪽만 반영)는 provider 단일 트랜잭션이 원천 봉쇄(전부 or 전무).
            if (!m_provider->TradeCommit(m_a, m_b, m_audit))
            {
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"db",
                    L"거래 영속 실패 (char %u <-> %u) - 양측 dirty 재표시, 다음 저장이 따라잡음", m_a.charId, m_b.charId);
                if (m_targetChannel != nullptr)
                {
                    const int chId = m_targetChannel->GetChannelId();
                    m_targetChannel->EnqueueSessionEvent(new InventorySaveFailedJob(m_a.sid, chId));
                    m_targetChannel->EnqueueSessionEvent(new InventorySaveFailedJob(m_b.sid, chId));
                }
            }
        }
    }
}
