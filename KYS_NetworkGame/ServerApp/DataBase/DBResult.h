#pragma once
#include "../GameServer/Types/Defines.h"
#include "../GameServer/Core/Thread/IJob.h"
#include "../../GameCommon/GameTypes.h"     // PlayerId (UINT32)
#include "../../GameCommon/GameDefines.h"    // WHISPER_NAME_MAX

namespace KYS
{
    namespace SERVERAPP
    {
        class ChannelManager;
        class GameSession;
        class Player;        // DbResultJob::ApplyInventory(Player*) 시그니처용 (포인터라 전방 선언 충분)

        // 인벤토리 한 칸의 영속 행 (character_inventory + item_instance JOIN 결과와 1:1).
        struct InventoryRow
        {
            BYTE   slot;         // 가방 0..23, 무기 100, 방어구 101
            UINT64 uid;          // item_instance PK
            int    templateId;
            int    quantity;
        };

        // 캐릭터 한 명의 인벤토리 전량 스냅샷 (값 타입 - DB 왕복 운반. Player* 금지, use-after-free 봉인).
        //   로드: LoadCharacterCommand 가 캐릭터와 같은 왕복에서 채워 DBResult 에 동봉(원자 로드 - 부분 로드 창 봉인) /
        //   저장: Channel 이 떠서 SaveInventory/TradeCommit 커맨드에 담음.
        struct InventorySnapshot
        {
            static const int MAX_ROWS = MAX_INVENTORY_SLOTS + 2;   // 가방 + 무기 + 방어구

            // -- 런타임 라우팅 (저장 경로에서 사용 - DBResult 동봉 로드 시엔 DBResult 의 sid/chId/charId 를 쓴다) --
            UINT64 sid;
            int    chId;
            bool   loadFailed;   // backend 도달 실패 - 빈 인벤으로 입장시키면 다음 저장이 아이템을 지우므로 세션을 끊는다

            // -- 영속 필드 --
            UINT32       charId;
            int          count;
            InventoryRow rows[MAX_ROWS];
        };

        struct DBResult   // 값 타입 - DB 로드 결과 + 회신 라우팅 정보
        {
            // -- 런타임 라우팅 (CSV 미저장 - 호출측 LoadCharacterCommand/DbResultJob 가 세팅) --
            UINT64   sid;       // 회신 대상 (generation 재조회 키)
            int      chId;      // 배정 채널
            bool     found;     // 캐릭터 행 존재 여부 (false -> 신규, 기본 캐릭터 생성, name 은 LoadCharacterCommand 가 채움)
            bool     loadFailed;   // DB backend 도달 실패(MySQL 끊김 등) - true 면 DbResultJob 이 FailLogin 으로 세션 정리(RST 종료·거부 패킷 없이). found=false(신규 캐릭터)와 구별.

            // -- 캐릭터 영속 필드 (characters 행과 대응) --
            UINT32   charId;    // 캐릭터 PK (선택해 입장한 캐릭터). SaveFull 이 WHERE char_id 로 이 행만 갱신.
            PlayerId playerId;
            wchar_t  name[WHISPER_NAME_MAX];   // 캐릭터명 (char_name 컬럼에서 로드, std::wstring 금지)
            int      x;
            int      y;
            int      mapId;
            int      hp;
            int      maxHp;
            int      mp;

            // -- 인벤토리 (원자 로드 - 캐릭터와 같은 DB 왕복에서 함께 로드해 한 결과로 운반. 부분 로드 창 봉인) --
            //   저장 경로(SaveFull/SaveDelta)는 캐릭터 필드만 쓰므로 이 필드를 무시한다 - 로그인 로드 전용.
            InventorySnapshot inventory;
        };

        // 캐릭터 목록 한 행 (캐릭터 선택 화면용 요약 - SC_CHARACTER_LIST 의 CharSummary 와 1:1).
        struct CharRow
        {
            UINT32  charId;
            BYTE    slotId;
            wchar_t name[WHISPER_NAME_MAX];
            int     mapId;
            int     hp;
            int     maxHp;
        };

        // 계정의 캐릭터 목록 (LoadCharacterList 결과). count + rows[MAX_CHAR_SLOTS]. sid = 회신 대상 raw 소켓.
        struct CharListResult
        {
            UINT64  sid;
            int     count;
            CharRow rows[MAX_CHAR_SLOTS];
        };

        // DB 로드 결과를 Channel 스레드에서 적용하는 작업 (generation 재확인 -> Player 부착 -> 캐릭터/인벤 채움 -> 로그인/인벤 응답 -> spawn).
        //   원자 로드: 캐릭터와 인벤이 한 결과(DBResult + 동봉 inventory)로 오므로, 이 Job 하나가 attach~인벤채움~spawn 예약을 끝까지 원자 실행한다.
        class DbResultJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            explicit DbResultJob(const DBResult& result);
            ~DbResultJob() override = default;
            void Execute() override;   // GetSession/AttachPlayer는 ChannelManager::GetInstance() 직접
            // 복사/이동 = IJob base 4줄 =delete 상속 (재선언 불요, 다형 base)
        private:
            void FailLogin(GameSession* gs);    // 로그인 실패 정리 - RST 종료(OnSessionGone 단일 대칭 teardown). 캐릭터/인벤 loadFailed, AttachPlayer-null 공통.
            void ApplyInventory(Player* p, GameSession* gs);   // 동봉 인벤 스냅샷을 Player 에 채우고 SC_INVENTORY/SC_STAT_UPDATE 송신
            DBResult        m_result;           // 값 스냅샷 (sid, chId, found, loadFailed, 캐릭터 필드, 동봉 inventory)
        };

        // 인벤토리 저장이 롤백(실패)됐을 때만 채널 스레드에서 그 플레이어의 dirty 를 되살리는 작업.
        //   게임 스레드가 스냅샷 시점에 dirty 를 낙관적으로 껐으므로, 저장이 실패하면 이 재표시가 없으면 그 데이터는
        //   다음 주기에 저장되지 않는다. 성공 경로는 이미 껐으니 회신이 없다(콜백은 희소한 롤백에만 = pickup 창 봉인).
        //   sid 로 generation-safe 재조회 - 그 사이 떠난 세션이면 null 이라 no-op.
        //   m_channelId = 발행 채널: 채널변경(in-place handoff)으로 대상이 다른 채널로 옮겨갔으면 옛 채널 콜백이
        //   새 채널 소유 Player 를 건드리지 않도록 소유권 재검사(기존 소유권 가드와 동형, 이관됐으면 새 채널이 자기 주기에 저장).
        class InventorySaveFailedJob : public KYS::GAMESERVER::THREAD::IJob
        {
        public:
            InventorySaveFailedJob(UINT64 sid, int channelId) : m_sid(sid), m_channelId(channelId) {}
            ~InventorySaveFailedJob() override = default;
            void Execute() override;   // 대상이 여전히 발행 채널 소유면 인벤 dirty 재표시 (이관/이탈이면 no-op)
        private:
            UINT64 m_sid;         // 재표시 대상 (generation 재조회 키)
            int    m_channelId;   // 발행 채널 id (도착 시 대상 소유 채널과 대조)
        };

        // 거래 감사 1행 - 이동한 아이템 하나의 "누가 누구에게 무엇을" 기록 (trade_audit 테이블과 1:1).
        //   oldUid = 주는 쪽이 갖고 있던 원본 실물 / newUid = 받는 쪽에 새로 놓인 실물 - 이 짝이
        //   uid 계보(이 아이템이 어디서 와서 어디로 갔나)를 잇는다. newUid = 0 은 "전량이 수령측
        //   기존 스택에 합류"(스택형은 합류 순간 실물 정체성이 흡수됨 - 0 이 정직한 기록).
        //   prevQuantity = 이동 전 주는 쪽 스택 수량(변경 전 값 - 부분 스택 거래의 사후 검증용).
        struct TradeAuditRecord
        {
            UINT32 giverCharId;
            UINT32 receiverCharId;
            UINT64 oldUid;
            UINT64 newUid;
            int    templateId;
            int    quantity;       // 이동 수량
            int    prevQuantity;   // 이동 전 주는 쪽 스택 수량
        };

        // 거래 1건의 감사 행 묶음 (값 스냅샷 - Player* 금지 규칙 동일 적용).
        //   TradeCommitCommand 가 인벤 스냅샷과 함께 운반해 같은 DB 트랜잭션 안에서 INSERT 된다
        //   (거래 결과와 그 증거가 같은 체크포인트에 함께 있거나 함께 없거나).
        struct TradeAuditSnapshot
        {
            static const int MAX_ROWS = MAX_TRADE_SLOTS * 2;   // 양측 오퍼 합산 상한

            UINT64           tradeUid;   // 거래 1건 고유 번호 - 감사 행 중복 차단 키 재료((trade_uid,row_idx) UNIQUE 가 재실행 INSERT 를 걸러냄). 실물 uid 와 같은 발급기(영속 uid 단일 도메인)
            int              count;
            TradeAuditRecord rows[MAX_ROWS];
        };
    }
}
