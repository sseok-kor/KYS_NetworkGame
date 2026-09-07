#include "pch_serverapp.h"
#include "DBResult.h"                              // DbResultJob 선언 (struct DBResult 와 같은 파일)
#include "../Game/Channel/ChannelManager.h"        // GetSession / OnLoginSuccess / GetSharedServices
#include "../Game/Session/GameSession.h"                    // GameSession::GetPlayer / Send
#include "../Game/Player/Player.h"                  // SetPos/SetHp/SetMaxHp/SetMp/SetName/SetCharId
#include "../SharedService/SharedServicesThread.h"  // RegisterName 디렉터리
#include "../SharedService/NameDirectoryJobs.h"
#include "../../GameCommon/Protocol/CPacket.h"
#include "../../GameCommon/Protocol/GamePackets.h"           // SC_ENTER_WORLD / SC_INVENTORY / SC_STAT_UPDATE
#include "../../GameCommon/Protocol/PacketType.h"            // PacketType::SC_ENTER_WORLD
#include "../../GameServer/Types/Defines.h"         // MAX_PACKET_SIZE
#include "../../GameCommon/GameDefines.h"           // WHISPER_NAME_MAX
#include "../../GameCommon/ItemData/ItemTable.h"           // FindItemDef (로드된 인벤 아이템 정의 조회)
#include "../../GameCommon/MapData/MapTable.h"            // MapTableCount/MapTableAt/MapWidthFor·HeightFor (DB 손상 mapId/좌표 세탁)
#include "../../GameCommon/MapData/WalkableTable.h"       // IsWalkable (저장 좌표가 벽 안이면 부활 지점으로 세탁)
#include "../GameServer/Core/Log/Logger.h"          // 데이터 오염(고아 행) 이력 파일 로그

namespace KYS
{
    namespace SERVERAPP
    {
        DbResultJob::DbResultJob(const DBResult& result)
            : m_result(result)   // 값 스냅샷
        {
        }

        // 인벤 저장이 롤백됐을 때 dirty 를 되살린다. 발행 채널 스레드에서 실행되므로 Player 접근에 락 불요.
        //   게임 스레드가 스냅샷 시점에 낙관적으로 dirty 를 껐으므로, 저장 실패 시 이 재표시가 없으면 그 데이터는 다음 주기에 저장되지 않는다.
        //   generation-safe: 저장 왕복 사이 세션이 떠났으면 GetPlayer 가 null -> no-op.
        //   소유권 재검사: 채널변경(in-place handoff)으로 대상이 다른 채널 소유가 됐으면 no-op - 이관 경로(BeginChannelHandoff)가
        //   보수적으로 인벤 dirty 를 켜 두므로 새 소유 채널의 다음 autosave 가 재저장을 이어받는다.
        void InventorySaveFailedJob::Execute()
        {
            Player* p = ChannelManager::GetInstance().GetPlayer(m_sid);
            if (p != nullptr && p->GetGameSession() != nullptr && p->GetGameSession()->GetChannelId() == m_channelId)
            {
                p->MarkDbDirty(DB_DIRTY_INVENTORY);   // 저장 실패 - 다음 주기에 재시도하도록 인벤 dirty 재표시
            }
        }

        // 로그인 완료를 콜백 하나로 (generation 재확인 -> AttachPlayer -> 캐릭터/인벤 채움 -> Send -> spawn 예약).
        //   원자 로드: 캐릭터와 인벤이 한 결과(m_result + 동봉 inventory)로 오므로, attach~인벤채움~spawn 예약이 이 Job 하나 안에서
        //   원자 실행된다. ProcessSessionEvent 는 한 Job 을 끝까지 실행하므로 "attached 지만 인벤 빈" 상태가 Job 경계를 넘지 못하고,
        //   어떤 저장 경로(OnClientLeave/종료 저장)도 빈 인벤을 관측하지 못한다(부분 로드 창 소멸).
        //   (정체성/인증 = OnLoginSuccess[Init+SetChannel] 은 LoginLinkThread admission 이 먼저 처리.)
        void DbResultJob::Execute()
        {
            // (a) generation 재확인 - 로드 중 클라가 끊겨 슬롯이 재사용/Reset 됐으면 nullptr -> return.
            //     이 재확인이 AttachPlayer(Player 할당) *전* 이라, 옛 핸들이면 할당/spawn 자체가 안 일어난다 (버려진 객체/누수 0).
            GameSession* gs = ChannelManager::GetInstance().GetSession(m_result.sid);
            if (gs == nullptr)
            {
                return;   // 끊김/옛 핸들 (정상 race, 조용히 버림, Player 미할당이라 회수 불요)
            }

            // (a') 캐릭터 로드 실패(loadFailed)/미발견(!found), 또는 인벤 로드 실패 - Player 부착 *전* 에 세션 정리(부분 로드 방지).
            //   인벤 backend 도달 실패면 빈 인벤으로 입장시키는 순간 다음 저장이 아이템을 통째로 지우므로, 캐릭터 실패와 동일하게 끊는다(클라 재접속 복구).
            if (m_result.loadFailed || !m_result.found || m_result.inventory.loadFailed)
            {
                FailLogin(gs);
                return;
            }

            // (b) Player 할당(ChannelManager 풀) + gs->SetPlayer (findable). spawn 은 아래 (f) 에서 인벤 채운 뒤 예약.
            Player* p = ChannelManager::GetInstance().AttachPlayer(m_result.sid);
            if (p == nullptr)
            {
                FailLogin(gs);   // 풀 고갈 - loadFailed 와 동일한 un-reapable player-less 누수라 같은 정리(대칭, DBResultJob 잠복 누수 봉인)
                return;
            }

            // (c) 캐릭터 필드 채움 - 방금 할당된 Player 에 로드 데이터 적용 (spawn 전에 채운다). found=true 확정(위 (a')에서 걸렀음).
            //   신규 캐릭터 생성은 별도 경로(CreateCharacter INSERT 기본값)라 여긴 항상 기존 캐릭터 복원만.
            p->SetCharId(m_result.charId);  // SaveFull WHERE char_id 키 - 저장 방향 전파에 필요

            // DB 손상 방어 1 - mapId 를 좌표보다 먼저 검증한다. 아래 좌표 클램프/통행 검사가 이 mapId 로
            //   맵 표/통행 격자를 조회하므로, 범위 밖 값이 먼저 걸러지지 않으면 엉뚱한 맵 크기로 클램프된다.
            //   범위 밖 = 맵0 부활 지점으로 세탁 (하류 AddPlayer 가드와 대칭 - 여기가 producer 라 1차 책임).
            int mapId = m_result.mapId;
            Position pos;
            if (mapId < 0 || mapId >= MapTableCount())
            {
                mapId = 0;
                pos = MapTableAt(0).respawn;
            }
            else
            {
                // DB 손상 방어 2 - 좌표를 그 맵 경계로 클램프 (범위 밖 좌표가 Map::Insert 의 m_grid 범위 밖 쓰기가 되는 것 차단).
                const int w = MapWidthFor(mapId);
                const int h = MapHeightFor(mapId);
                pos.x = (m_result.x < 0) ? 0 : ((m_result.x > w) ? w : m_result.x);
                pos.y = (m_result.y < 0) ? 0 : ((m_result.y > h) ? h : m_result.y);

                // 저장 뒤 통행 파일이 좁아져 저장 좌표가 벽 안일 수 있다 - 재로그인이 유일한 세탁 지점.
                //   벽 안이면 그 맵 부활 지점으로 (벽 안 위치가 자동 저장으로 다시 영속되는 고착 차단).
                if (!IsWalkable(mapId, pos.x, pos.y)) { pos = MapTableAt(mapId).respawn; }
            }
            p->SetMapId(mapId);   // 저장 맵 복원 (세탁 완료 값)
            p->SetPos(pos);
            p->SetHp(m_result.hp);          // GameObject base
            p->SetMaxHp(m_result.maxHp);    // hp 상한 복원
            p->SetMp(m_result.mp);          // Player::m_mp
            p->SetName(m_result.name);      // Player::m_name (char_name 에서 로드)

            // (d) 입장 완료 통지(admission-ack). gs->Send -> 풀의 SendTo 가 세대 검증 + lease -> 혹시 끊겼으면 응답이 자연히 드롭(전송 안 됨).
            SC_ENTER_WORLD enter;
            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_ENTER_WORLD));
            enter.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            // SC_ENTER_WORLD 는 평문(scramble 안 함) = obfuscation arm 트리거. 클라는 아직 unarmed 라 평문으로 정상 판독하고
            //   그 핸들러에서 arm -> 다음 패킷(SC_INVENTORY)부터 scramble/descramble. 이 패킷은 서버 send 키를 advance 하지 않는다.
            //   성공 경로만 이 opcode 를 보내므로(거부/인증실패는 SC_LOGIN_RESULT{FAIL}) 클라 arm 이 payload 검사 없이 opcode 수신만으로 성립한다.
            gs->Send(out.GetBuffer(), out.GetSize(), ESendDropPolicy::KEEP_CONNECTION);   // 평문 admission 응답 - 키 전진 전이라 넘쳐도 안 끊는다

            // (e) 동봉 인벤 스냅샷을 Player 에 채우고 SC_INVENTORY/SC_STAT_UPDATE 통지.
            //     spawn *전* 에 인메모리 인벤을 채우는 것이 저장 경로 안전 불변식의 핵심 - grid 편입 시엔 이미 인벤 완비.
            ApplyInventory(p, gs);

            // 이름 디렉터리 등록 - 다른 스레드(이 DB 결과 스레드)에서 디렉터리를 직접 만지지 않고
            //   SST 에 Job 위임 (디렉터리 쓰기는 SST 스레드 혼자, Single Writer). sid 동봉(재로그인 race 짝맞춤).
            SharedServicesThread::GetInstance().EnqueueJob(new RegisterNameJob(p->GetName(), m_result.sid));   // 이름 디렉터리 등록 (SST 단독 write)

            // (f) 채널 진입(spawn) 예약 - 캐릭터+인벤을 다 채운 *뒤* 에만 grid 편입되도록(원자 로드).
            //     OnChannelEnterJob 은 같은 drain 루프의 다음 바퀴에서 실행되므로(ProcessSessionEvent 의 while(Dequeue) 는
            //     상한 없이 돌고, 이 Job 이 Execute 를 끝내야 다음 Dequeue 가 온다), grid 열거 저장 경로는 항상 인벤 완료 상태만 본다.
            ChannelManager::GetInstance().EnqueueChannelEnter(m_result.sid, m_result.chId);
        }

        // 로그인 실패 정리 - RST 강제 종료(DB_LOAD_FAILED, 서버 TIME_WAIT 회피)로 라이브러리 EnqueueDisconnect -> OnSessionGone 단일 대칭
        //   teardown(CCU/online/역인덱스/풀 해제, player-less 세션 회수)을 유발한다. 실패 응답 패킷은 보내지 않는다 - RST(CancelIoEx +
        //   SO_LINGER{1,0})가 직전 WSASend 를 취소/폐기해 전달이 보장 안 되고, 클라는 연결 끊김(recv 에러)으로 실패를 인지하기 때문
        //   (kick 경로 LoginLinkThread::KickPending 의 "Send 없이 종료" idiom 과 동일, WSASend/closesocket race 회피). DBThread 아니라 채널 스레드(여기)에서만 호출(GameSession 단일 writer).
        void DbResultJob::FailLogin(GameSession* gs)
        {
            gs->Disconnect(EDisconnectReason::DB_LOAD_FAILED);
        }

        // 동봉 인벤 스냅샷을 Player 에 채우고 전체 상태를 클라에 통지 (DbResultJob::Execute 내부에서만 호출 - spawn 전 인메모리 채움).
        //   호출 시점엔 p/gs 유효 + 인벤 loadFailed 아님이 caller(Execute (a')~(b))에서 이미 확정.
        void DbResultJob::ApplyInventory(Player* p, GameSession* gs)
        {
            // Player 인벤 채움. 모르는 templateId(고아 행)는 참조 무결성 관문에서 걸러 칸을 비운다.
            Inventory& inv = p->GetInventory();
            inv.ClearAll();
            for (int i = 0; i < m_result.inventory.count; ++i)
            {
                const InventoryRow& row = m_result.inventory.rows[i];
                const ItemDef* itemDef = FindItemDef(row.templateId);
                if (itemDef == nullptr)
                {
                    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                        KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"db",
                        L"인벤 로드 - 모르는 templateId %d (char_id=%u, uid=%llu) - 칸 비움",
                        row.templateId, m_result.charId, row.uid);
                    continue;   // 다음 저장에서 자연 정리 (전량 교체 저장이라 빈 칸은 행이 안 남음)
                }
                if (row.quantity <= 0)
                {
                    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                        KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"db",
                        L"인벤 로드 - 수량 오염 %d (char_id=%u, uid=%llu) - 칸 비움",
                        row.quantity, m_result.charId, row.uid);
                    continue;
                }
                int quantity = row.quantity;
                if (quantity > itemDef->stackMax)
                {
                    // 과적 행 클램프 - 거래 수량 상한 검증은 "칸 수량 <= stackMax" 불변식에 기대므로 로드에서 복원한다(오염 1행이 오버플로를 재무장하지 않게).
                    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                        KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_WARN, L"db",
                        L"인벤 로드 - 수량 %d > stackMax %d 클램프 (char_id=%u, uid=%llu)",
                        row.quantity, itemDef->stackMax, m_result.charId, row.uid);
                    quantity = itemDef->stackMax;
                }
                inv.SetAt(row.slot, row.uid, row.templateId, quantity);
            }

            // 인벤 전체 스냅샷을 클라에 통지 (입장 초기화 - 이후 변화는 SC_ITEM_UPDATE 가 슬롯 단위로).
            SC_INVENTORY invBody{};
            invBody.count = 0;
            static const int kSlots[2] = { EQUIP_SLOT_WEAPON, EQUIP_SLOT_ARMOR };
            for (int slot = 0; slot < MAX_INVENTORY_SLOTS; ++slot)
            {
                const ItemInstance& it = inv.At(slot);
                if (it.IsEmpty()) { continue; }
                ItemSlotEntry& e = invBody.items[invBody.count++];
                e.slot = static_cast<BYTE>(slot);
                e.uid = it.uid;
                e.templateId = it.templateId;
                e.quantity = it.quantity;
            }
            for (int i = 0; i < 2; ++i)
            {
                const ItemInstance& it = inv.At(kSlots[i]);
                if (it.IsEmpty()) { continue; }
                ItemSlotEntry& e = invBody.items[invBody.count++];
                e.slot = static_cast<BYTE>(kSlots[i]);
                e.uid = it.uid;
                e.templateId = it.templateId;
                e.quantity = it.quantity;
            }
            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_INVENTORY));
            invBody.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            gs->SendScrambled(out.GetBuffer(), out.GetSize());

            // 장착 반영 공/방 통지 (입장 초기화 - 클라 표시용. 권위 계산은 어차피 서버).
            SC_STAT_UPDATE statBody{};
            statBody.atkPower = p->GetAttackPower();
            statBody.defPower = p->GetDefPower();
            KYS::GAMECOMMON::PROTOCOL::CPacket statOut(MAX_PACKET_SIZE);
            statOut.Begin(static_cast<USHORT>(PacketType::SC_STAT_UPDATE));
            statBody.Serialize(statOut);
            if (!statOut.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", statOut.GetType(), statOut.GetSize()); return; }
            gs->SendScrambled(statOut.GetBuffer(), statOut.GetSize());
        }
    }
}
