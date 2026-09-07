#include "pch_serverapp.h"
#include "Channel.h"
#include "ChannelManager.h"
#include "../World/MapManager.h"
#include "../Combat/CombatFormula.h"
#include "../Item/Item.h"        // IssueItemUid (줍기 확정 순간 영속 uid 발급)
#include "../Item/DropTable.h"   // 몬스터 사망 드랍 추첨 (서버 전용 표)
#include "../../DataBase/DBCommands.h"
#include "../../SharedService/SharedServicesThread.h"
#include "../../SharedService/CrossChannelChat.h"
#include "../../SharedService/NameDirectoryJobs.h"
#include "../GameCommon/Protocol/GamePackets.h"
#include "../GameCommon/GameDefines.h"
#include "../GameCommon/Protocol/PacketHeader.h"
#include "../GameCommon/Protocol/PacketType.h"
#include "../GameCommon/Protocol/CPacket.h"
#include "../GameCommon/MapData/PortalTable.h"
#include "../GameCommon/ItemData/ItemTable.h"       // FindItemDef / EItemType (아이템 정의 조회 - 줍기/장착/소비 분기)
#include "../GameCommon/MapData/MapTable.h"        // MapTableCount/MapTableAt/MapWidthFor/MapHeightFor (맵 메타 - maps.csv 적재 표)
#include "../GameCommon/MapData/WalkableTable.h"   // IsPathWalkable (이동 보고 수용의 벽 게이트 - 얇은 벽 건너뛰기 차단)
#include "../Monster/SpawnTable.h"         // SpawnGroupData/SpawnGroupCount (스폰 그룹 - spawns.csv 적재 표)
#include "../GameServer/Core/Log/Logger.h"   // 서버/유저 이벤트 파일 로그 (라이브러리는 도구 제공만 - 호출은 앱)
#include <algorithm>   // std::sort - tick 표본 정렬(percentile/trimmed mean 산출)

namespace KYS
{
    namespace SERVERAPP
    {
        // 이탈 사유를 로그에 남길 실무어로 (EDisconnectReason 각 값과 1:1 - 값 추가 시 case도 함께).
        static const wchar_t* DisconnectReasonName(EDisconnectReason reason)
        {
            switch (reason)
            {
            case EDisconnectReason::SEND_TIMEOUT:       return L"SEND_TIMEOUT";
            case EDisconnectReason::RECV_FAIL:          return L"RECV_FAIL";
            case EDisconnectReason::PROTOCOL_VIOLATION: return L"PROTOCOL_VIOLATION";
            case EDisconnectReason::CLIENT_FIN:         return L"CLIENT_FIN";
            case EDisconnectReason::SERVER_SHUTDOWN:    return L"SERVER_SHUTDOWN";
            case EDisconnectReason::IDLE_TIMEOUT:       return L"IDLE_TIMEOUT";
            case EDisconnectReason::NET_RESET:          return L"NET_RESET";
            case EDisconnectReason::SEND_FAIL:          return L"SEND_FAIL";
            case EDisconnectReason::SERVER_FULL:        return L"SERVER_FULL";
            case EDisconnectReason::DB_LOAD_FAILED:     return L"DB_LOAD_FAILED";
            case EDisconnectReason::NO_RECV_TIMEOUT:    return L"NO_RECV_TIMEOUT";
            case EDisconnectReason::LOGIN_REJECTED:     return L"LOGIN_REJECTED";
            default:                                    return L"UNKNOWN";
            }
        }
        // 30Hz tick 간격 - 1,000,000us / 30 = 33,333.3 -> 33,333us (정수). MMORPG sim 표준(10~20Hz), LoL 30Hz 정합.
        //   dt/sleep 예산/over-warn 임계가 이 값에서 파생되어 자동 보정(쿨다운/이동/AI는 dt 기반이라 실제 시간 불변).
        static const UINT64 TICK_INTERVAL_US = 33333;

        // ProcessJob per-tick drain 상한 (livelock 차단, graceful degradation). 상한 초과분은 큐에 남아 다음 tick(defer), 큐 가득이면 shed.
        //   critical(로그인/전투/채팅/포탈)은 저빈도라 큰 상한, movement(CS_MOVE)는 droppable이라 과부하 시 먼저 shed.
        //   측정 튜닝 값: 30Hz는 tick당 33ms 예산이라 한 tick에 더 처리 가능 -> 초당 처리 천장(2000x30=60K) 보존 위해 2000.
        static const int MAX_CRITICAL_JOBS_PER_TICK = 2000;
        static const int MAX_MOVE_JOBS_PER_TICK = 2000;

        static const UINT64 ORPHAN_SWEEP_INTERVAL_MS = 1000;    // idle 점검 주기 (약 1초마다)
        static const UINT64 SESSION_IDLE_TIMEOUT_MS = 30000;    // 무활동 판정 임계 (30초)

        // 스폰포인트 표와 맵별 유지 목표 인구는 파일로 외부화 - 조정은 재컴파일 대신 파일 한 줄.
        //   스폰포인트(위치+종류+배회 기준점) = Data/spawns.csv (SpawnTable) / 맵별 유지 인구 = Data/maps.csv 의 monsterCap (MapTable).
        //   두 값의 분리(스폰포인트 수 != 유지 인구 = 밀도 레버)는 하드코딩 시절 그대로다.

        // 포탈 표는 GameCommon 공유 SSOT(Data/portals.csv)에서 부팅 시 적재 - 클라와 같은 파일을 읽는다.
        //   portalId = 행 순서(0부터). 좌표 규약과 dst 산출은 portals.csv 주석 참고. 도착(dst)은 서버 권위.

        // portalId가 가리키는 포탈이 발동 가능한지 서버 권위로 검증한다 (핵/스푸핑 차단).
        //   p        : 발동을 시도한 플레이어
        //   portalId : 클라가 보낸 포탈 번호
        //   반환     : 발동 가능하면 해당 PortalDef*, 아니면 nullptr (범위 밖 / 다른 맵 / 트리거 반경 밖)
        static const PortalDef* ResolvePortal(const Player* p, int portalId)
        {
            if (portalId < 0 || portalId >= PortalTableCount()) { return nullptr; }   // 범위 밖
            const PortalDef& portal = PortalTableAt(portalId);
            if (p->GetMapId() != portal.srcMapId) { return nullptr; }          // 다른 맵에서 온 스푸핑 차단
            const Position pos = p->GetPos();
            const long long dx = static_cast<long long>(pos.x) - portal.trigger.x;
            const long long dy = static_cast<long long>(pos.y) - portal.trigger.y;
            if (dx * dx + dy * dy > static_cast<long long>(portal.triggerRadius) * portal.triggerRadius)
            {
                return nullptr;   // 트리거 반경 밖 (거리 제곱 비교, sqrt 회피)
            }
            return &portal;
        }

        // 한 세션에게 SC 패킷 1개를 만들어 보낸다 (Begin -> body.Serialize -> End -> scramble 송신).
        //   단일 수신자 통지의 공통 관용구. body 는 Serialize(CPacket&) const 를 가진 SC 구조체. 호출 전 gs != nullptr 보장.
        template <typename TBody>
        static void SendToSession(GameSession* gs, PacketType opcode, const TBody& body)
        {
            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(opcode));
            body.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            gs->SendScrambled(out.GetBuffer(), out.GetSize());
        }

        // 본인에게 자기 맵/위치를 통지한다 (SC_MAP_CHANGE). 로그인 스폰과 포탈 이동 후 공통.
        //   클라가 자기 새 맵/좌표를 아는 유일한 경로 (자기 자신 SC_SPAWN은 안 보내므로).
        static void SendMapChange(const Player* p)
        {
            const Position pos = p->GetPos();
            SC_MAP_CHANGE notify;
            notify.playerId = p->GetPlayerId();
            notify.mapId = p->GetMapId();
            notify.x = pos.x;
            notify.y = pos.y;
            GameSession* gs = p->GetGameSession();   // 짝 세션 역포인터 (세션 재조회 회피)
            if (gs != nullptr) { SendToSession(gs, PacketType::SC_MAP_CHANGE, notify); }
        }

        // 로그인 완료 시 본인에게 자기 캐릭터 전체 초기 상태를 통지한다 (SC_CHAR_INFO, DB 로드값).
        //   클라가 자기 hp/maxHp(기본값 max)를 DB 정확값으로 보정한다 - SC_MAP_CHANGE엔 hp가 없어 로그인 시 자기 hp를 max로 표시하던 갭 해소.
        //   MapleStory getCharInfo 정합: 로그인=전체 상태(여기) / 이후 포탈=SC_MAP_CHANGE 위치 통지.
        static void SendCharInfo(const Player* p)
        {
            const Position pos = p->GetPos();
            SC_CHAR_INFO notify;
            notify.playerId = p->GetPlayerId();
            notify.mapId = p->GetMapId();
            notify.x = pos.x;
            notify.y = pos.y;
            notify.hp = p->GetHp();
            notify.maxHp = p->GetMaxHp();
            notify.mp = p->GetMp();
            GameSession* gs = p->GetGameSession();   // 짝 세션 역포인터 (세션 재조회 회피)
            if (gs != nullptr) { SendToSession(gs, PacketType::SC_CHAR_INFO, notify); }
        }

        // 채널 변경 결과를 본인에게 통지한다 (SC_CHANNEL_CHANGE_RESULT). OK면 클라가 옛 채널 시야를 GameState reset.
        //   즉시 Send(lease 스레드 안전) - OK는 도착측 채널 스레드가, 실패는 출발측 채널 스레드가 보낸다.
        static void SendChannelChangeResult(GameSession* gs, EChannelChangeResult result, int channelId)
        {
            if (gs == nullptr) { return; }
            SC_CHANNEL_CHANGE_RESULT notify;
            notify.result = static_cast<BYTE>(result);
            notify.channelId = static_cast<BYTE>(channelId);
            SendToSession(gs, PacketType::SC_CHANNEL_CHANGE_RESULT, notify);
        }

        // 벽 게이트가 이동 보고를 보정한 횟수 (전 채널 공용 관측 신호 - 정상 플레이는 0 근방에 머문다.
        //   클라도 같은 통행 격자로 같은 벽에서 멈추기 때문. 지속 발화 = 좌표 조작 or 지형 못 읽은 강등 클라.
        //   드문 정상 발화도 있다: 벽 모서리를 꺾어 도는 중 보고 하나가 과부하로 유실되면 서버 예측과
        //   보고 사이 "직선"이 실제 걸은 ㄱ자 경로와 달라 벽 칸을 지날 수 있다 - 자기수렴(1회 보정)이라 무해).
        static volatile LONG s_wallGateCorrectCount = 0;

        // 클라가 보고한 이동 위치를 서버 dead-reckon 예측과 대조해 수용/보정한다 (서버 권위, 치트 방지).
        //   허용오차 안 + 벽 통과 없음 -> 보고 위치 수용(맵 경계 클램프) / 아니면 -> 서버 권위 예측 위치(predictedPos)로 덮음.
        //   둘 다 SetDirty -> SendObjectUpdates이 전원 broadcast(self 포함) 대상으로 수집 (mover는 자기 echo로 맞춤).
        static void AcceptOrCorrectPosition(Player* p, const CS_MOVE& req)
        {
            const Position predictedPos = p->PosSync();   // 서버 dead-reckon 위치 예측 (현재 상태 기준)
            const int dx = req.x - predictedPos.x;
            const int dy = req.y - predictedPos.y;
            if ((dx * dx + dy * dy) <= (MOVE_SYNC_TOLERANCE * MOVE_SYNC_TOLERANCE))   // 거리 제곱 비교 (sqrt 회피)
            {
                // 허용오차 내 -> 보고 좌표 후보를 그 맵 경계 안에 가둔다 (맵마다 크기 다름).
                const int mapId = p->GetMapId();
                const int w = MapWidthFor(mapId);
                const int h = MapHeightFor(mapId);
                Position accepted;
                accepted.x = (req.x < 0) ? 0 : ((req.x > w) ? w : req.x);
                accepted.y = (req.y < 0) ? 0 : ((req.y > h) ? h : req.y);

                // 서버 위치 -> 보고 좌표 사이 칸이 전부 통행일 때만 수용. 끝점만 검사하면 허용오차(64px)가
                //   벽 두께(50px)보다 커서 얇은 벽 너머 좌표 보고가 그대로 통과한다 (막힌 보고 = 거부·보정).
                if (IsPathWalkable(mapId, predictedPos, accepted))
                {
                    p->SetPos(accepted);
                }
                else
                {
                    p->SetPos(predictedPos);   // 벽 통과 보고 -> 서버 권위 위치로 보정
                    const LONG count = ::InterlockedIncrement(&s_wallGateCorrectCount);
                    if (count == 1 || (count % 100) == 0)   // 첫 회 + 100회마다 - 스팸 없이 발화 사실만 관측
                    {
                        KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                            KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"wallgate",
                            L"벽 게이트 보정 %ld회 (최근 sid=%llu)", count, p->GetSid());
                    }
                }
            }
            else
            {
                // 초과(치트/동기 어긋남) -> 서버 권위 위치로 보정 (전원 broadcast가 mover에게 되돌아가 맞춤).
                p->SetPos(predictedPos);
            }
            p->SetDirty(true);   // 수용/보정 둘 다 바뀜 표시
        }

        // 거래 양측의 수락 표시를 함께 리셋한다. 오퍼가 바뀌면(add/remove/커밋 실패) 직전 수락은 무효 - 막판 바꿔치기 스캠 방어.
        static void ResetBothAccepts(Player* a, Player* b)
        {
            a->GetTrade().accepted = false;
            b->GetTrade().accepted = false;
        }

        // Player의 현재 상태를 DB 저장용 구조체에 채운다.
        //   p   : 스냅샷을 뜰 플레이어
        //   out : 채워질 저장 구조체
        static void FillCharacterSnapshot(const Player* p, DBResult& out)
        {
            out.sid = p->GetSid();
            out.chId = 0;                  // 저장엔 불요(회신 라우팅 키) - 0 무방. 로드 회신에서만 의미.
            out.found = true;
            out.charId = p->GetCharId();   // SaveFull 이 WHERE char_id 로 이 캐릭터 행만 갱신 (저장 방향 전파)
            out.playerId = p->GetPlayerId();
            const Position pos = p->GetPos();
            out.x = pos.x;
            out.y = pos.y;
            out.mapId = p->GetMapId();
            out.hp = p->GetHp();
            out.maxHp = p->GetMaxHp();
            out.mp = p->GetMp();
            wcscpy_s(out.name, WHISPER_NAME_MAX, p->GetName());
        }

        // Player의 인벤토리(가방 + 장착)를 DB 저장용 전량 스냅샷으로 뜬다 (빈 칸 제외).
        //   저장은 전량 교체 방식이라 이 스냅샷이 곧 "저장 후 DB 의 전부"가 된다.
        static void FillInventorySnapshot(const Player* p, InventorySnapshot& out)
        {
            out.sid = p->GetSid();
            out.chId = 0;               // 저장엔 불요 (로드 회신 라우팅 키)
            out.loadFailed = false;
            out.charId = p->GetCharId();
            out.count = 0;

            const Inventory& inv = p->GetInventory();
            for (int slot = 0; slot < MAX_INVENTORY_SLOTS; ++slot)
            {
                const ItemInstance& it = inv.At(slot);
                if (it.IsEmpty()) { continue; }
                InventoryRow& row = out.rows[out.count++];
                row.slot = static_cast<BYTE>(slot);
                row.uid = it.uid;
                row.templateId = it.templateId;
                row.quantity = it.quantity;
            }
            static const int kEquipSlots[2] = { EQUIP_SLOT_WEAPON, EQUIP_SLOT_ARMOR };
            for (int i = 0; i < 2; ++i)
            {
                const ItemInstance& it = inv.At(kEquipSlots[i]);
                if (it.IsEmpty()) { continue; }
                InventoryRow& row = out.rows[out.count++];
                row.slot = static_cast<BYTE>(kEquipSlots[i]);
                row.uid = it.uid;
                row.templateId = it.templateId;
                row.quantity = it.quantity;
            }
        }

        // 캐릭터 + 인벤토리 전량을 마지막 기회 저장 (로그아웃/서버 종료). 캐릭터 먼저, 인벤 다음 순서.
        //   호출 전 dbThread != nullptr 이 보장돼야 한다 (두 호출처 모두 상위에서 null 체크). autosave 의 dirty 게이트 저장과는 다른 경로.
        static void SavePlayerFull(const Player* p, DBThread* dbThread)
        {
            DBResult snapshot;
            FillCharacterSnapshot(p, snapshot);
            dbThread->Enqueue(new SaveFullCommand(snapshot));

            InventorySnapshot invSnapshot;
            FillInventorySnapshot(p, invSnapshot);
            dbThread->Enqueue(new SaveInventoryCommand(invSnapshot));
        }

        // 받는 쪽 가방에 이 uid 실물이 실제로 놓였는지 확인 - Inventory::Add 는 같은 종류 스택이 있으면
        //   먼저 거기 붓고(전량 합류 시 넘겨받은 uid 는 안 씀), 남은 양만 새 실물로 놓기 때문.
        //   감사의 new uid 가 존재하지 않는 실물을 가리키면 거짓 증거가 되므로 놓임 여부를 실측한다.
        static bool InventoryContainsUid(const Inventory& inv, UINT64 uid)
        {
            for (int slot = 0; slot < MAX_INVENTORY_SLOTS; ++slot)
            {
                if (inv.At(slot).uid == uid) { return true; }
            }
            return false;
        }

        // 거래 감사 행 1개 추가 - 오퍼(원본 uid/템플릿/수량)에 수령측 새 uid 와 이동 전 스택 수량을 잇는다.
        //   old->new uid 짝이 아이템 계보의 연결 고리 (CommitTrade 의 받기 루프에서 호출).
        //   newUid = 0 은 "전량이 수령측 기존 스택에 합류"라는 뜻 - 스택형은 합류 순간 실물 정체성이
        //   흡수되므로(rAthena 가 스택형에 uid 를 아예 안 주는 것과 같은 본질) 0 이 정직한 기록이다.
        static void AppendTradeAudit(TradeAuditSnapshot& audit, UINT32 giverCharId, UINT32 receiverCharId,
                                     const TradeOfferSlot& offer, UINT64 newUid, int prevQuantity)
        {
            if (audit.count >= TradeAuditSnapshot::MAX_ROWS) { return; }   // 상한 = 양측 오퍼 합 - 정상 경로에선 도달 불가
            TradeAuditRecord& row = audit.rows[audit.count];
            row.giverCharId    = giverCharId;
            row.receiverCharId = receiverCharId;
            row.oldUid         = offer.uid;
            row.newUid         = newUid;
            row.templateId     = offer.templateId;
            row.quantity       = offer.quantity;
            row.prevQuantity   = prevQuantity;
            ++audit.count;
        }

        // 부팅 시 스폰 그룹을 MapManager에 등록하고 그룹별 count만큼 초기 채움한다(이후는 그룹별 부활 예약이 유지).
        //   main 이 콘텐츠 파일(maps/monsters/spawns) 적재 검증을 끝낸 뒤 채널 스레드 기동 전에 명시 호출한다
        //   (생성자에서 부르면 파일 적재보다 먼저 돌아 빈 표로 스폰하므로 - 2단계 초기화).
        void Channel::SpawnInitialMonsters()
        {
            int mapCaps[MAX_MAP_COUNT] = {};   // 맵별 안전 천장 - maps.csv monsterCap 을 InitSpawns 계약 형태(배열)로 변환
            for (int i = 0; i < MapTableCount(); ++i)
            {
                mapCaps[i] = MapTableAt(i).monsterCap;
            }
            m_mapManager.InitSpawns(SpawnGroupData(), static_cast<size_t>(SpawnGroupCount()), mapCaps);   // 그룹 등록 + 천장 설정 + 초기 채움 (관찰자 0이라 통지 무동작)
        }

        // 채팅 가능 여부 - 빈 메시지/도배를 거른다.
        //   sender : 보낸 플레이어
        //   msg    : 메시지
        //   len    : 길이
        bool Channel::CanChat(Player* sender, const wchar_t* msg, int len)
        {
            if (msg == nullptr || len <= 0)
            {
                return false;   // 빈/널 메시지 드롭
            }
            const UINT64 now = ::GetTickCount64();   // 프로세스 전역 단조 시계 - m_lastChatMs 는 채널 이관 후에도 살아남으므로 채널별 타이머(epoch 상이)로 비교하면 cross-epoch 언더플로가 난다
            if (now - sender->GetLastChatMs() < static_cast<UINT64>(CHAT_MIN_INTERVAL_MS))
            {
                return false;   // 도배 -> 드롭
            }
            sender->SetLastChatMs(now);
            return true;
        }

        // 채널 생성 - 큐/타이머/모니터 카운터만 초기화한다 (2단계 초기화의 1단계).
        //   초기 몬스터 스폰은 main 이 콘텐츠 파일 적재 후 SpawnInitialMonsters 를 따로 부른다 (2단계).
        //   channelId : 이 채널 id (전부 게임 채널, 0..N-1)
        Channel::Channel(int channelId)
            : m_channelId(channelId)
            , m_mailbox(2048)                          // critical lane nodePool 2048 (>= cap 2000 -> cap이 실효되게 풀 정합, 기본 1024면 cap 무효였음)
            , m_moveMailbox(2048, KYS::GAMESERVER::THREAD::EOverflowPolicy::DROP)   // movement lane (CS_MOVE) nodePool 2048 (> cap 2000 -> defer headroom 48), DROP - 과부하 시 shed
            , m_sessionEventMailbox(256, KYS::GAMESERVER::THREAD::EOverflowPolicy::GROW)
            , m_tickTimer()
            , m_lastDbSaveMs(0)
            , m_lastSweepMs(0)
            , m_running(true)
            , m_mapManager(channelId)               // 채널 id 주입 - 스폰 몬스터에 stamp (길찾기 결과 회신 대상). 선언 순서: m_running 뒤
            , m_broadcastPacket(MAX_PACKET_SIZE)   // 선언 순서: m_idleSids 뒤 - mover broadcast 재사용 버퍼
            , m_packetCount(0)
            , m_bytesRecv(0)
            , m_bytesSent(0)
            , m_totalTickUs(0)
            , m_maxTickUs(0)
            , m_overBudgetCount(0)
            , m_warnCount(0)
            , m_tickCount(0)
            , m_phaseRecvUs(0)
            , m_phaseBroadcastUs(0)
            , m_phaseSendUs(0)
            , m_phaseUpdateUs(0)
            , m_tickSamples{}            // 원형 버퍼 0 초기화 (미기록 칸은 count 가드로 안 읽지만 안전하게)
            , m_tickSampleHead(0)
            , m_tickSampleCount(0)
            , m_sleepUs(0)
            , m_lastDrainCount(0)
            , m_jobSamples{}
            , m_droppedMove(0)
            , m_droppedCritical(0)
        {
            // 스폰은 여기서 하지 않는다 - 콘텐츠 파일(maps/monsters/spawns) 적재가 채널 생성보다 늦어
            //   main 이 적재 검증 후 SpawnInitialMonsters 를 명시 호출한다 (채널 스레드 기동 전 단일 스레드 구간).
        }

        Channel::~Channel()
        {
            // m_mailbox/m_tickTimer는 값 멤버 -> 자동 소멸.
            // 미처리 Job이 mailbox에 남을 수 있으나 종료 경로는 main이 Stop->join 후 처리.
        }

        // 64바이트 정렬 할당 (m_mailbox가 alignas(64)이라 일반 new로는 정렬 보장 안 됨).
        void* Channel::operator new(size_t size)
        {
            void* p = _aligned_malloc(size, alignof(Channel));   // 64 정렬
            if (p == nullptr)
            {
                throw std::bad_alloc();
            }
            return p;
        }

        void Channel::operator delete(void* ptr)
        {
            _aligned_free(ptr);   // _aligned_malloc과 반드시 짝 (일반 free는 UB). main의 delete가 호출.
        }

        bool Channel::EnqueueJob(KYS::GAMESERVER::THREAD::IJob* job)
        {
            // critical lane - Worker(producer) tail 락, Channel 스레드 drain. 풀(2048)+자체 cap으로 best-effort 보존이나
            //   극단 burst(>2048 적체) 시 DROP은 못 막는다(GROW는 OOM 위험이라 DROP이 safety valve). -> drop 계측해 가시화.
            if (!m_mailbox.Enqueue(job))
            {
                ::InterlockedIncrement64(&m_droppedCritical);   // critical 손실(login/skill/chat) - movement(m_droppedMove)와 대칭 계측
                return false;   // 포화(DROP) - mailbox 가 job 을 이미 delete함. 호출자는 회수 금지(이중 해제), bool 은 계측용
            }
            return true;
        }

        void Channel::EnqueueMoveJob(KYS::GAMESERVER::THREAD::IJob* job)
        {
            if (!m_moveMailbox.Enqueue(job))   // movement lane(DROP) - 풀 소진 시 false -> CS_MOVE shed(다음 보고 + dead-reckoning이 메움)
            {
                ::InterlockedIncrement64(&m_droppedMove);   // shed 계측 (워커 다수 동시 -> Interlocked)
            }
        }

        void Channel::EnqueueSessionEvent(KYS::GAMESERVER::THREAD::IJob* job)
        {
            m_sessionEventMailbox.Enqueue(job);     // 세션이벤트(enter/leave/login-complete) - GROW(드롭 없음)
        }


        int Channel::GetChannelId() const
        {
            return m_channelId;       // m_channelId 반환
        }

        KYS::SERVERAPP::Monster* Channel::FindMonsterForPath(int mapId, UINT32 monsterId)
        {
            return m_mapManager.FindMonsterForPath(mapId, monsterId);   // 길찾기 결과 재주입 - 맵 순회 재조회 위임
        }

        // 채널 진입 - Player를 맵에 올리고 활동 시각을 시작점으로 찍는다.
        void Channel::OnChannelEnter(UINT64 sid)
        {
            ChannelManager& cm = ChannelManager::GetInstance();   // 싱글턴 직접 (m_channelManager 멤버 제거)
            m_mapManager.AddPlayer(sid, &cm);          // 진입 맵은 DB 결과로 세팅된 Player.mapId 사용(하드코딩 0 제거)
            GameSession* gs = cm.GetSession(sid);
            if (gs != nullptr)
            {
                gs->SetLastActivityMs(::GetTickCount64());   // 진입 = 첫 활동. sweep 기준 시작점 (프로세스 전역 시계 - 세션은 채널을 넘나들어 채널별 타이머로 찍으면 cross-epoch 언더플로)

                // 본인에게 자기 캐릭터 전체 초기 상태 통지(맵/위치 + hp 포함). 신규=랜덤 스폰, 복귀=DB 복원 위치/hp라 클라가 자기 상태를 알 방법이 이것뿐.
                //   (다른 객체 SC_SPAWN은 보내도 자기 자신 SC_SPAWN은 안 보내므로 - 자기 전용 통지. 이후 포탈은 SendMapChange.)
                Player* p = cm.GetPlayer(sid);
                if (p != nullptr)
                {
                    SendCharInfo(p);   // 본인에게 자기 캐릭터 전체 초기 상태 통지 (로그인 - hp 포함, MapleStory getCharInfo 정합)
                }
            }
        }

        // 인게임 채널 변경 도착측 - 대상 채널 스레드가 실행(MigrateChannel이 post한 enter job).
        //   순서: RESULT 먼저(즉시 Send, 클라 옛 채널 시야 reset) -> 새 그리드 등록(새 id+시야 배치) -> CharInfo(새 id/위치).
        //   RESULT(즉시)가 배치 spawn(tick-end flush)보다 먼저 도착하므로 클라 reset이 새 spawn을 지우지 않는다.
        void Channel::OnChannelChangeEnter(UINT64 sid)
        {
            ChannelManager& cm = ChannelManager::GetInstance();
            GameSession* gs = cm.GetSession(sid);
            Player* p = cm.GetPlayer(sid);
            if (gs == nullptr || p == nullptr)
            {
                return;   // 이관 사이 끊김 - leave 경로가 정리(enter job과 leave job은 같은 mailbox FIFO라 순서 보존)
            }
            // 소유권 재검사 (도착측 대칭 가드) - 이 enter job이 큐에 대기하는 사이 세션이 또 다른 채널로 재이관됐으면(gs->GetChannelId()!=m_channelId)
            //   이 채널은 더는 소유자가 아니므로 grid 삽입 금지. 없으면 stale enter가 이미 넘어간 세션을 이 채널 그리드에 이중 insert -> UAF.
            if (gs->GetChannelId() != m_channelId)
            {
                return;
            }
            SendChannelChangeResult(gs, EChannelChangeResult::OK, m_channelId);   // 클라 GameState reset 트리거
            gs->SetLastActivityMs(::GetTickCount64());                           // 진입 = 활동 갱신 (sweep 회피, 프로세스 전역 시계)
            m_mapManager.AddPlayer(sid, &cm);                                    // 새 playerId 발급 + 새 채널 시야 양방향 spawn
            SendCharInfo(p);                                                     // 새 id/위치/hp 통지 (AddPlayer가 p에 새 id 세팅)
        }

        // 채널 이동으로 떠남 (세션 생존) - 레이어 B에서 셀 이탈/주변 디스폰 통지 예정.
        void Channel::OnChannelLeave(UINT64 sid)
        {
            (void)sid;
        }

        // 연결 종료로 떠남 (세션 사망) - 주변 디스폰 통지, DB 저장, 이름 해제 후 게임 상태를 회수한다.
        void Channel::OnClientLeave(UINT64 sid, EDisconnectReason reason)
        {
            ChannelManager& cm = ChannelManager::GetInstance();   // 싱글턴 직접

            // 소유권 검사 - disconnect는 관측 시점의 채널로 라우팅되는데, 그 사이 MigrateChannel이 소유권을 옮겼을 수 있다.
            //   이 채널이 더는 소유자가 아니면(이관됨) 현 소유 채널로 leave를 재라우팅한다. 그 채널이 자기 그리드에서 제거 + free 해야
            //   도착 채널 그리드에 dangling Player*가 남지 않는다(cross-thread use-after-free 봉인). 세션은 소유 채널이 free할 때까지 map에 생존.
            {
                GameSession* owner = cm.GetSession(sid);
                if (owner != nullptr && owner->GetChannelId() != m_channelId)
                {
                    cm.RerouteLeave(sid, reason);
                    return;
                }
            }

            // 파괴 전에 주변 관찰자에게 SC_DESPAWN 통지 (회수 후엔 GetPlayer가 not-found)
            //   leave 디스폰 통지는 여기서만 (일원화). MapManager::Despawn은 셀 unlink/풀 반납만 (통지 X).
            wchar_t leaverName[32] = L"-";   // 아래 leave 로그용 - Player 가 풀에 반납되기 전에 복사해 둔다 (반납 후 참조 금지)
            Player* leaver = cm.GetPlayer(sid);
            if (leaver != nullptr)
            {
                ::wcsncpy_s(leaverName, _countof(leaverName), leaver->GetName(), _TRUNCATE);
                CancelTradeFor(leaver, static_cast<BYTE>(ETradeResult::PARTNER_LEFT));   // 거래 중 접속 종료 - 상대에게 이탈 통지 + 양측 리셋

                m_mapManager.DespawnVisibilityForOnLeave(leaver);  // leaver 맵의 시야 범위 관찰자에게 SC_DESPAWN

                DBThread* dbThread = &DBThread::GetInstance();
                if (dbThread != nullptr)
                {
                    SavePlayerFull(leaver, dbThread);   // 캐릭터 + 인벤 전량 (로그아웃 = 마지막 기회)
                }

                SharedServicesThread* sst = &SharedServicesThread::GetInstance();
                if (sst != nullptr)
                {
                    sst->EnqueueJob(new UnregisterNameJob(leaver->GetName(), sid));   // 이름+sid (저장 sid 일치 시에만 erase - 재로그인 race 봉인)
                }
            }

            m_mapManager.RemovePlayer(sid, &cm);           // [순서 고정] 반드시 아래 OnSessionGone 보다 먼저 - broadcast 가 격자에서 길어온 Player 의 역포인터로 수신자를 잡으므로,
            cm.OnSessionGone(sid);                         //   격자에서 먼저 빼야 세션 정리 구간(map erase ~ 역포인터 clear)에 그 Player 가 수신자로 안 잡힌다. 순서를 바꾸면 오송신.

            // 이 채널이 실제 처리한 leave 를 reason 별 집계 (rerouted leave 는 위에서 return 되어 제외).
            //   카운터는 로그와 별개로 유지 - 대량 이탈 버스트에서 로그가 drop 돼도 분포는 메트릭으로 생존한다.
            const int r = static_cast<int>(reason);
            if (r >= 0 && r < LEAVE_REASON_COUNT) { ++m_leaveReasonCount[r]; }

            // 이탈 이력 - 사유 포함 (사후 분석의 1차 자료. 중복 로그인 축출은 NET_RESET 으로 나가며 kick 발행 로그와 sid 로 대조)
            KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                KYS::GAMESERVER::LOG::LogChannel::USER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"leave",
                L"sid=%llu name=%s ch=%d reason=%s",
                sid, leaverName, m_channelId, DisconnectReasonName(reason));
        }

        // 프로토콜 위반 세션 끊기 - sid로 게임세션을 찾아 있으면 PROTOCOL_VIOLATION 으로 끊는다.
        //   OnRecv 안에서 헤더 미달/알 수 없는 opcode 두 지점이 같은 처리를 하므로 한곳으로 모은다.
        //   뒤따르는 return/break 는 문맥마다 달라 호출부에 남긴다.
        static void DisconnectForProtocolViolation(ChannelManager& cm, UINT64 sid)
        {
            GameSession* gs = cm.GetSession(sid);
            if (gs)
            {
                gs->Disconnect(EDisconnectReason::PROTOCOL_VIOLATION);
            }
        }

        // 완성 패킷 1개 처리 - opcode를 뽑아 게이트 검사 후 해당 핸들러로 분기한다.
        void Channel::OnRecv(UINT64 sid, const BYTE* data, int size)
        {
            ChannelManager& cm = ChannelManager::GetInstance();   // 싱글턴 직접
            // 모니터: OnRecvJob 1건 = 패킷 1개 처리. 이 Channel 스레드 단독 writer(무락).
            ++m_packetCount;
            m_bytesRecv += static_cast<UINT64>(size);

            GameSession* gsAct = cm.GetSession(sid);
            if (gsAct != nullptr)
            {
                gsAct->SetLastActivityMs(::GetTickCount64());   // 살아있는 클라는 패킷마다 갱신 - 회수 회피 (프로세스 전역 시계)
            }

            // (1) 최소 헤더 가드 - type 필드(offset 2)를 읽기 전 헤더 크기 보장
            if (size < static_cast<int>(sizeof(PacketHeader)))
            {
                DisconnectForProtocolViolation(cm, sid);
                return;
            }

            // (2) opcode 추출 - wire는 BE라 host로 ntohs (CPacket::Begin htons와 대칭)
            const PacketHeader* header = reinterpret_cast<const PacketHeader*>(data);
            PacketType opcode = static_cast<PacketType>(ntohs(header->type));

            // (3) opcode switch (함수포인터 테이블 m_handlers[] 없음). 인증 게임세션만 여기 도달(미인증은 ChannelManager pre-auth 분기).
            //   pre-auth 핸드셰이크 opcode(아래 no-op 묶음)는 admission 직후 stale/중복으로 여기 닿을 수 있어 무시한다.
            //   그 외 알 수 없는 opcode 만 default 로 거부(CS_LOGIN 등 게임 채널에 올 수 없는 것 포함).
            switch (opcode)
            {
            case PacketType::CS_MOVE:
                OnMove(sid, data, size);
                break;

            case PacketType::CS_PING:
                OnPing(sid, data, size);
                break;

            case PacketType::CS_PORTAL:
                OnPortal(sid, data, size);
                break;

            case PacketType::CS_CHANNEL_CHANGE:
                OnChannelChange(sid, data, size);   // 인게임 채널 변경 (in-place handoff 출발측)
                break;

            case PacketType::CS_CHANNEL_LIST_REQUEST:
                OnChannelListRequest(sid);   // 인게임 picker 인구 재조회 (payload 없음 - 현재 인구 스냅샷 회신)
                break;

            case PacketType::CS_SKILL:
                OnSkill(sid, data, size);
                break;

            case PacketType::CS_CHAT:
                OnChat(sid, opcode, data, size);
                break;

            case PacketType::CS_WHISPER:
                OnChat(sid, opcode, data, size);   // 같은 핸들러 - OnChat 내부서 opcode로 갈림
                break;

            case PacketType::CS_ITEM_PICKUP:
                OnItemPickup(sid, data, size);   // 바닥 아이템 줍기 (거리/소유권/공간 서버 검증)
                break;

            case PacketType::CS_ITEM_MOVE:
                OnItemMove(sid, data, size);     // 가방 칸 이동 (스택 합류/스왑)
                break;

            case PacketType::CS_ITEM_USE:
                OnItemUse(sid, data, size);      // 소모품 사용 (HP 회복 + 수량 감소)
                break;

            case PacketType::CS_ITEM_EQUIP:
                OnItemEquip(sid, data, size);    // 장착 (가방 -> 무기/방어구 칸)
                break;

            case PacketType::CS_ITEM_UNEQUIP:
                OnItemUnequip(sid, data, size);  // 장착 해제 (장착 칸 -> 가방 빈 칸)
                break;

            case PacketType::CS_ITEM_DISCARD:
                OnItemDiscard(sid, data, size);  // 버리기 (가방 -> 자기 위치 바닥 드랍)
                break;

            case PacketType::CS_TRADE_REQUEST:
                OnTradeRequest(sid, data, size);     // 거래 요청 (같은 맵 이름 상대)
                break;

            case PacketType::CS_TRADE_RESPONSE:
                OnTradeResponse(sid, data, size);    // 받은 요청 수락/거절
                break;

            case PacketType::CS_TRADE_ADD_ITEM:
                OnTradeAddItem(sid, data, size);     // 거래창에 오퍼 올리기
                break;

            case PacketType::CS_TRADE_REMOVE_ITEM:
                OnTradeRemoveItem(sid, data, size);  // 거래창에서 오퍼 내리기
                break;

            case PacketType::CS_TRADE_ACCEPT:
                OnTradeAccept(sid, data, size);      // 내 수락 표시 (양측 수락 시 커밋)
                break;

            case PacketType::CS_TRADE_CANCEL:
                OnTradeCancel(sid, data, size);      // 거래 취소
                break;

            case PacketType::CS_GAME_AUTH:
            case PacketType::CS_CHANNEL_SELECT:
            case PacketType::CS_CHARACTER_SELECT:
            case PacketType::CS_CHARACTER_CREATE:
            case PacketType::CS_CHARACTER_DELETE:
                // pre-auth 핸드셰이크의 stale 중복 - admission 직후 도착(F17 defer 창에서 재시도하던 select가
                //   admit 되는 순간 닿는 등). 이미 게임세션이 선 상태라 무의미하므로 조용히 무시한다.
                //   여기를 default(PROTOCOL_VIOLATION)로 보내면 방금 입장한 정상 세션을 자기 retry로 끊게 된다.
                break;

            default:
                // unknown opcode -> 프로토콜 위반 끊기
                DisconnectForProtocolViolation(cm, sid);
                break;
            }
        }

        // CS_PING - 받은 clientTimeMs를 SC_PONG으로 그대로 되돌린다.
        void Channel::OnPing(UINT64 sid, const BYTE* data, int size)
        {
            ChannelManager& cm = ChannelManager::GetInstance();   // 싱글턴 직접
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (size < headerSize + 4)   // CS_PING payload 4B(clientTimeMs) 미만 = 절단 -> 버림 (형제 핸들러 가드와 대칭)
            {
                return;
            }

            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);   // 헤더 skip, payload 적재
            CS_PING req;
            req.Deserialize(pkt);

            GameSession* gs = cm.GetSession(sid);
            if (gs == nullptr || gs->GetChannelId() != m_channelId)
            {
                return;   // 옛 sid / 다른 채널로 이관됨 (정상 race, 조용히 버림)
            }

            // SC_PONG 되돌림. 활동시각 갱신은 OnRecv 진입부가 이미 처리.
            SC_PONG pong;
            pong.clientTimeMs = req.clientTimeMs;
            SendToSession(gs, PacketType::SC_PONG, pong);
        }

        // CS_MOVE - 클라가 보고한 위치를 서버 dead-reckon 예측과 대조해 수용하거나 보정한다.
        void Channel::OnMove(UINT64 sid, const BYTE* data, int size)
        {
            // (1) 역직렬화 + direction 검증
            const int headerSize = static_cast<int>(sizeof(PacketHeader));   // = 4
            if (size < headerSize + 14) { return; }   // payload 14B 미만 = 절단 -> 버림 (Deserialize OOB read 차단)
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);   // payload만 적재
            CS_MOVE req;
            req.Deserialize(pkt);                              // moveState, direction, x, y, clientSeq
            if (static_cast<unsigned int>(req.direction) >= static_cast<unsigned int>(EMoveDirection::COUNT)) { return; }   // 8방(0..7) 밖 = 변조 클라 - LUT 인덱스 OOB 차단

            // (2) sid -> Player (소유 채널 검사 - 이관으로 다른 채널 소유가 된 세션의 잔여 잡이 이 채널에서 실행되는 것을 차단)
            //     GetSession 한 번으로 소유권 확인 + Player 획득 (GetPlayer도 내부에서 GetSession하므로 락 추가 없음).
            ChannelManager& cm = ChannelManager::GetInstance();
            Player* p = ResolveOwnedPlayer(cm, sid);
            if (p == nullptr) { return; }
            if (p->IsDead()) { return; }   // 사망 중 이동 봉인 - 시신 위치/moveState 가 입력으로 움직이지 않게 (OnSkill 사망 게이트와 대칭. 사망 시 서버가 STOP 확정이라 드롭해도 desync 없음)

            // (2.5) clientSeq 단조 가드 - CS_MOVE 2-lane 분리(STOP=critical / START=movement)는 같은 세션
            //   이동 패킷의 처리 순서를 역전시킬 수 있다: STOP 이 critical 로 먼저 drain 되고, 뒤늦게 도착한 stale START 가
            //   서버 상태를 다시 START 로 되돌리면, 정지한 클라(B0 event-driven 이라 추가 CS_MOVE 없음)의 아바타가 맵끝까지
            //   무한 dead-reckon 표류한다. TCP 라 원래 순서는 보존되므로 이미 처리한 seq 이하의 이동은 stale 로 버린다(최신 의도만 반영).
            if (req.clientSeq <= p->GetLastSeq()) { return; }

            // (3) dead-reckon 대조로 위치 수용(클램프)/보정(서버 권위) + 바뀜 표시
            AcceptOrCorrectPosition(p, req);

            // (4) 예측용 방향 상태 + 함께 보낼 기준 seq 갱신 (Send 0 - broadcast 송신은 TickLoop의 SendObjectUpdates이)
            p->SetMoveState(req.moveState, req.direction);
            p->SetLastSeq(req.clientSeq);
        }

        // CS_PORTAL - portalId를 서버 포탈 표로 검증(맵/근접)한 뒤 목적지로 이동시킨다.
        void Channel::OnPortal(UINT64 sid, const BYTE* data, int size)
        {
            // 1. 역직렬화 - 헤더 4B 뒤 payload(portalId 4B)만 떼어 CS_PORTAL 복원.
            const int headerSize = static_cast<int>(sizeof(PacketHeader));   // = 4
            if (size < headerSize + 4) { return; }   // payload 4B(portalId) 미만 = 절단 -> 버림 (OOB read 차단)
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_PORTAL body;
            body.Deserialize(pkt);

            // 2. 플레이어 + 포탈 트리거 서버 권위 검증 (범위 밖 / 다른 맵 / 트리거 반경 밖이면 무시 = 핵 방지).
            //    소유 채널 검사 - 이관으로 다른 채널 소유가 된 세션의 잔여 포탈 잡이 이 채널 그리드를 만지는 것을 차단(이중 grid insert 봉인).
            ChannelManager& cm = ChannelManager::GetInstance();
            Player* p = ResolveOwnedPlayer(cm, sid);
            if (p == nullptr) { return; }
            if (p->IsDead()) { return; }   // 사망 중 포탈 봉인 - 시신 텔레포트 차단 (스킬/아이템/거래와 같은 사망 정책)

            // 포탈 도배 방어 - 통과 사이 최소 간격 미달이면 드롭(맵 이동 + 즉시 DB 저장 폭주 차단).
            if (!p->TryAct(EPlayerAction::PORTAL, ::GetTickCount64(), static_cast<UINT64>(PORTAL_MIN_INTERVAL_MS)))
            {
                return;
            }

            const PortalDef* portal = ResolvePortal(p, body.portalId);
            if (portal == nullptr) { return; }

            // 거래 중 맵 이동 - 같은 맵 게이트가 깨지므로 이동 직전 거래를 종결한다(상대 이탈 통지).
            CancelTradeFor(p, static_cast<BYTE>(ETradeResult::PARTNER_LEFT));

            // 3. 서버 표의 도착 맵/좌표로 이동. 무효 dst/동일맵이면 no-op(false) - 무이동 통지/저장을 막는다.
            if (!m_mapManager.ChangeMap(p, portal->dstMapId, portal->dst)) { return; }

            // 4. 본인에게 새 맵/위치 통지 (클라가 자기 아바타를 도착 좌표로 재배치).
            SendMapChange(p);

            // 5. 맵 이동은 고가치 이벤트 - 즉시 저장(autosave 60초 주기를 안 기다림, 크래시 시 위치 손실 0).
            DBResult moveSnap{};
            FillCharacterSnapshot(p, moveSnap);
            DBThread::GetInstance().Enqueue(new SaveFullCommand(moveSnap));
        }

        // CS_CHANNEL_LIST_REQUEST - 인게임 채널변경 picker가 최신 인구를 요청 (payload 없음).
        //   로그인 때 캐시된 목록은 stale(각 클라의 로그인 순간 이후 입장자 미반영) - picker 열 때마다 현재 인구 스냅샷을 회신한다.
        //   요청자는 admission 완료 상태라 스냅샷에 자기 자신도 포함된다(로그인 push 는 admission 전이라 자기 미포함이었음).
        void Channel::OnChannelListRequest(UINT64 sid)
        {
            ChannelManager& cm = ChannelManager::GetInstance();
            Player* p = ResolveOwnedPlayer(cm, sid);
            if (p == nullptr) { return; }
            GameSession* gs = p->GetGameSession();   // 아래 SC_CHANNEL_LIST 즉시 Send 용 (p 를 푼 그 세션)
            // 채널 목록 재조회 도배 방어 - 매 요청이 패킷 빌드+Send 를 유발.
            if (!p->TryAct(EPlayerAction::CHANNEL_LIST, ::GetTickCount64(), static_cast<UINT64>(CHANNEL_LIST_MIN_INTERVAL_MS)))
            {
                return;
            }
            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            cm.BuildChannelListPacket(out);
            gs->SendScrambled(out.GetBuffer(), out.GetSize());   // 자기 세션 즉시 Send (lease 불요 - SendChannelChangeResult 선례)
        }

        // CS_CHANNEL_CHANGE - 인게임 채널 변경 출발측(현 채널 스레드). in-place handoff.
        //   검증 -> A 그리드 despawn/제거 -> mover 배치 quiesce(cross-thread 레이스 봉인) -> MigrateChannel(락: count 스왑+SetChannel+B enter post).
        //   성공 RESULT(OK)는 B의 enter job이 보낸다(클라 reset이 새 spawn보다 앞서게). 여기선 실패 RESULT만 즉시 통지.
        void Channel::OnChannelChange(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));   // = 4
            if (size < headerSize + 1) { return; }   // payload 1B(channelId) 미만 = 절단 -> 버림 (OOB read 차단)
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_CHANNEL_CHANGE req;
            req.Deserialize(pkt);   // channelId
            const int targetCh = static_cast<int>(req.channelId);

            ChannelManager& cm = ChannelManager::GetInstance();
            GameSession* gs = cm.GetSession(sid);
            Player* p = cm.GetPlayer(sid);
            if (gs == nullptr || p == nullptr)
            {
                SendChannelChangeResult(gs, EChannelChangeResult::NOT_IN_GAME, m_channelId);   // gs null이면 헬퍼 내부 가드로 no-op
                return;
            }
            if (gs->GetChannelId() != m_channelId)
            {
                // 이미 다른 채널로 이관된 세션의 중복/잔여 요청 - 조용히 드롭 (첫 이관이 도착측에서 OK를 이미 통지함).
                //   재진입 가드: 이게 없으면 같은 tick 크리티컬 lane의 중복 CS_CHANNEL_CHANGE가 이중 migrate -> count drift/이중 grid insert(UAF).
                return;
            }
            if (targetCh == m_channelId)
            {
                SendChannelChangeResult(gs, EChannelChangeResult::SAME, m_channelId);
                return;
            }
            if (!cm.IsChannelJoinable(targetCh))
            {
                // IsChannelJoinable은 범위 밖과 cap 초과 모두 false - 범위로 INVALID/FULL 구분해 통지.
                const bool inRange = (targetCh >= 0 && targetCh < cm.GetChannelCount());
                SendChannelChangeResult(gs, inRange ? EChannelChangeResult::FULL : EChannelChangeResult::INVALID, m_channelId);
                return;
            }

            // 채널 변경 도배 방어 - 비싼 in-place handoff(A 그리드 despawn + migrate) 연타 차단. 사람 클릭은 이 간격을 안 넘는다.
            //   SAME/범위밖/FULL 은 위에서 이미 걸러졌으니 실제 이관 직전에만 쿨다운을 소비한다(정상 목록->선택 UX 보존).
            if (!p->TryAct(EPlayerAction::CHANNEL_CHANGE, ::GetTickCount64(), static_cast<UINT64>(CHANNEL_CHANGE_MIN_INTERVAL_MS)))
            {
                SendChannelChangeResult(gs, EChannelChangeResult::RATE_LIMITED, m_channelId);   // 쿨다운 거부를 클라에 통지 (무통지 = 거짓 성공 오해 방지). gs non-null(810 검증)
                return;
            }

            // 거래 중 채널 변경 - 상대는 이 채널에 남고 나만 떠나므로 이관 시작 전 거래를 종결한다(상대 이탈 통지).
            CancelTradeFor(p, static_cast<BYTE>(ETradeResult::PARTNER_LEFT));

            // --- 실제 이관 시작 --- 검증 통과 후: A 그리드 despawn/제거 + 배치 quiesce + migrate (순서 민감 = cross-thread race seal).
            BeginChannelHandoff(sid, gs, p, targetCh, cm);
        }

        // 채널 이관 실행 - OnChannelChange 가 모든 검증(소유/조인가능/쿨다운)을 끝낸 뒤 부르는 순서 민감 tail.
        //   순서 고정(cross-thread race seal): despawn 통지 전에 그리드 unlink 금지, 배치 quiesce 전에 migrate 금지, migrate 는 항상 마지막.
        void Channel::BeginChannelHandoff(UINT64 sid, GameSession* gs, Player* p, int targetCh, ChannelManager& cm)
        {
            // (1) A 그리드에서 despawn 통지 + 제거 (Player는 GameSession 소유라 파괴 안 됨, 소속만 빠짐).
            m_mapManager.DespawnVisibilityForOnLeave(p);   // A의 시야 관찰자에게 SC_DESPAWN
            m_mapManager.RemovePlayer(sid, &cm);           // A 그리드 셀 unlink (통지는 위에서 이미 함)

            // (2) mover 배치 quiesce - A의 tick-end flush가 B의 배치 append와 레이스하지 않게 지금 비우고 flush 대상에서 제외.
            gs->FlushBatch();               // 잔여 A 배치 즉시 Send (m_batchPos -> 0)
            RemoveFromFlushList(gs);        // A의 tick-end flush 순회에서 제외
            gs->SetBatchQueued(false);      // 재등록 가드 리셋

            // (2.5) 보수적 인벤 dirty - 이관 중 in-flight 인벤 저장(autosave/거래 커밋)의 실패 회신(InventorySaveFailedJob)은
            //   발행 채널 소유권 가드에서 무음 drop 되고, 새 채널 autosave 는 dirty 게이트라 그 재표시를 이어받지 못한다.
            //   이관 시점에 인벤 dirty 를 켜 두면 새 채널의 다음 주기 전량 재저장이 그 창을 봉인한다(전량 교체 저장 = 멱등, 비용은 잉여 저장 1회).
            p->MarkDbDirty(DB_DIRTY_INVENTORY);

            // (3) count 스왑 + SetChannel + B enter job post (한 write 락). 실패(이관 사이 세션 소멸)면 이미 A 그리드서 뺐고,
            //     종료 경로(OnClientLeave)의 RemovePlayer가 두 번 불려도 무해(map miss no-op)해 정리된다.
            cm.MigrateChannel(sid, m_channelId, targetCh);
        }

        // CS_SKILL - 서버 권위로 스킬 판정: 자원 소비, 주변 몬스터 타격, SC_DAMAGE/SC_DEATH 송신.
        void Channel::OnSkill(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));   // = 4
            if (size < headerSize + 4) { return; }   // payload 4B(skillId) 미만 = 절단 -> 버림 (OOB read 차단)

            // (1) 역직렬화
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_SKILL req;
            req.Deserialize(pkt);   // skillId 단일

            // (2) caster resolve + 검증 (옛 sid / 죽은 caster는 시전 불가 / 다른 채널로 이관됨)
            ChannelManager& cm = ChannelManager::GetInstance();
            Player* caster = ResolveOwnedPlayer(cm, sid);
            if (caster == nullptr) { return; }
            if (caster->IsDead()) { return; }   // 사망 중 공격 봉인

            // (3) 자원 소비 - 공용 쿨다운/MP. 실패 시 시전 무효
            if (!caster->ConsumeForSkill(req.skillId)) { return; }

            // (4) 타겟 후보 = caster 시야 (타격은 SKILL_HIT_RANGE로 좁힘)
            std::vector<GameObject*> candidates;
            m_mapManager.GetNearbyObjectsFor(caster, VIEW_RANGE, candidates);
            const Position cpos = caster->GetPos();

            // (5) 사거리 안 살아있는 몬스터마다 타격 + SC_DAMAGE/SC_DEATH 송신
            for (std::vector<GameObject*>::iterator it = candidates.begin(); it != candidates.end(); ++it)
            {
                GameObject* target = *it;
                if (target == caster) { continue; }                                 // 자기 자신 제외
                if (target->GetObjectType() != EObjectType::MONSTER) { continue; }   // PvE - monster만 (player 무피해)
                if (target->IsDead()) { continue; }                                 // 시신 재타격 금지

                // 사거리 필터 (거리 제곱 비교 - sqrt 회피, 오버플로 방지 long long)
                const Position tpos = target->GetPos();
                const long long dx = static_cast<long long>(tpos.x) - cpos.x;
                const long long dy = static_cast<long long>(tpos.y) - cpos.y;
                if (dx * dx + dy * dy > static_cast<long long>(SKILL_HIT_RANGE) * SKILL_HIT_RANGE) { continue; }   // 사거리 밖

                // 벽 관통 차단 - caster<->target 사이 시야선이 벽에 막히면 타격 무효(스킬 사거리 > 벽 두께라 벽 너머 타격이 가능했던 구멍 봉인)
                if (!HasLineOfSight(caster->GetMapId(), cpos, tpos)) { continue; }

                ApplySkillHit(caster, target);
            }
            // 주의: SendObjectUpdates에 전투 분기를 추가하지 않는다 - 이벤트는 핸들러가 직접 보낸다.
        }

        // AOI 목록 중 연결 있는 player 수신자에게만 패킷을 B4 배치 누적한다 (즉시 Send 대신 tick-end flush).
        //   이 세션 포인터는 tick-end flush 까지 m_flushList 에 남는다(이관 시 BeginChannelHandoff 의 RemoveFromFlushList 가 걷어낸다).
        void Channel::BatchToPlayersInAoi(const std::vector<GameObject*>& aoi, const BYTE* buf, int size)
        {
            for (std::vector<GameObject*>::const_iterator r = aoi.begin(); r != aoi.end(); ++r)
            {
                if ((*r)->GetObjectType() != EObjectType::PLAYER) { continue; }   // 아래 캐스팅의 전제(옮기거나 넓히면 안 된다) + monster GetId()는 wire id라 sid 아님
                Player* to = static_cast<Player*>(*r);   BatchTo(to->GetGameSession(), to, buf, size);   // 역포인터 (PLAYER 가드 뒤 - 전역 SRWLock 우회) + 짝 검사. Map 의 Stage* 는 tick 경계를 넘어 sid 를 적재한다
            }
        }

        // 한 타겟에 데미지 적용 + SC_DAMAGE를 대상 시야 범위 관찰자(caster 포함)에 송신, 사망 전이(alive -> dead) 시에만 SC_DEATH 1회.
        //   수신자 앵커 = 피격 대상 - 피해 표시는 대상을 보는 이들이 봐야 한다(caster AOI 재사용 시 대상만 보이는 관찰자가 누락).
        void Channel::ApplySkillHit(Player* caster, GameObject* target)
        {
            const UINT32 atkId = caster->GetPlayerId();                     // caster는 항상 player
            const UINT32 targetId = static_cast<UINT32>(target->GetId());   // target은 monster - 통합 단조 id를 UINT32로

            // (6) 데미지 산출 + 적용 - 사망은 상태가 아니라 전이로 판정
            const int dmg = CombatFormula::Compute(caster, target);
            const bool wasAlive = !target->IsDead();
            const int applied = target->TakeDamage(dmg);
            const int remain = (target->GetHp() < 0) ? 0 : target->GetHp();

            // 드랍 소유권 재료 - 실제 적용된 데미지를 기여로 누적 (사망 시 상위 3명이 계단식 우선권).
            if (applied > 0 && target->GetObjectType() == EObjectType::MONSTER)
            {
                static_cast<Monster*>(target)->AddDamageCredit(caster->GetCharId(), applied);
            }

            // (7) 수신자 = 피격 대상 시야 범위 관찰자 (대상 위치는 이 호출 동안 고정 -> 피해/사망 배치에 재사용)
            std::vector<GameObject*> viewers;
            m_mapManager.GetNearbyObjectsFor(target, VIEW_RANGE, viewers);

            // (8) SC_DAMAGE 직렬화 (대상당 1회) + 대상 AOI player 전원 배치
            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            if (!BuildDamagePacket(out, atkId, targetId, dmg, remain)) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); }   // damage=원시 판정치 - applied(잔여 hp로 잘린 값)면 막타가 늘 "몬스터 잔여 hp"로 표시됨. hp 동기는 remainingHp 담당
            else { BatchToPlayersInAoi(viewers, out.GetBuffer(), out.GetSize()); }

            // (9) 사망 전이에서만 1회 SC_DEATH - 같은 대상 AOI 재사용
            if (wasAlive && target->IsDead())
            {
                static_cast<Monster*>(target)->MarkDead();   // DEAD 전이 + 유예 타이머 시작
                KYS::GAMECOMMON::PROTOCOL::CPacket deathPkt(MAX_PACKET_SIZE);
                if (!BuildDeathPacket(deathPkt, targetId)) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", deathPkt.GetType(), deathPkt.GetSize()); }
                else { BatchToPlayersInAoi(viewers, deathPkt.GetBuffer(), deathPkt.GetSize()); }

                GenerateDrops(target);   // (10) 사망 확정 지점에서만 드랍 추첨 - 기여 상위 3명이 우선권
            }
        }

        // 몬스터 사망 확정 시 드랍 표를 추첨해 바닥 드랍을 만든다. 행마다 독립 추첨(rAthena 방식)이라
        //   한 번에 여러 종류가 떨어질 수 있다. 좌표는 사망 지점 주변으로 흩어 놓는다(겹침 방지).
        void Channel::GenerateDrops(GameObject* deadMonster)
        {
            Monster* monster = static_cast<Monster*>(deadMonster);

            UINT32 owners[3];
            monster->GetTopDamageCredits(owners);   // 데미지 기여 1~3순위 (계단식 소유권 재료)

            const UINT64 nowMs = m_tickTimer.GetCurrentTimeMS();
            const EMonsterType deadType = monster->GetMonsterType();
            const Position deadPos = deadMonster->GetPos();
            const int mapId = deadMonster->GetMapId();

            const int ruleCount = DropTableCount();
            for (int i = 0; i < ruleCount; ++i)
            {
                const DropRule& rule = DropRuleAt(i);
                if (rule.monsterType != deadType) { continue; }
                if (CombatFormula::RandomRange(1, 1000) > rule.chancePermil) { continue; }   // 천분율 추첨

                const int quantity = CombatFormula::RandomRange(rule.minQty, rule.maxQty);
                Position dropPos = deadPos;                                            // 사망 지점 주변 분산
                dropPos.x += CombatFormula::RandomRange(-CELL_SIZE / 4, CELL_SIZE / 4);
                dropPos.y += CombatFormula::RandomRange(-CELL_SIZE / 4, CELL_SIZE / 4);
                // 분산(+-62px)이 벽 칸/맵 밖에 떨어지면 사망 지점으로 되돌린다 - 몬스터가 서 있던 칸이라 통행 보장.
                //   원점을 아는 건 여기뿐이라 세탁도 여기서 한다 (SpawnGroundItem 은 분산 후 좌표만 받는다).
                if (!IsWalkable(mapId, dropPos.x, dropPos.y)) { dropPos = deadPos; }
                m_mapManager.SpawnGroundItem(mapId, dropPos, rule.templateId, quantity, owners, nowMs);
            }
        }

        // 세션 소유(채널 일치) + Player 조회 - IsDead 는 여기서 검사하지 않는다(사망 정책은 핸들러별: 채팅/목록은 사망 중 허용, 이동/포탈/스킬은 각 핸들러의 사망 게이트가 차단). 실패 = nullptr.
        //   OnMove/OnPortal/OnSkill/OnChat/OnChannelListRequest 의 공통 소유권 가드.
        Player* Channel::ResolveOwnedPlayer(ChannelManager& cm, UINT64 sid)
        {
            GameSession* gs = cm.GetSession(sid);
            if (gs == nullptr || gs->GetChannelId() != m_channelId) { return nullptr; }   // 옛 sid / 타 채널 (조용히 버림)
            Player* p = gs->GetPlayer();
            if (p == nullptr) { return nullptr; }
            return p;
        }

        // 아이템 핸들러 공용 진입 검증 - 세션 소유(채널 일치) + Player 존재 + 생존. 실패 = nullptr.
        Player* Channel::ResolveItemSender(UINT64 sid, GameSession*& outGs)
        {
            outGs = ChannelManager::GetInstance().GetSession(sid);
            if (outGs == nullptr || outGs->GetChannelId() != m_channelId) { return nullptr; }   // 옛 sid / 타 채널 (조용히 버림)
            Player* p = outGs->GetPlayer();
            if (p == nullptr || p->IsDead()) { return nullptr; }   // 미부착 / 사망 중엔 아이템 조작 불가
            return p;
        }

        // 아이템 핸들러 공통 진입 - ResolveItemSender 로 세션/생존 검증 후 거래 중이면 LOCKED_IN_TRADE 통지하고 nullptr.
        Player* Channel::BeginItemHandler(UINT64 sid, GameSession*& outGs)
        {
            Player* p = ResolveItemSender(sid, outGs);
            if (p == nullptr) { return nullptr; }
            if (p->GetTrade().IsTrading()) { SendItemResult(outGs, static_cast<BYTE>(EItemResult::LOCKED_IN_TRADE)); return nullptr; }   // 거래 중엔 인벤 잠금
            return p;
        }

        // 아이템 조작 실패 사유 통지 (성공은 SC_ITEM_UPDATE/SC_INVENTORY 가 대신한다).
        void Channel::SendItemResult(GameSession* gs, BYTE result)
        {
            SC_ITEM_RESULT body;
            body.result = result;
            SendToSession(gs, PacketType::SC_ITEM_RESULT, body);
        }

        // 인벤 전량 스냅샷 통지 - 줍기(합류 슬롯이 여럿일 수 있음)와 거래 완료(변경 폭이 큼)가 사용.
        void Channel::SendFullInventory(GameSession* gs, const Player* p)
        {
            const Inventory& inv = p->GetInventory();
            SC_INVENTORY body{};
            body.count = 0;
            for (int slot = 0; slot < MAX_INVENTORY_SLOTS; ++slot)
            {
                const ItemInstance& it = inv.At(slot);
                if (it.IsEmpty()) { continue; }
                ItemSlotEntry& e = body.items[body.count++];
                e.slot = static_cast<BYTE>(slot);
                e.uid = it.uid;
                e.templateId = it.templateId;
                e.quantity = it.quantity;
            }
            static const int kEquipSlots[2] = { EQUIP_SLOT_WEAPON, EQUIP_SLOT_ARMOR };
            for (int i = 0; i < 2; ++i)
            {
                const ItemInstance& it = inv.At(kEquipSlots[i]);
                if (it.IsEmpty()) { continue; }
                ItemSlotEntry& e = body.items[body.count++];
                e.slot = static_cast<BYTE>(kEquipSlots[i]);
                e.uid = it.uid;
                e.templateId = it.templateId;
                e.quantity = it.quantity;
            }
            SendToSession(gs, PacketType::SC_INVENTORY, body);
        }

        // 장착 변화 후 자기 공/방 통지 (클라 표시용 - 권위 계산은 어차피 서버가 매 타격마다 다시 한다).
        void Channel::SendStatUpdate(GameSession* gs, const Player* p)
        {
            SC_STAT_UPDATE body;
            body.atkPower = p->GetAttackPower();
            body.defPower = p->GetDefPower();
            SendToSession(gs, PacketType::SC_STAT_UPDATE, body);
        }

        // CS_ITEM_PICKUP - 바닥 아이템 줍기. 검증 순서: 실재 -> 거리 -> 소유권 -> 인벤 공간 (전부 서버 판정 - 클라 신뢰 0).
        void Channel::OnItemPickup(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));   // = 4
            if (size < headerSize + 4) { return; }   // objectId(4B) 없으면 절단 -> 버림
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_ITEM_PICKUP req;
            req.Deserialize(pkt);

            GameSession* gs = nullptr;
            Player* p = BeginItemHandler(sid, gs);
            if (p == nullptr) { return; }

            // (1) 실재 - 자기 맵에서 objectId 조회 (이미 사라졌으면 NOT_FOUND)
            GroundItem* item = m_mapManager.FindGroundItem(p->GetMapId(), req.objectId);
            if (item == nullptr) { SendItemResult(gs, static_cast<BYTE>(EItemResult::NOT_FOUND)); return; }

            // (2) 거리 - 멀리서 줍기 차단 (거리 제곱 비교. HeavenMS 는 이 검증이 없어 클라 신뢰 갭이었다 - 우리는 넣는다)
            const Position myPos = p->GetPos();
            const Position itemPos = item->GetPos();
            const long long dx = static_cast<long long>(myPos.x) - itemPos.x;
            const long long dy = static_cast<long long>(myPos.y) - itemPos.y;
            if (dx * dx + dy * dy > static_cast<long long>(PICKUP_RANGE) * PICKUP_RANGE)
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::TOO_FAR));
                return;
            }

            // (3) 소유권 - 계단식 보호 시간 (기여 1순위 3초 -> 2순위 5초 -> 3순위 7초 -> 자유)
            const UINT64 nowMs = m_tickTimer.GetCurrentTimeMS();
            if (!item->CanBePickedBy(p->GetCharId(), nowMs))
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::NOT_OWNER));
                return;
            }

            // (4) 인벤 공간 - 사전검증 통과 후에만 상태 변경 (부분 획득 없음)
            const int templateId = item->GetTemplateId();
            const int quantity = item->GetQuantity();
            if (!p->GetInventory().HasRoomFor(templateId, quantity))
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::INV_FULL));
                return;
            }

            // 확정 - 바닥에서 먼저 제거(관찰자 SC_DESPAWN, 같은 tick 이중 줍기 차단) 후 인벤 편입 + uid 발급.
            m_mapManager.DespawnGroundItem(item);
            p->GetInventory().Add(IssueItemUid(), templateId, quantity);
            p->MarkDbDirty(DB_DIRTY_INVENTORY);
            SendFullInventory(gs, p);   // 스택 합류가 여러 칸에 걸칠 수 있어 전량 스냅샷으로 통지
        }

        // 지정 칸들의 현재 내용을 통지 - 변경 폭이 정확한 조작(이동 2칸/사용 1칸/장착 2칸/버림 1칸) 전용.
        //   빈 칸은 uid 0 으로 실려 클라가 그 칸을 비운다.
        void Channel::SendItemUpdate(GameSession* gs, const Player* p, const int* slots, int slotCount)
        {
            const Inventory& inv = p->GetInventory();
            SC_ITEM_UPDATE body{};
            body.count = 0;
            for (int i = 0; i < slotCount && i < SC_ITEM_UPDATE::MAX_ENTRIES; ++i)
            {
                const ItemInstance& it = inv.At(slots[i]);
                ItemSlotEntry& e = body.items[body.count++];
                e.slot = static_cast<BYTE>(slots[i]);
                e.uid = it.uid;
                e.templateId = it.templateId;
                e.quantity = it.quantity;
            }
            SendToSession(gs, PacketType::SC_ITEM_UPDATE, body);
        }

        // CS_ITEM_MOVE - 가방 칸 이동 (같은 종류면 스택 합류, 아니면 스왑). 장착 칸은 Equip/Unequip 전용 경로.
        void Channel::OnItemMove(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (size < headerSize + 2) { return; }   // fromSlot(1B)+toSlot(1B)
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_ITEM_MOVE req;
            req.Deserialize(pkt);

            GameSession* gs = nullptr;
            Player* p = BeginItemHandler(sid, gs);
            if (p == nullptr) { return; }

            if (!p->GetInventory().Move(req.fromSlot, req.toSlot))
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT));
                return;
            }
            p->MarkDbDirty(DB_DIRTY_INVENTORY);
            const int slots[2] = { req.fromSlot, req.toSlot };
            SendItemUpdate(gs, p, slots, 2);
        }

        // CS_ITEM_USE - 소모품 사용. 종류 검증(CONSUME) -> 수량 1 감소 -> HP 회복 (상한 클램프).
        void Channel::OnItemUse(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (size < headerSize + 1) { return; }
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_ITEM_USE req;
            req.Deserialize(pkt);

            GameSession* gs = nullptr;
            Player* p = BeginItemHandler(sid, gs);
            if (p == nullptr) { return; }

            const ItemInstance& it = p->GetInventory().At(req.slot);
            if (it.IsEmpty()) { SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT)); return; }

            const ItemDef* def = FindItemDef(it.templateId);
            if (def == nullptr || def->itemType != EItemType::CONSUME)
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::WRONG_TYPE));   // 장비/기타는 사용 불가
                return;
            }
            const int hpRestore = def->hpRestore;   // RemoveAt 이 칸을 비울 수 있어 효과값 먼저 확보

            if (!p->GetInventory().RemoveAt(req.slot, 1))
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT));
                return;
            }
            int hp = p->GetHp() + hpRestore;
            if (hp > p->GetMaxHp()) { hp = p->GetMaxHp(); }   // 과회복 클램프
            p->SetHp(hp);
            p->MarkDbDirty(DB_DIRTY_INVENTORY);

            const int slots[1] = { req.slot };
            SendItemUpdate(gs, p, slots, 1);
            SendCharInfo(p);   // 회복된 hp 자기 통지 (기존 self-통지 경로 재사용)
        }

        // CS_ITEM_EQUIP - 장착. 스탯 수치는 저장하지 않는다 - "무기 칸에 무엇이 있나"(슬롯 배치)가 원본이고
        //   공격력은 매 타격마다 GetAttackPower()가 파생 계산한다 (이중 진실 봉인).
        void Channel::OnItemEquip(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (size < headerSize + 1) { return; }
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_ITEM_EQUIP req;
            req.Deserialize(pkt);

            GameSession* gs = nullptr;
            Player* p = BeginItemHandler(sid, gs);
            if (p == nullptr) { return; }

            // 통지할 장착 칸 번호를 종류에서 미리 확정 (Equip 성공 후엔 가방 칸 내용이 바뀌어 있음)
            const ItemInstance& it = p->GetInventory().At(req.slot);
            if (it.IsEmpty()) { SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT)); return; }
            const ItemDef* def = FindItemDef(it.templateId);
            if (def == nullptr) { SendItemResult(gs, static_cast<BYTE>(EItemResult::NOT_FOUND)); return; }

            int equipSlot = 0;
            if (def->itemType == EItemType::EQUIP_WEAPON) { equipSlot = EQUIP_SLOT_WEAPON; }
            else if (def->itemType == EItemType::EQUIP_ARMOR) { equipSlot = EQUIP_SLOT_ARMOR; }
            else { SendItemResult(gs, static_cast<BYTE>(EItemResult::WRONG_TYPE)); return; }   // 소모품/기타 장착 불가

            if (!p->GetInventory().Equip(req.slot))
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT));
                return;
            }
            p->MarkDbDirty(DB_DIRTY_INVENTORY);

            const int slots[2] = { req.slot, equipSlot };   // 가방 칸(기존 장착품 또는 빈 칸) + 장착 칸
            SendItemUpdate(gs, p, slots, 2);
            SendStatUpdate(gs, p);   // 장착 변화 = 파생 공/방 변화 통지 (표시용 - 권위는 서버 재계산)
        }

        // CS_ITEM_UNEQUIP - 장착 해제. 가방 빈 칸이 없으면 실패 (아이템 증발 경로 없음).
        void Channel::OnItemUnequip(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (size < headerSize + 1) { return; }
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_ITEM_UNEQUIP req;
            req.Deserialize(pkt);

            GameSession* gs = nullptr;
            Player* p = BeginItemHandler(sid, gs);
            if (p == nullptr) { return; }

            if (req.equipSlot != EQUIP_SLOT_WEAPON && req.equipSlot != EQUIP_SLOT_ARMOR)
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT));   // 장착 칸 번호가 아님
                return;
            }
            if (p->GetInventory().At(req.equipSlot).IsEmpty())
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT));   // 비어 있는 장착 칸
                return;
            }

            const int bagSlot = p->GetInventory().Unequip(req.equipSlot);
            if (bagSlot < 0)
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::INV_FULL));   // 가방 가득 - 해제 불가
                return;
            }
            p->MarkDbDirty(DB_DIRTY_INVENTORY);

            const int slots[2] = { req.equipSlot, bagSlot };
            SendItemUpdate(gs, p, slots, 2);
            SendStatUpdate(gs, p);
        }

        // CS_ITEM_DISCARD - 버리기. 가방 칸만 허용 (장착품은 해제 먼저). 버린 것은 자기 위치 바닥 드랍이 되고
        //   버린 사람이 단독 1순위 소유권을 갖는다 (실수 즉시 회수 가능). 도배는 맵당 cap 이 방어.
        void Channel::OnItemDiscard(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (size < headerSize + 5) { return; }   // slot(1B)+quantity(4B)
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_ITEM_DISCARD req;
            req.Deserialize(pkt);

            GameSession* gs = nullptr;
            Player* p = BeginItemHandler(sid, gs);
            if (p == nullptr) { return; }

            if (req.slot >= MAX_INVENTORY_SLOTS)
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT));   // 장착 칸 직접 버리기 금지
                return;
            }
            const ItemInstance dropped = p->GetInventory().At(req.slot);   // 값 복사 - RemoveAt 후에도 종류 필요
            if (dropped.IsEmpty()) { SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT)); return; }
            if (req.quantity <= 0 || req.quantity > dropped.quantity)
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT));
                return;
            }

            if (!p->GetInventory().RemoveAt(req.slot, req.quantity))
            {
                SendItemResult(gs, static_cast<BYTE>(EItemResult::BAD_SLOT));
                return;
            }
            p->MarkDbDirty(DB_DIRTY_INVENTORY);

            UINT32 owners[3] = { p->GetCharId(), 0, 0 };   // 버린 사람 단독 우선권
            m_mapManager.SpawnGroundItem(p->GetMapId(), p->GetPos(), dropped.templateId, req.quantity,
                                         owners, m_tickTimer.GetCurrentTimeMS());

            const int slots[1] = { req.slot };
            SendItemUpdate(gs, p, slots, 1);
        }

        // 거래 상대 세션 조회 - 저장된 partnerSid 로 세션을 찾아 존재 + 같은 채널이면 반환 (실패 nullptr).
        //   ResolveTradePartner(상호참조/맵/생존 재검증) 와 CancelTradeFor 가 공유하는 세션 확보 단계.
        GameSession* Channel::ResolvePartnerSession(ChannelManager& cm, const TradeState& tradeState) const
        {
            const UINT64 partnerSid = tradeState.partnerSid;
            if (partnerSid == 0) { return nullptr; }
            GameSession* pgs = cm.GetSession(partnerSid);
            if (pgs == nullptr || pgs->GetChannelId() != m_channelId) { return nullptr; }   // 옛 sid / 타 채널
            return pgs;
        }

        // me 의 거래 상대를 조회한다 - 상호 참조(상대가 나를 가리킴)/같은 맵/생존을 매번 재검증한다.
        //   TradeState 는 플레이어별 값이라 한쪽만 리셋된 stale partnerSid 유령 거래가 가능한데, 상호 참조가 이를 원천 차단.
        Player* Channel::ResolveTradePartner(Player* me)
        {
            GameSession* pgs = ResolvePartnerSession(ChannelManager::GetInstance(), me->GetTrade());
            if (pgs == nullptr) { return nullptr; }
            Player* partner = pgs->GetPlayer();
            if (partner == nullptr) { return nullptr; }
            if (partner->GetTrade().partnerSid != me->GetSid()) { return nullptr; }   // 상호 참조 확인 (한쪽만 리셋된 유령 봉인)
            if (partner->GetMapId() != me->GetMapId()) { return nullptr; }             // 같은 맵 게이트 유지
            if (partner->IsDead()) { return nullptr; }
            return partner;
        }

        // SC_TRADE_UPDATE - me 시점의 거래창 현재 상태 (내 오퍼 압축 목록 + 상대 오퍼 압축 목록 + 양측 수락 여부).
        //   압축 = 쓰인 칸만 순서대로 담고 각 원소 slot 에 거래칸 번호를 실어 클라가 어느 칸인지 안다.
        void Channel::SendTradeUpdate(GameSession* gs, const Player* me, const Player* partner)
        {
            if (gs == nullptr) { return; }
            const TradeState& mine = me->GetTrade();
            const TradeState& theirs = partner->GetTrade();
            SC_TRADE_UPDATE body{};
            body.myAccept = mine.accepted ? 1 : 0;
            body.partnerAccept = theirs.accepted ? 1 : 0;
            body.myCount = 0;
            body.partnerCount = 0;
            for (int i = 0; i < MAX_TRADE_SLOTS; ++i)
            {
                if (!mine.offer[i].used) { continue; }
                ItemSlotEntry& e = body.mine[body.myCount++];
                e.slot = static_cast<BYTE>(i);
                e.uid = mine.offer[i].uid;
                e.templateId = mine.offer[i].templateId;
                e.quantity = mine.offer[i].quantity;
            }
            for (int i = 0; i < MAX_TRADE_SLOTS; ++i)
            {
                if (!theirs.offer[i].used) { continue; }
                ItemSlotEntry& e = body.partner[body.partnerCount++];
                e.slot = static_cast<BYTE>(i);
                e.uid = theirs.offer[i].uid;
                e.templateId = theirs.offer[i].templateId;
                e.quantity = theirs.offer[i].quantity;
            }
            SendToSession(gs, PacketType::SC_TRADE_UPDATE, body);
        }

        // SC_TRADE_RESULT - 거래 종결/실패 사유 통지 (완료/취소/상대 이탈/공간 부족 등).
        void Channel::SendTradeResult(GameSession* gs, BYTE result)
        {
            if (gs == nullptr) { return; }
            SC_TRADE_RESULT body;
            body.result = result;
            SendToSession(gs, PacketType::SC_TRADE_RESULT, body);
        }

        // 거래 강제 종결 - 양측 TradeState 를 리셋하고 각자에게 SC_TRADE_RESULT 를 보낸다.
        //   취소/상대 이탈뿐 아니라 leave/portal/채널변경 훅이 공용으로 부른다. 상대는 항상 통지, 자기(p)는 세션이 살아있을 때만.
        void Channel::CancelTradeFor(Player* p, BYTE result)
        {
            if (p == nullptr || !p->GetTrade().IsTrading()) { return; }
            ChannelManager& cm = ChannelManager::GetInstance();

            GameSession* pgs = ResolvePartnerSession(cm, p->GetTrade());
            if (pgs != nullptr)
            {
                Player* partner = pgs->GetPlayer();
                if (partner != nullptr && partner->GetTrade().partnerSid == p->GetSid())
                {
                    partner->GetTrade().Reset();
                    SendTradeResult(pgs, result);   // 상대에게 종결 통지
                }
            }

            GameSession* mygs = cm.GetSession(p->GetSid());
            p->GetTrade().Reset();
            SendTradeResult(mygs, result);   // 자기에게도 통지 (mygs null 이면 헬퍼 내부 가드로 no-op)
        }

        // CS_TRADE_REQUEST - /trade 이름. 같은 맵의 그 이름 상대에게 거래 요청을 건다.
        void Channel::OnTradeRequest(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (size < headerSize + 2) { return; }   // targetName 최소 1자(2B) - ReadString이 길이 초과를 막음
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_TRADE_REQUEST req;
            req.Deserialize(pkt);

            GameSession* gs = nullptr;
            Player* p = ResolveItemSender(sid, gs);
            if (p == nullptr) { return; }

            if (p->GetTrade().IsTrading()) { SendTradeResult(gs, static_cast<BYTE>(ETradeResult::BUSY)); return; }   // 내가 이미 거래 중

            Player* target = m_mapManager.FindPlayerByNameInMap(p->GetMapId(), req.targetName, p->GetSid());
            if (target == nullptr || target->IsDead())
            {
                SendTradeResult(gs, static_cast<BYTE>(ETradeResult::NOT_FOUND));   // 그 이름 상대가 이 맵에 없음/사망
                return;
            }
            if (target->GetTrade().IsTrading())
            {
                SendTradeResult(gs, static_cast<BYTE>(ETradeResult::BUSY));   // 상대가 다른 거래 중
                return;
            }

            // 요청 성립 - 나=요청 대기(PENDING_OUT), 상대=응답 대기(PENDING_IN). 상호 partnerSid 확정.
            p->GetTrade().Reset();
            p->GetTrade().partnerSid = target->GetSid();
            p->GetTrade().state = ETradeState::PENDING_OUT;

            target->GetTrade().Reset();
            target->GetTrade().partnerSid = p->GetSid();
            target->GetTrade().state = ETradeState::PENDING_IN;

            GameSession* tgs = ChannelManager::GetInstance().GetSession(target->GetSid());
            if (tgs != nullptr)
            {
                SC_TRADE_REQUEST notify;
                wcscpy_s(notify.requesterName, WHISPER_NAME_MAX, p->GetName());
                SendToSession(tgs, PacketType::SC_TRADE_REQUEST, notify);
            }
        }

        // CS_TRADE_RESPONSE - 받은 거래 요청을 수락/거절. 수락하면 양측 거래창이 열린다(OPEN).
        void Channel::OnTradeResponse(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (size < headerSize + 1) { return; }   // accept(1B)
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_TRADE_RESPONSE req;
            req.Deserialize(pkt);

            GameSession* gs = nullptr;
            Player* p = ResolveItemSender(sid, gs);
            if (p == nullptr) { return; }
            if (p->GetTrade().state != ETradeState::PENDING_IN) { return; }   // 응답 대기 상태가 아니면 stale - 무시

            // 요청자 재확인 - 그 사이 상대가 나가/죽었/딴 거래로 갈아탔으면 상대 이탈로 종결.
            Player* requester = ResolveTradePartner(p);
            if (requester == nullptr || requester->GetTrade().state != ETradeState::PENDING_OUT)
            {
                p->GetTrade().Reset();
                SendTradeResult(gs, static_cast<BYTE>(ETradeResult::PARTNER_LEFT));
                return;
            }

            if (req.accept == 0)
            {
                // 거절 - 양측 리셋 + 취소 통지.
                CancelTradeFor(p, static_cast<BYTE>(ETradeResult::CANCELLED));
                return;
            }

            // 수락 - 양측 거래창 열기. 오퍼/수락은 초기화된 상태로 시작.
            p->GetTrade().state = ETradeState::OPEN;
            p->GetTrade().accepted = false;
            requester->GetTrade().state = ETradeState::OPEN;
            requester->GetTrade().accepted = false;

            GameSession* rgs = ChannelManager::GetInstance().GetSession(requester->GetSid());
            // 양측에 SC_TRADE_OPEN(상대 이름) + 초기 빈 SC_TRADE_UPDATE.
            {
                SC_TRADE_OPEN openToMe;
                wcscpy_s(openToMe.partnerName, WHISPER_NAME_MAX, requester->GetName());
                SendToSession(gs, PacketType::SC_TRADE_OPEN, openToMe);
            }
            if (rgs != nullptr)
            {
                SC_TRADE_OPEN openToReq;
                wcscpy_s(openToReq.partnerName, WHISPER_NAME_MAX, p->GetName());
                SendToSession(rgs, PacketType::SC_TRADE_OPEN, openToReq);
            }
            SendTradeUpdate(gs, p, requester);
            SendTradeUpdate(rgs, requester, p);
        }

        // CS_TRADE_ADD_ITEM - 거래창에 오퍼를 올린다. 올리면 스캠 방지로 양측 수락이 풀린다(WoW식).
        void Channel::OnTradeAddItem(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (size < headerSize + 6) { return; }   // bagSlot(1B)+quantity(4B)+tradeSlot(1B)
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_TRADE_ADD_ITEM req;
            req.Deserialize(pkt);

            GameSession* gs = nullptr;
            Player* p = ResolveItemSender(sid, gs);
            if (p == nullptr) { return; }
            if (p->GetTrade().state != ETradeState::OPEN) { return; }   // 거래창이 안 열렸으면 무시

            Player* partner = ResolveTradePartner(p);
            if (partner == nullptr) { CancelTradeFor(p, static_cast<BYTE>(ETradeResult::PARTNER_LEFT)); return; }

            // 범위/내용 검증 (신뢰경계 - 클라 신뢰 0).
            if (req.tradeSlot >= MAX_TRADE_SLOTS) { return; }
            if (req.bagSlot >= MAX_INVENTORY_SLOTS) { return; }   // 가방 칸만 (장착품은 올리기 불가)
            if (req.quantity <= 0) { return; }
            const ItemInstance& it = p->GetInventory().At(req.bagSlot);
            if (it.IsEmpty()) { return; }
            if (req.quantity > it.quantity) { return; }   // 한 칸 보유량 초과 요청 차단 - alreadyFromBag + req.quantity signed 오버플로(허위 수량 표시) 전에 상한 봉인

            // 이 가방 칸에서 이미 올린 총량 + 이번 요청이 실제 보유량을 넘지 않게 (덮어쓰기면 그 칸 몫은 제외).
            TradeState& mine = p->GetTrade();
            int alreadyFromBag = mine.OfferedQuantityFrom(req.bagSlot);
            if (mine.offer[req.tradeSlot].used && mine.offer[req.tradeSlot].bagSlot == req.bagSlot)
            {
                alreadyFromBag -= mine.offer[req.tradeSlot].quantity;   // 이 거래칸을 덮어쓰는 중이면 옛 몫은 빼고 계산
            }
            if (alreadyFromBag + req.quantity > it.quantity) { return; }   // 보유량 초과 - 무시(복제 시도 봉인)

            mine.offer[req.tradeSlot].used = true;
            mine.offer[req.tradeSlot].bagSlot = req.bagSlot;
            mine.offer[req.tradeSlot].uid = it.uid;
            mine.offer[req.tradeSlot].templateId = it.templateId;
            mine.offer[req.tradeSlot].quantity = req.quantity;

            // 오퍼 변경 = 양측 수락 강제 리셋 (막판 바꿔치기 스캠 방어).
            ResetBothAccepts(p, partner);

            GameSession* pgs = ChannelManager::GetInstance().GetSession(partner->GetSid());
            SendTradeUpdate(gs, p, partner);
            SendTradeUpdate(pgs, partner, p);
        }

        // CS_TRADE_REMOVE_ITEM - 거래창에서 오퍼를 내린다. 역시 양측 수락이 풀린다.
        void Channel::OnTradeRemoveItem(UINT64 sid, const BYTE* data, int size)
        {
            const int headerSize = static_cast<int>(sizeof(PacketHeader));
            if (size < headerSize + 1) { return; }   // tradeSlot(1B)
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);
            CS_TRADE_REMOVE_ITEM req;
            req.Deserialize(pkt);

            GameSession* gs = nullptr;
            Player* p = ResolveItemSender(sid, gs);
            if (p == nullptr) { return; }
            if (p->GetTrade().state != ETradeState::OPEN) { return; }

            Player* partner = ResolveTradePartner(p);
            if (partner == nullptr) { CancelTradeFor(p, static_cast<BYTE>(ETradeResult::PARTNER_LEFT)); return; }

            if (req.tradeSlot >= MAX_TRADE_SLOTS) { return; }

            TradeState& mine = p->GetTrade();
            mine.offer[req.tradeSlot].used = false;   // 칸 비우기 (내용은 다음 add가 덮음)
            ResetBothAccepts(p, partner);

            GameSession* pgs = ChannelManager::GetInstance().GetSession(partner->GetSid());
            SendTradeUpdate(gs, p, partner);
            SendTradeUpdate(pgs, partner, p);
        }

        // CS_TRADE_ACCEPT - 내 수락을 표시(payload 없음). 양측 모두 수락이면 커밋한다.
        void Channel::OnTradeAccept(UINT64 sid, const BYTE* data, int size)
        {
            (void)data; (void)size;   // payload 없음
            GameSession* gs = nullptr;
            Player* p = ResolveItemSender(sid, gs);
            if (p == nullptr) { return; }
            if (p->GetTrade().state != ETradeState::OPEN) { return; }

            Player* partner = ResolveTradePartner(p);
            if (partner == nullptr) { CancelTradeFor(p, static_cast<BYTE>(ETradeResult::PARTNER_LEFT)); return; }

            p->GetTrade().accepted = true;

            GameSession* pgs = ChannelManager::GetInstance().GetSession(partner->GetSid());
            if (p->GetTrade().accepted && partner->GetTrade().accepted)
            {
                CommitTrade(p, partner, gs, pgs);   // 양측 수락 완료 - 사본 시뮬 후 스왑 + DB 트랜잭션
                return;
            }
            // 아직 한쪽만 수락 - 양측에 수락 상태 갱신 통지.
            SendTradeUpdate(gs, p, partner);
            SendTradeUpdate(pgs, partner, p);
        }

        // CS_TRADE_CANCEL - 거래 취소(payload 없음). 양측 종결.
        void Channel::OnTradeCancel(UINT64 sid, const BYTE* data, int size)
        {
            (void)data; (void)size;   // payload 없음
            GameSession* gs = nullptr;
            Player* p = ResolveItemSender(sid, gs);
            if (p == nullptr) { return; }
            if (!p->GetTrade().IsTrading()) { return; }
            CancelTradeFor(p, static_cast<BYTE>(ETradeResult::CANCELLED));
        }

        // 양측 수락 완료 - 실제 인벤을 건드리기 전에 사본에서 스왑을 시뮬해 양측 공간을 검증하고,
        //   통과하면 실제 인벤에 같은 연산을 적용한 뒤 단일 DB 트랜잭션으로 확정한다(부분 교환/복제/증발 봉인).
        void Channel::CommitTrade(Player* a, Player* b, GameSession* gsA, GameSession* gsB)
        {
            const TradeState& ta = a->GetTrade();
            const TradeState& tb = b->GetTrade();

            // (1) 사본 시뮬레이션 - a가 준 것을 빼고 b가 준 것을 받을 공간이 있는지(양측 각각) 검증.
            //     Inventory 는 포인터 없는 값 타입(ItemInstance 배열)이라 복사가 안전하다.
            Inventory simA = a->GetInventory();
            Inventory simB = b->GetInventory();
            for (int i = 0; i < MAX_TRADE_SLOTS; ++i)
            {
                if (ta.offer[i].used) { simA.RemoveAt(ta.offer[i].bagSlot, ta.offer[i].quantity); }   // 잠금 중이라 실패 불가(add-time 검증)
                if (tb.offer[i].used) { simB.RemoveAt(tb.offer[i].bagSlot, tb.offer[i].quantity); }
            }
            bool ok = true;
            for (int i = 0; i < MAX_TRADE_SLOTS && ok; ++i)
            {
                // 시뮬 uid 는 0 이 아닌 아무 값 - 0 을 넘기면 새로 놓인 칸이 IsEmpty()==true 로 남아
                //   다음 시뮬 Add 가 그 칸을 빈 칸으로 오판(수용량 과대평가 -> 시뮬 통과 후 실제 실패 = 증발).
                //   시뮬은 uid 를 "칸 점유 표시"로만 쓰므로 중복돼도 무해하다.
                static const UINT64 TRADE_SIM_UID = ~0ULL;
                if (tb.offer[i].used && !simA.Add(TRADE_SIM_UID, tb.offer[i].templateId, tb.offer[i].quantity)) { ok = false; }   // b 오퍼를 a가 받음
                if (ta.offer[i].used && !simB.Add(TRADE_SIM_UID, ta.offer[i].templateId, ta.offer[i].quantity)) { ok = false; }   // a 오퍼를 b가 받음
            }

            if (!ok)
            {
                // 어느 한쪽 인벤이 상대 오퍼를 못 받음 - 아무 것도 바꾸지 않고 수락만 리셋, 거래창 유지(재조정 후 재수락).
                ResetBothAccepts(a, b);
                SendTradeUpdate(gsA, a, b);
                SendTradeUpdate(gsB, b, a);
                SendTradeResult(gsA, static_cast<BYTE>(ETradeResult::INV_FULL));
                SendTradeResult(gsB, static_cast<BYTE>(ETradeResult::INV_FULL));
                return;
            }

            // (2) 실제 적용 - 시뮬과 같은 순서(양측에서 빼고 -> 양측이 받기). 받는 아이템마다 새 uid 발급.
            //     감사 재료도 여기서 캡처한다: 빼기 '전'의 스택 수량(변경 전 값)과 받기에서 발급되는 새 uid -
            //     원본(offer.uid)과 새 uid 의 짝이 이 아이템의 계보(어디서 와서 어디로 갔나)를 잇는다.
            Inventory& invA = a->GetInventory();
            Inventory& invB = b->GetInventory();
            int prevQtyA[MAX_TRADE_SLOTS];   // a 오퍼 각각의 이동 전 스택 수량 (빼기 전에 캡처)
            int prevQtyB[MAX_TRADE_SLOTS];
            for (int i = 0; i < MAX_TRADE_SLOTS; ++i)
            {
                prevQtyA[i] = ta.offer[i].used ? invA.At(ta.offer[i].bagSlot).quantity : 0;
                prevQtyB[i] = tb.offer[i].used ? invB.At(tb.offer[i].bagSlot).quantity : 0;
            }
            for (int i = 0; i < MAX_TRADE_SLOTS; ++i)
            {
                if (ta.offer[i].used && !invA.RemoveAt(ta.offer[i].bagSlot, ta.offer[i].quantity))
                {
                    // 잠금(add-time 검증)된 오퍼라 도달 불가 - 발동하면 잠금-실제 불일치(더블 스펜드 위험) 회귀. 아래 수령 Add 실패 로그와 대칭.
                    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                        KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"trade",
                        L"제거 실패 - 잠금 오퍼가 실제 제거 실패 (char %u slot %d x%d)",
                        a->GetCharId(), ta.offer[i].bagSlot, ta.offer[i].quantity);
                }
                if (tb.offer[i].used && !invB.RemoveAt(tb.offer[i].bagSlot, tb.offer[i].quantity))
                {
                    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                        KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"trade",
                        L"제거 실패 - 잠금 오퍼가 실제 제거 실패 (char %u slot %d x%d)",
                        b->GetCharId(), tb.offer[i].bagSlot, tb.offer[i].quantity);
                }
            }
            TradeAuditSnapshot audit;
            audit.tradeUid = IssueItemUid();   // 거래 고유 번호 - 감사 행 중복 차단 키(모호 COMMIT 재실행의 이중 증거 차단). 실물 uid 와 같은 영속 uid 발급기
            audit.count = 0;
            for (int i = 0; i < MAX_TRADE_SLOTS; ++i)
            {
                if (tb.offer[i].used)
                {
                    const UINT64 newUid = IssueItemUid();
                    if (!invA.Add(newUid, tb.offer[i].templateId, tb.offer[i].quantity))
                    {
                        // 시뮬이 같은 상태, 같은 순서로 통과했으므로 도달 불가 - 발동하면 시뮬-실제 불일치 회귀.
                        KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                            KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"trade",
                            L"수령 Add 실패 - 시뮬과 실제 불일치 (char %u <- tpl %d x%d)",
                            a->GetCharId(), tb.offer[i].templateId, tb.offer[i].quantity);
                    }
                    const UINT64 placedUid = InventoryContainsUid(invA, newUid) ? newUid : 0;   // 0 = 전량 기존 스택 합류
                    AppendTradeAudit(audit, b->GetCharId(), a->GetCharId(), tb.offer[i], placedUid, prevQtyB[i]);
                }
                if (ta.offer[i].used)
                {
                    const UINT64 newUid = IssueItemUid();
                    if (!invB.Add(newUid, ta.offer[i].templateId, ta.offer[i].quantity))
                    {
                        KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                            KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"trade",
                            L"수령 Add 실패 - 시뮬과 실제 불일치 (char %u <- tpl %d x%d)",
                            b->GetCharId(), ta.offer[i].templateId, ta.offer[i].quantity);
                    }
                    const UINT64 placedUid = InventoryContainsUid(invB, newUid) ? newUid : 0;   // 0 = 전량 기존 스택 합류
                    AppendTradeAudit(audit, a->GetCharId(), b->GetCharId(), ta.offer[i], placedUid, prevQtyA[i]);
                }
            }

            // (3) 단일 DB 트랜잭션 - 양측 스냅샷 + 감사 행을 한 커맨드로(Begin -> wipe 양측 -> insert 양측 -> 감사 insert -> Commit).
            //     크래시가 Begin~Commit 사이에 나면 롤백되어 양측 다 거래 전 상태(복제/증발 없음).
            //     감사가 같은 트랜잭션이라 "거래 결과는 영속됐는데 증거가 없다"도 구조적으로 불가.
            //     성공 시엔 이 커맨드가 곧 최신 상태를 영속화하므로 dirty 불요. 실패(롤백) 시엔 커맨드가 양측 dirty 를 재표시해(this 전달)
            //     접속 유지자는 다음 autosave, 채널 이관자는 이관 시 보수적 재-dirty, 이탈자는 leave 최종 저장(SavePlayerFull)이
            //     post-trade 로 수렴시킨다. (이탈자의 leave 저장까지 실패하면 재시도 없음 - 알려진 잔여 창)
            InventorySnapshot snapA, snapB;
            FillInventorySnapshot(a, snapA);
            FillInventorySnapshot(b, snapB);
            DBThread::GetInstance().Enqueue(new TradeCommitCommand(snapA, snapB, audit, this));

            // (4) 종결 - 상태 리셋 + 완료 통지 + 전량 스냅샷(변경 폭이 큼).
            a->GetTrade().Reset();
            b->GetTrade().Reset();
            SendTradeResult(gsA, static_cast<BYTE>(ETradeResult::COMPLETE));
            SendTradeResult(gsB, static_cast<BYTE>(ETradeResult::COMPLETE));
            SendFullInventory(gsA, a);
            SendFullInventory(gsB, b);
        }

        // CS_CHAT/CS_WHISPER - 일반 채팅은 같은 맵 전원에게, 귓속말은 cross-channel로 라우팅.
        void Channel::OnChat(UINT64 sid, PacketType opcode, const BYTE* data, int size)
        {
            // 진입 검증 + 공통 파싱 (CS_CHAT / CS_WHISPER가 같은 핸들러서 opcode로 갈림).
            const int headerSize = static_cast<int>(sizeof(PacketHeader));   // = 4
            if (size < headerSize + 2) { return; }   // 길이 필드(2B)도 없으면 절단 -> 버림 (나머지는 ReadString이 clamp)
            KYS::GAMECOMMON::PROTOCOL::CPacket pkt(size);
            pkt.Write(data + headerSize, size - headerSize);   // 헤더 4B skip -> payload 적재

            ChannelManager& cm = ChannelManager::GetInstance();
            Player* sender = ResolveOwnedPlayer(cm, sid);
            if (sender == nullptr) { return; }   // 옛 sid (정상 race - 조용히 버림)

            // opcode 분기 - 각 핸들러로 위임.
            if (opcode == PacketType::CS_CHAT)         { HandlePublicChat(sender, pkt); }
            else if (opcode == PacketType::CS_WHISPER) { HandleWhisper(sid, sender, pkt); }

            // [!] Send 외 부수효과 0 - SendObjectUpdates(상태 바뀜 표시 broadcast)에 채팅 분기 추가 금지.
        }

        // CS_CHAT 분기 - 발신자 이름/메시지를 SC_CHAT_BROADCAST로 같은 맵 전원에 배치 송신 (서버 권위 이름).
        void Channel::HandlePublicChat(Player* sender, KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
        {
            CS_CHAT req;
            req.Deserialize(pkt);                          // message (ReadString이 길이 초과를 막음)

            const int msgLen = static_cast<int>(wcslen(req.message));
            if (!CanChat(sender, req.message, msgLen)) { return; }   // 빈/도배 -> 조용히 드롭

            SC_CHAT_BROADCAST chatBroadcast;
            chatBroadcast.playerId = sender->GetPlayerId();                            // 시야 안이면 말풍선 부착용
            wcscpy_s(chatBroadcast.senderName, WHISPER_NAME_MAX, sender->GetName());   // 발신자 이름 (서버 권위 - 위조 0)
            wcscpy_s(chatBroadcast.message, CHAT_MSG_MAX, req.message);

            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_CHAT_BROADCAST));
            chatBroadcast.Serialize(out);                             // playerId -> senderName -> message
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }

            // 같은 맵 player 전원에 B4 배치 누적(즉시 Send 아님) -> tick-end FlushBatch에서 수신자당 1 WSASend.
            //   (즉시 full-map Send가 phase1 inline O(N) WSASend 폭주 원인이었음 - 배칭으로 송신횟수 D x A->수신자수.)
            const int recipients = m_mapManager.BroadcastToMapBatched(sender->GetMapId(), m_channelId,
                out.GetBuffer(), out.GetSize(), m_flushList);   // 시야 아님(맵 전체, MapleStory 정합), 역포인터 송신(전역락 우회)
            m_bytesSent += static_cast<UINT64>(out.GetSize()) * static_cast<UINT64>(recipients);
        }

        // CS_WHISPER 분기 - 도배 게이트 통과 후 cross-channel 라우팅 잡을 SharedServicesThread에 넘긴다.
        void Channel::HandleWhisper(UINT64 senderSid, Player* sender, KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
        {
            CS_WHISPER req;
            req.Deserialize(pkt);                          // targetName + message (ReadString이 길이 초과를 막음)

            const int whisperLen = static_cast<int>(wcslen(req.message));
            if (whisperLen <= 0) { return; }               // 빈 메시지 - 조용히 드롭 (악성/오류 클라, 통지 불요)
            if (!CanChat(sender, req.message, whisperLen))
            {
                // len>0 인데 게이트 실패 = 도배 rate-limit -> 발신자에게 SC_WHISPER_FAIL{RATE_LIMITED} 직송 (이미 발신 채널 스레드).
                SC_WHISPER_FAIL fail;
                wcscpy_s(fail.targetName, WHISPER_NAME_MAX, req.targetName);
                fail.reason = static_cast<BYTE>(EWhisperFail::RATE_LIMITED);
                GameSession* gs = sender->GetGameSession();
                if (gs != nullptr) { SendToSession(gs, PacketType::SC_WHISPER_FAIL, fail); }
                return;
            }

            SharedServicesThread* sst = &SharedServicesThread::GetInstance();
            if (sst != nullptr)
            {
                sst->EnqueueJob(new WhisperRouteJob(sender->GetName(), senderSid, req));
            }
        }



        // 30Hz 게임 루프 - 4개 phase를 순서대로 돌리고 남은 시간만큼 잔다.
        void Channel::TickLoop()
        {
            m_tickTimer.Initialize();
            while (m_running)
            {
                const UINT64 startUs = m_tickTimer.GetCurrentTimeMicro();          // tick 시작 시각
                const float  dt = TICK_INTERVAL_US / 1000000.0f;     // 고정 timestep 30Hz (결정론 dead-reckon, TICK_INTERVAL_US서 파생)

                ProcessSessionEvent();   // (0) 세션이벤트 큐 우선 drain
                ProcessJob();             // (1) drain - mailbox job Execute (OnMove=대조 후 수용/보정, Send 0)
                const UINT64 afterJobUs = m_tickTimer.GetCurrentTimeMicro();        // per-phase 계측 경계
                SendObjectUpdates();          // (2) broadcast - 바뀜 표시된 객체를 전원(self 포함) 송신 후 표시 해제
                const UINT64 afterBroadcastUs = m_tickTimer.GetCurrentTimeMicro();
                UpdateFrame(dt);          // (3) update - 객체 순회 다형 위치 예측 후 바뀜 표시
                const UINT64 afterUpdateUs = m_tickTimer.GetCurrentTimeMicro();
                m_phaseRecvUs += (afterJobUs - startUs);                  // (0)+(1) recv/job
                m_phaseBroadcastUs += (afterBroadcastUs - afterJobUs);   // (2) broadcast 전체
                m_phaseUpdateUs += (afterUpdateUs - afterBroadcastUs);   // (3) update
                KickIdleSessions();
                Sleep(startUs);           // (4) measure-then-sleep (멤버명 Sleep, Win32 호출은 ::Sleep)
            }

            // 루프 종료(m_running=false) 후 종료 처리 - 강제 종료 전 마지막 저장 기회.
            //   (1) ProcessSessionEvent: 종료 직전까지 mailbox 에 쌓인 세션이벤트(입장/이탈 in-flight)를 남김없이 drain.
            //       라이브러리 Stop 은 active 세션을 끊지 않으므로 접속 중 플레이어의 LEAVE 는 여기서 생기지 않는다 - 그 저장은 (2)가 담당.
            //   (2) ShutdownSaveConnectedPlayers: 아직 접속 중인 전 플레이어를 dirty 무관으로 캐릭터+인벤 저장(로그아웃 저장의 일괄판).
            //   실제 DB 쓰기는 main 이 채널 join 후 dbThread.Stop() 의 최종 drain 에서 수행(Save-before-Load FIFO 유지).
            ProcessSessionEvent();
            ShutdownSaveConnectedPlayers();
        }

        // 종료 저장 - 접속 중 전 플레이어를 dirty 무관으로 캐릭터+인벤 저장한다(로그아웃 저장의 일괄판).
        //   호출: TickLoop 루프 종료 직후 1회, 소유 채널 스레드에서(single-writer). 여기서 enqueue 만 하고
        //   실제 DB 쓰기는 main 이 채널 join 후 dbThread.Stop() 의 최종 drain 에서 수행.
        void Channel::ShutdownSaveConnectedPlayers()
        {
            DBThread* dbThread = &DBThread::GetInstance();
            if (dbThread == nullptr)
            {
                return;
            }

            std::vector<Player*> players;
            m_mapManager.CollectPlayersForSave(players);   // PLAYER 타입만 수집 (autosave 와 동일 열거)
            for (std::vector<Player*>::iterator it = players.begin(); it != players.end(); ++it)
            {
                Player* p = *it;
                SavePlayerFull(p, dbThread);   // dirty 무관 - 종료는 마지막 기회라 전량 저장(autosave 의 dirty 게이트와 다른 점)
            }

            // 종료 저장 실행 이력 - 0명이어도 무조건 기록 ("아예 안 돌았다" 와 "대상이 0명이었다" 를 파일에서 구분).
            //   graceful shutdown 3단계(감지 -> 저장 -> 종료) 중 '저장' 단계의 파일 증거.
            KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"shutdown",
                L"ch=%d 종료 저장 %d명 enqueue", m_channelId, static_cast<int>(players.size()));
        }

        // 세션이벤트 큐를 비운다 (enter/leave/login-complete - 매 tick 우선 처리).
        void Channel::ProcessSessionEvent()
        {
            KYS::GAMESERVER::THREAD::IJob* job = nullptr;
            while (m_sessionEventMailbox.Dequeue(job))
            {
                job->Execute();   // OnChannelEnter / OnClientLeave / DbResultJob
                delete job;       // 소유권: enqueue 측 new -> drain 측 delete
            }
        }

        // (1) drain - mailbox의 job을 모두 실행한다 (OnRecv -> opcode 분기).
        void Channel::ProcessJob()
        {
            KYS::GAMESERVER::THREAD::IJob* job = nullptr;

            // (1) critical lane 먼저 - 로그인/전투/채팅/포탈/종료. per-tick 상한으로 tick 시간 유계(livelock 차단, 우선 처리).
            //   상한 초과분은 큐에 남아 다음 tick(defer), 풀 소진 시 DROP(m_droppedCritical 계측). critical은 저빈도라 정상 부하선 상한 미발동.
            int criticalProcessed = 0;
            while (criticalProcessed < MAX_CRITICAL_JOBS_PER_TICK && m_mailbox.Dequeue(job))
            {
                job->Execute();   // -> OnRecv -> OnSkill/OnChat/OnPortal/... (즉답은 BatchTo로 tick-end flush)
                delete job;       // 소유권: ChannelManager new -> Channel drain delete
                ++criticalProcessed;
            }

            // (2) movement lane (CS_MOVE) - 별도 상한. droppable(dead-reckoning이 메움)이라 과부하 시 여기가 먼저 shed.
            int movementProcessed = 0;
            while (movementProcessed < MAX_MOVE_JOBS_PER_TICK && m_moveMailbox.Dequeue(job))
            {
                job->Execute();   // -> OnRecv -> OnMove (PosSync 대조 후 수용/보정, Send 0)
                delete job;
                ++movementProcessed;
            }

            m_lastDrainCount = criticalProcessed + movementProcessed;   // Sleep(measure)에서 m_jobSamples 에 기록 (tick당 입력 분포 - 입력 burst 판정)
        }

        // 한 수신자에게 B4 배치 누적 - 즉시 gs->Send 대신 m_sendBatch에 쌓고 tick-end FlushBatch에서 1회 송신.
        //   채팅(맵 전체)/스킬(AOI) inline 송신을 이동 broadcast와 같은 배치 경로로 통일(WSASend 폭주 제거).
        //   phase1(ProcessJob의 OnChat/OnSkill)에서 등록해도 m_flushList가 tick-end에 비워지므로 phase2 flush에 함께 나간다.
        void Channel::BatchTo(GameSession* gs, const Player* owner, const BYTE* buf, int size)
        {
            if (gs == nullptr || gs->GetChannelId() != m_channelId || (owner != nullptr && gs->GetPlayer() != owner)) { return; }   // 3가드: null / 다른 채널로 이관된 세션(옛 채널의 배치 접근 = cross-thread 레이스) / 짝 검사 - owner 를 역포인터로 따라와 얻은 gs 는 owner 를 자기 Player 로 인정해야 한다 (AttachPlayer 가 두 방향을 함께 걸므로 서로 가리키는 것이 "지금 유효한 짝" 의 정의. 어긋나면 그 Player 는 회수돼 슬롯이 재사용된 것 - 새 주인에게 남의 패킷을 보내지 않는다)
            if (!gs->IsBatchQueued()) { m_flushList.push_back(gs); gs->SetBatchQueued(true); }   // flush 대상 1회 등록
            gs->AppendToBatch(buf, size);                                                        // 수신자 버퍼 누적
            m_bytesSent += static_cast<UINT64>(size);                                            // 송신 바이트 계측
        }

        // (2) broadcast - 이번 tick에 바뀜 표시된 객체를 시야(AOI) 안 player 전원에게 보낸다.
        //   수집 로직은 Map에 위임, 전송은 Channel이 수행. 전 tick 바뀜 표시를 소비.
        void Channel::SendObjectUpdates()
        {
            ChannelManager& cm = ChannelManager::GetInstance();   // 싱글턴 직접 (outbound 수신자 resolve)
            // m_flushList는 여기서 clear 안 함 - phase1(OnChat/OnSkill의 BatchTo) 등록분도 담겨 있으므로
            //   flush 루프 뒤(tick-end)에 비운다. (시작에서 clear하면 phase1 채팅/스킬 배치 수신자가 사라짐.)
            BroadcastMovedObjects();          // (2a) 바뀜 표시 mover -> 시야 전원 배치

            BroadcastSpawnsAndDespawns(cm);   // (2b) 출현/소멸 패킷 -> 수신자 배치 합류

            FlushBatchedSends();              // (2c) 수신자별 누적 배치를 1회 Send
        }

        // (2a) 이동 broadcast - 이번 tick 바뀜 표시된 mover를 시야(AOI) 안 player 전원에게 배치 누적한다.
        void Channel::BroadcastMovedObjects()
        {
            m_movedObjects.clear();
            m_mapManager.CollectMovedObjects(m_movedObjects);   // 각 Map의 바뀜 표시 객체 모음

            std::vector<GameObject*> nearbyObjects;               // 시야 수신자 임시 버퍼 (지역)
            for (std::vector<GameObject*>::iterator it = m_movedObjects.begin(); it != m_movedObjects.end(); ++it)
            {
                GameObject* mover = *it;
                if (mover->IsDead()) { continue; }               // phase1 이동 후 처치된 시신 - 이동 broadcast 스킵(생산측 Map 가드와 대칭, 클라 시신 미끄럼 desync 봉인)
                m_broadcastPacket.Reset();                       // [필수] 매 mover 전 writePos/readPos 0 복원 (Begin은 append라 Reset 없으면 누적->버퍼 초과)
                if (!mover->SerializeBroadcast(m_broadcastPacket)) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", m_broadcastPacket.GetType(), m_broadcastPacket.GetSize()); continue; }    // 다형 - Player=SC_MOVE_BROADCAST (mover당 1회)
                const BYTE* buf = m_broadcastPacket.GetBuffer(); // base GetBuffer() (고정 버퍼 - 재사용)
                const int   size = m_broadcastPacket.GetSize();  // base GetSize() const (mover마다 캡처 - 길이 다름)
                _ASSERTE(size > 0);   // no-op SerializeBroadcast(미이동 객체)가 m_movedObjects에 잘못 들었으면 포착 - 정상 mover(Player/Monster)는 항상 full 패킷

                // mover의 시야 범위 수집 (sid->object 재조회 0회, mover당 1회) -> 각 GetId -> Send
                nearbyObjects.clear();
                m_mapManager.GetNearbyObjectsFor(mover, VIEW_RANGE, nearbyObjects);   // mover 맵의 시야 범위 수집 위임
                for (std::vector<GameObject*>::iterator a = nearbyObjects.begin(); a != nearbyObjects.end(); ++a)
                {
                    if ((*a)->GetObjectType() != EObjectType::PLAYER)                // Monster 스킵
                    {
                        continue;
                    }

                    GameSession* gs = static_cast<Player*>(*a)->GetGameSession();   // 역포인터 (PLAYER 가드 뒤 - 전역 SRWLock 우회)
                    if (gs != nullptr && gs->GetChannelId() == m_channelId)   // 다른 채널로 이관된 수신자 배치 접근 차단
                    {
                        if (!gs->IsBatchQueued()) { m_flushList.push_back(gs); gs->SetBatchQueued(true); }   // 첫 누적 시 flush 대상 등록 (tick당 1회)
                        gs->AppendToBatch(buf, size);               // 즉시 Send 대신 수신자 버퍼에 누적 (tick-end 1회 flush)
                        m_bytesSent += static_cast<UINT64>(size);   // 송신 바이트 계측 (수신자당)
                    }
                }
                mover->SetDirty(false);                           // 송신 끝 - 바뀜 표시 해제
            }

            m_mapManager.ClearMovedObjects();                            // 맵별 바뀜 표시 일괄 clear (Map::Update가 clear 안 함)
        }

        // (2b) spawn/despawn broadcast - Map이 직렬화한 출현/소멸 패킷을 수신자별 배치에 합류시킨다.
        void Channel::BroadcastSpawnsAndDespawns(ChannelManager& cm)
        {
            // spawn/despawn 송신 - Map이 직렬화/push한 것을 Channel이 보냄
            m_pendingPackets.clear();
            m_mapManager.CollectOutbound(m_pendingPackets);
            for (std::vector<OutboundPacket>::iterator it = m_pendingPackets.begin();
                it != m_pendingPackets.end(); ++it)
            {
                GameSession* gs = cm.GetSession(it->sid);   // 수신자 resolve (옛 sid면 nullptr)
                // 소유권 가드 - grid Erase는 m_outboundPackets를 안 비우므로, 직전 tick에 큐된 mover 대상 stale spawn 패킷이
                //   이관된 세션(이제 다른 채널 소유)의 배치에 append되어 그 채널과 레이스할 수 있다. 소유 채널만 append.
                if (gs != nullptr && gs->GetChannelId() == m_channelId)
                {
                    if (!gs->IsBatchQueued()) { m_flushList.push_back(gs); gs->SetBatchQueued(true); }   // spawn/despawn도 같은 배치에 흡수
                    gs->AppendToBatch(it->data, it->size);   // 누적 (이동 broadcast와 같은 수신자 배치에 합류)
                    m_bytesSent += static_cast<UINT64>(it->size);   // 송신 바이트 계측 (수신자당)
                }
            }
            m_mapManager.ClearOutbound();   // 송신 끝 -> 각 맵 송신 버퍼 비움
        }

        // 채널 이동 출발측 - mover를 이번 tick flush 대상(m_flushList)에서 제외한다 (swap-and-pop, 순서 비보존).
        //   grid 제거 직후 호출 - 이후 A의 tick-end flush가 mover 배치를 안 만져 B 스레드의 append와 레이스가 사라진다.
        void Channel::RemoveFromFlushList(GameSession* gs)
        {
            for (std::vector<GameSession*>::iterator it = m_flushList.begin(); it != m_flushList.end(); ++it)
            {
                if (*it == gs)
                {
                    *it = m_flushList.back();
                    m_flushList.pop_back();
                    return;
                }
            }
        }

        // (2c) flush - 수신자별로 누적된 송신 배치를 1회씩 Send하고 다음 tick을 위해 비운다.
        void Channel::FlushBatchedSends()
        {
            // flush: 수신자별 누적 배치를 1회 Send (WSASend DxA -> ~수신자수). per-phase send 계측은 여기.
            const UINT64 flushStartUs = m_tickTimer.GetCurrentTimeMicro();
            for (std::vector<GameSession*>::iterator f = m_flushList.begin(); f != m_flushList.end(); ++f)
            {
                (*f)->FlushBatch();          // 누적분 1회 Send (RingBuffer 연속영역 -> WSASend 1회가 K패킷 운반)
                (*f)->SetBatchQueued(false); // 다음 tick 위해 등록 해제
            }
            m_flushList.clear();   // tick-end clear - phase1(채팅/스킬 BatchTo) + phase2(이동/spawn) 배치분을 함께 flush한 뒤 비움
            m_phaseSendUs += (m_tickTimer.GetCurrentTimeMicro() - flushStartUs);   // send(flush) 누적 - 실제 gs->Send/WSASend 시간
        }

        // (3) update - sim(위치 예측/셀 이동/시야/바뀜 표시)을 MapManager에 위임하고, 주기적으로 DB 저장.
        void Channel::UpdateFrame(float deltaTime)
        {
            const UINT64 nowMs = m_tickTimer.GetCurrentTimeMS();   // 벽시계 ms (리스폰 절대시각 비교 + DB 저장 게이트 공용, tick 내 1회). Initialize 안 됐으면 0
            m_mapManager.Update(deltaTime, nowMs);   // 위임 - sim은 MapManager/Map. 여기선 send 안 함. nowMs로 리스폰 만료 비교(wall-clock).

            // 이번 tick 몬스터에게 죽은 플레이어의 거래 강제 종결 - 사망자는 거래 불가라 상대에게 이탈 통지.
            //   CancelTradeFor는 거래 중이 아니면 no-op이라 사망자 전원에 무조건 호출해도 안전(대개 빈 목록).
            std::vector<UINT64> diedPlayers;
            m_mapManager.CollectDiedPlayers(diedPlayers);
            if (!diedPlayers.empty())
            {
                ChannelManager& cm = ChannelManager::GetInstance();
                for (std::vector<UINT64>::iterator it = diedPlayers.begin(); it != diedPlayers.end(); ++it)
                {
                    CancelTradeFor(cm.GetPlayer(*it), static_cast<BYTE>(ETradeResult::PARTNER_LEFT));   // GetPlayer null 이면 내부 가드로 no-op
                }
            }

            // DB 주기 저장 (벽시계 ms 정수 비교 - deltaTime 누산 아님)
            DBThread* dbThread = &DBThread::GetInstance();
            if (dbThread != nullptr)
            {
                if (m_lastDbSaveMs == 0)
                {
                    m_lastDbSaveMs = nowMs;   // 최초 1회 - 부팅 직후 저장 폭주 방지 (첫 진입은 기준만)
                }
                else if ((nowMs - m_lastDbSaveMs) >= DB_SAVE_INTERVAL * 1000ULL)   // 60*1000 ms (=60s, DB_SAVE_INTERVAL, GameDefines.h 전역)
                {
                    std::vector<Player*> players;
                    m_mapManager.CollectPlayersForSave(players);   // PLAYER 타입만 수집
                    for (std::vector<Player*>::iterator it = players.begin(); it != players.end(); ++it)
                    {
                        Player* p = *it;
                        DBResult snapshot;
                        FillCharacterSnapshot(p, snapshot);
                        dbThread->Enqueue(new SaveFullCommand(snapshot));

                        // 인벤은 바뀐 플레이어만 (dirty 비트) - 전량 교체 쓰기라 무변경 저장은 낭비.
                        if ((p->GetDbDirty() & DB_DIRTY_INVENTORY) != 0)
                        {
                            InventorySnapshot invSnapshot;
                            FillInventorySnapshot(p, invSnapshot);
                            // 주기 autosave (낙관적 clear + 롤백 재표시):
                            //   스냅샷을 뜬 이 게임 스레드에서 dirty 를 곧바로 낙관적으로 끈다. 스냅샷 이후의 인벤 변경은
                            //   dirty 를 다시 켜므로 다음 주기에 저장된다(enqueue~콜백 사이 pickup 창을 구조적으로 봉인).
                            //   저장이 롤백되면 InventorySaveFailedJob 이 회신돼 dirty 를 되살려 다음 주기 재저장한다(성공은 회신 없음).
                            dbThread->Enqueue(new SaveInventoryCommand(invSnapshot, this));
                            p->ClearDbDirty();   // 낙관적 clear - 게임 스레드(소유자 자신)라 cross-thread 레이스 없음

                        }
                    }
                    m_lastDbSaveMs = nowMs;
                }
            }
        }

        // 무활동 세션을 약 1초 간격으로 점검해 회수한다.
        void Channel::KickIdleSessions()
        {
            ChannelManager& cm = ChannelManager::GetInstance();   // 싱글턴 직접
            const UINT64 nowMs = ::GetTickCount64();   // 스탬프(SetLastActivityMs)와 같은 프로세스 전역 시계로 비교해야 cross-epoch 언더플로가 없다
            if (nowMs == 0)
            {
                return;   // 방어적 0 가드 (GetTickCount64는 부팅 직후 극초기 외엔 0이 아니다)
            }
            if (m_lastSweepMs == 0)
            {
                m_lastSweepMs = nowMs;   // 최초 1회 - 첫 진입은 기준만 (부팅 직후 즉시 점검 방지)
                return;
            }
            if (nowMs - m_lastSweepMs < ORPHAN_SWEEP_INTERVAL_MS)
            {
                return;   // 간격 제한 - 약 1초에 한 번만 전수 스캔
            }
            m_lastSweepMs = nowMs;

            // read 락으로 idle 후보 sid만 스냅샷 -> 락 밖에서 회수 (반복자 무효화/재진입 락 회피).
            m_idleSids.clear();
            cm.CollectIdleSessions(m_channelId, nowMs, static_cast<UINT64>(SESSION_IDLE_TIMEOUT_MS), m_idleSids);
            int kickedThisSweep = 0;   // 이번 sweep 에서 idle Disconnect 를 발행한 수 (async leave job 이 뒤이어 게임 상태 정리 - 아래 sweep 요약 로그용)
            for (std::vector<UINT64>::iterator it = m_idleSids.begin(); it != m_idleSids.end(); ++it)
            {
                const UINT64 sid = *it;
                GameSession* gs = cm.GetSession(sid);
                if (gs == nullptr)
                {
                    continue;   // 스냅샷~회수 사이 이미 떠남
                }
                // idle 회수: Disconnect만 호출한다 - 소켓 close 후 pending IO 0 도달 시 DecPendingIo -> EnqueueDisconnect -> OnClientLeaveJob(async)가 게임 상태를 정리한다.
                //   형제 축출 경로(라이브러리 reaper, KickByAccount 등)와 동일 - 직접 OnClientLeave를 부르지 않아 이중 정리와 순서 결박이 사라진다(OnClientLeave는 idempotent).
                gs->Disconnect(EDisconnectReason::IDLE_TIMEOUT);     // 소켓 close -> async leave job이 게임 상태 정리
                ++kickedThisSweep;
            }
            // idle 축출 sweep 요약 (개별 이탈은 leave 로그가 사유 IDLE_TIMEOUT 으로 남기고, 여기는 sweep 단위 규모만)
            if (kickedThisSweep > 0)
            {
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"kick",
                    L"ch=%d idle 세션 %d개 회수", m_channelId, kickedThisSweep);
            }
        }

        // (4) measure-then-sleep - tick 처리시간을 측정/누산하고 남은 시간만큼 잔다 (멤버명 Sleep, Win32 호출은 ::Sleep).
        void Channel::Sleep(UINT64 startUs)
        {
            const UINT64 elapsedUs = m_tickTimer.GetCurrentTimeMicro() - startUs;   // tick 끝 시각 - 시작 시각

            // 모니터: tick 처리시간 분포 누산 (단일 writer - 무락)
            m_totalTickUs += elapsedUs;
            if (elapsedUs > m_maxTickUs)
            {
                m_maxTickUs = elapsedUs;
            }

            // tick 정밀 지표용 표본 기록 (원형 버퍼 - 채널 스레드 단일 writer라 무락).
            //   가장 오래된 칸을 덮어써 최근 WINDOW개만 유지(sliding window). dump 시 정렬해 percentile/trimmed 산출.
            m_tickSamples[m_tickSampleHead] = elapsedUs;
            m_jobSamples[m_tickSampleHead] = static_cast<UINT64>(m_lastDrainCount);   // burst: tick당 입력 수 (elapsedUs와 같은 head 공유)
            m_tickSampleHead = (m_tickSampleHead + 1) % TICK_SAMPLE_WINDOW;
            if (m_tickSampleCount < TICK_SAMPLE_WINDOW) { ++m_tickSampleCount; }

            if (elapsedUs >= TICK_INTERVAL_US)
            {
                ++m_overBudgetCount;                          // 예산 초과 (>= TICK_INTERVAL_US = tick 한 칸을 넘김)
            }
            else if (elapsedUs >= (TICK_INTERVAL_US * 4 / 5))
            {
                ++m_warnCount;                                // 80% 경고 (>= 예산의 4/5)
            }
            ++m_tickCount;

            if (elapsedUs < TICK_INTERVAL_US)
            {
                const UINT64 sleepUs = TICK_INTERVAL_US - elapsedUs;
                m_sleepUs += sleepUs;   // idle 정확: 실제 여유 누적 (over면 이 분기 안 타 sleep 0 - bimodal/선점에서도 정확)
                ::Sleep(static_cast<DWORD>(sleepUs / 1000));   // Win32 ::Sleep (멤버명 충돌 회피). 남은 시간만 (us->ms)
            }
            // elapsed > interval(tick 초과)이면 sleep 0 - 처리가 밀려 다음 tick이 더 밀리는 상황 인지
        }

        void Channel::Stop()
        {
            m_running = false;   // 단일 writer(main). volatile로 루프가 다음 반복에 관측.
        }

        // 모니터 카운터 + 맵별 몬스터 수 + 이탈 사유 분포를 복사하고 tick 분포까지 산출해 반환한다 (메인 스레드가 'q' 입력 시 호출).
        ChannelStatsSnapshot Channel::GetMonitorStats() const
        {
            // 카운터 스칼라는 plain read 로 복사 (cross-thread 근사 - x64 정렬 read는 원자적).
            //   맵별 몬스터 수와 이탈 사유 분포는 아래 배열 루프로 채우고, tick 분포는 표본 정렬이 필요해 FillTickDistribution 으로 뺐다.
            ChannelStatsSnapshot s{};   // 0-초기화 필수 - monsterPerMap은 쓰는 맵 수까지만 채우므로 꼬리 칸이 쓰레기로 남으면 안 됨
            s.packetCount = m_packetCount;
            s.bytesRecv = m_bytesRecv;
            s.bytesSent = m_bytesSent;
            s.totalTickUs = m_totalTickUs;
            s.maxTickUs = m_maxTickUs;
            s.overBudgetCount = m_overBudgetCount;
            s.warnCount = m_warnCount;
            s.tickCount = m_tickCount;
            s.sleepUs = m_sleepUs;   // idle 계산용 (실제 Sleep 누적 - trimmedAvg 추정 대체)
            s.moveDropped = static_cast<UINT64>(m_droppedMove);           // movement lane shed 누적 (과부하 가시화)
            s.criticalDropped = static_cast<UINT64>(m_droppedCritical);   // critical lane drop 누적 (대칭 계측, 0이 정상)
            s.phaseRecvUs = m_phaseRecvUs;
            s.phaseBroadcastUs = m_phaseBroadcastUs;
            s.phaseSendUs = m_phaseSendUs;
            s.phaseUpdateUs = m_phaseUpdateUs;
            for (int i = 0; i < MapTableCount(); ++i)
            {
                s.monsterPerMap[i] = m_mapManager.GetMonsterCount(i);   // 맵별 몬스터 수 (채널 스레드 카운터 - 근사, 쓰는 맵 수만큼)
            }
            for (int i = 0; i < LEAVE_REASON_COUNT; ++i)
            {
                s.leaveReasonCount[i] = m_leaveReasonCount[i];   // 이탈 사유 분포 (채널 스레드 단일 writer - 근사 read)
            }

            FillTickDistribution(s);
            return s;
        }

        // 워치독 하트비트 읽기 - 총 tick 카운터를 그대로 노출한다 (plain read - GetMonitorStats와 같은 cross-thread 근사).
        //   정의를 .cpp에 두는 이유: 워치독 폴링 루프에서 호출할 때 컴파일러가 값을 캐싱하지 못하게 (매번 실제 read).
        UINT64 Channel::GetTickCounter() const
        {
            return m_tickCount;
        }

        // tick 처리시간 + tick당 입력 수의 분포 지표(trimmed mean, percentile)를 표본 원형 버퍼에서 산출해 채운다.
        void Channel::FillTickDistribution(ChannelStatsSnapshot& s) const
        {
            // tick 정밀 지표: 표본 원형 버퍼를 로컬로 복사 후 정렬해 trimmed mean + percentile 산출.
            //   cross-thread 근사: 채널 스레드가 기록 중이어도 8B read는 원자적, count는 스냅샷 시점값(위 카운터 복사와 동일 철학).
            //   원형 버퍼는 시간순이 아니나 정렬하므로 [0..n) 그대로 복사하면 된다(count<WINDOW면 [0..count)가 기록된 칸).
            const int n = (m_tickSampleCount < TICK_SAMPLE_WINDOW) ? m_tickSampleCount : TICK_SAMPLE_WINDOW;
            if (n > 0)
            {
                UINT64 sorted[TICK_SAMPLE_WINDOW];
                for (int i = 0; i < n; ++i) { sorted[i] = m_tickSamples[i]; }
                std::sort(sorted, sorted + n);

                // trimmed mean: 상하위 각 5% 제거 평균 (대표값 - 양끝 극단 outlier 배제, 사용자 "최대 N/최소 M 제거" 비율 실현)
                const int trim = n * 5 / 100;
                int lo = trim;
                int hi = n - trim;
                if (hi <= lo) { lo = 0; hi = n; }   // 표본이 적어 trim 후 빌 경우 전체 사용
                UINT64 sum = 0;
                for (int i = lo; i < hi; ++i) { sum += sorted[i]; }
                s.trimmedAvgUs = sum / static_cast<UINT64>(hi - lo);

                // percentile: 정렬 배열의 위치 인덱스 (tail spike는 여기 + 전체 max(maxTickUs)로 이중 보존)
                s.p50Us = sorted[n * 50 / 100];
                s.p95Us = sorted[(n * 95 / 100 < n) ? (n * 95 / 100) : (n - 1)];
                s.p99Us = sorted[(n * 99 / 100 < n) ? (n * 99 / 100) : (n - 1)];
                s.windowMaxUs = sorted[n - 1];
                s.tickSampleCount = n;

                // burst 진단: tick당 입력(drain job) 수 분포 (elapsedUs와 같은 표본 패턴 - jobP95>>jobP50 = 입력 몰림 = WiFi/네트워크 burst)
                UINT64 jsorted[TICK_SAMPLE_WINDOW];
                for (int i = 0; i < n; ++i) { jsorted[i] = m_jobSamples[i]; }
                std::sort(jsorted, jsorted + n);
                s.jobP50 = jsorted[n * 50 / 100];
                s.jobP95 = jsorted[(n * 95 / 100 < n) ? (n * 95 / 100) : (n - 1)];
                s.jobMax = jsorted[n - 1];
            }
            else
            {
                s.trimmedAvgUs = 0; s.p50Us = 0; s.p95Us = 0; s.p99Us = 0; s.windowMaxUs = 0; s.tickSampleCount = 0;
                s.jobP50 = 0; s.jobP95 = 0; s.jobMax = 0;
            }
        }


    }
}
