#pragma once
#include "IDBCommand.h"
#include "DBResult.h"                              // DBResult (값 멤버, KYS::SERVERAPP) + DbResultJob
#include "../../GameServer/Types/Defines.h"        // UINT32 / UINT64
#include "../../GameCommon/GameDefines.h"          // WHISPER_NAME_MAX (LoadCharacter 키 크기)

namespace KYS
{
    namespace SERVERAPP
    {
        class IChannel;        // 전방 선언 (LoadCharacter 회신 대상 - 포인터만)
        class ChannelManager;  // 전방 선언 (DbResultJob 에 전달 - 포인터만)

        // 통째 저장 (로그아웃 / 주기). 생성 시점에 Player 필드를 DBResult 값으로 통째 복사해 든다.
        class SaveFullCommand : public IDBCommand
        {
        public:
            explicit SaveFullCommand(const DBResult& snapshot) : m_snapshot(snapshot) {}
            void Execute() override;   // m_provider->SaveFull(m_snapshot)
        private:
            DBResult m_snapshot;       // 값 스냅샷 (이후 Player 소멸돼도 독립 - use-after-free 봉인)
        };

        // 변경분 저장 (나중 대비 골격). dirtyMask 카테고리만 저장.
        //   지금은 통째 저장이 주력이고, 변경분 저장은 SaveDelta 작업 함수 위임 흐름만 시연하는 골격.
        class SaveDeltaCommand : public IDBCommand
        {
        public:
            SaveDeltaCommand(const DBResult& snapshot, UINT32 dirtyMask)
                : m_snapshot(snapshot), m_dirtyMask(dirtyMask) {
            }
            void Execute() override;   // m_provider->SaveDelta(m_snapshot, m_dirtyMask)
        private:
            DBResult m_snapshot;       // 값 스냅샷
            UINT32   m_dirtyMask;      // Player::m_dbDirty 비트마스크 스냅샷
        };

        // 선택한 캐릭터 로드(입장) - 캐릭터 + 인벤토리를 같은 왕복에서 함께 로드(원자 로드). char_id + account_id(소유 검증) 키.
        //   결과(캐릭터 + 동봉 인벤)를 DbResultJob 하나에 담아 대상 Channel mailbox 로 회신 -> attach~캐릭터채움~인벤채움 이 한 Job 에서 원자 실행, 그 뒤 spawn 은 별도 OnChannelEnterJob 으로 예약(인벤 다 채운 뒤 격자 편입 = 부분 로드 창 봉인).
        //   회신 경로 = post-admission(GameSession 존재)이라 채널 스레드 gs->Send (목록/생성/삭제의 pre-auth raw send 와 다름).
        class LoadCharacterCommand : public IDBCommand
        {
        public:
            LoadCharacterCommand(UINT64 sid, int chId, UINT32 charId, UINT32 accountId,
                IChannel* targetChannel);
            void Execute() override;   // m_provider->Load + LoadInventory -> new DbResultJob -> m_targetChannel->EnqueueSessionEvent
        private:
            UINT64    m_sid;             // 회신 대상 (generation 재조회 키)
            int       m_chId;            // 배정 채널
            UINT32    m_charId;          // 선택 캐릭터 PK
            UINT32    m_accountId;       // 소유 검증 (Load WHERE account_id)
            IChannel* m_targetChannel;   // 회신 Channel (비소유)
        };

        // 캐릭터 목록 로드 (캐릭 선택 화면). 결과를 SC_CHARACTER_LIST 로 pre-auth raw 소켓에 직접 송신(GameSession 없음).
        class LoadCharListCommand : public IDBCommand
        {
        public:
            LoadCharListCommand(UINT64 sid, UINT32 accountId) : m_sid(sid), m_accountId(accountId) {}
            void Execute() override;   // m_provider->LoadCharacterList -> SC_CHARACTER_LIST -> ChannelManager::SendRaw
        private:
            UINT64 m_sid;          // 회신 대상 raw 소켓
            UINT32 m_accountId;    // 목록 조회 범위
        };

        // 캐릭터 생성. 결과(+새 char_id)를 SC_CHARACTER_CREATE_RESULT 로 raw 소켓에 송신.
        class CreateCharCommand : public IDBCommand
        {
        public:
            CreateCharCommand(UINT64 sid, UINT32 accountId, const wchar_t* name, BYTE slotId);
            void Execute() override;   // m_provider->CreateCharacter -> SC_CHARACTER_CREATE_RESULT -> SendRaw
        private:
            UINT64  m_sid;
            UINT32  m_accountId;
            wchar_t m_name[WHISPER_NAME_MAX];   // 값 복사 (고정 배열)
            BYTE    m_slotId;
        };

        // 캐릭터 삭제(즉시 hard delete). 결과를 SC_CHARACTER_DELETE_RESULT 로 raw 소켓에 송신.
        class DeleteCharCommand : public IDBCommand
        {
        public:
            DeleteCharCommand(UINT64 sid, UINT32 accountId, UINT32 charId)
                : m_sid(sid), m_accountId(accountId), m_charId(charId) {
            }
            void Execute() override;   // m_provider->DeleteCharacter -> SC_CHARACTER_DELETE_RESULT -> SendRaw
        private:
            UINT64 m_sid;
            UINT32 m_accountId;
            UINT32 m_charId;
        };

        // 인벤토리 전량 교체 저장 (주기 dirty / 로그아웃). 스냅샷을 값으로 들고 provider 가 한 트랜잭션으로 교체.
        //   게임 스레드가 스냅샷 시점에 dirty 를 낙관적으로 껐으므로, targetChannel != nullptr(주기 autosave)일 때
        //   저장이 롤백되면 InventorySaveFailedJob 을 그 채널에 회신해 dirty 를 되살린다(성공은 회신 없음).
        //   nullptr(leave/종료 저장)이면 최종 저장이라 회신 불요 - 플레이어가 떠나 dirty 재표시가 불가하므로,
        //   연결 끊김 실패는 provider 가 재접속 후 트랜잭션 전체 재실행으로 직접 방어한다(마지막 저장의 생존율).
        class SaveInventoryCommand : public IDBCommand
        {
        public:
            explicit SaveInventoryCommand(const InventorySnapshot& snapshot, IChannel* targetChannel = nullptr)
                : m_snapshot(snapshot), m_targetChannel(targetChannel) {}
            void Execute() override;   // m_provider->SaveInventory -> 롤백 & targetChannel 있으면 InventorySaveFailedJob 회신
        private:
            InventorySnapshot m_snapshot;      // 값 스냅샷 (Player 소멸과 독립)
            IChannel*         m_targetChannel; // 저장 실패 재표시 콜백 회신 채널 (비소유, nullptr=회신 안 함)
        };

        // 거래 커밋 - 양측 인벤토리 + 거래 감사 행을 단일 트랜잭션으로 영속화. 인메모리 스왑은 채널 스레드에서
        //   이미 끝났고, 여기는 그 결과를 "전부 반영 or 전무"로 디스크에 남긴다 (반쪽 영속 = 복제/증발 dupe 차단이
        //   이 커맨드의 존재 이유. 감사가 같은 트랜잭션이라 "결과는 있는데 증거가 없다"도 불가).
        class TradeCommitCommand : public IDBCommand
        {
        public:
            TradeCommitCommand(const InventorySnapshot& a, const InventorySnapshot& b, const TradeAuditSnapshot& audit,
                IChannel* targetChannel = nullptr)
                : m_a(a), m_b(b), m_audit(audit), m_targetChannel(targetChannel) {
            }
            void Execute() override;   // m_provider->TradeCommit - 실패 시 양측 dirty 재표시 회신(SaveInventoryCommand 대칭·B-1)
        private:
            InventorySnapshot  m_a;      // 값 스냅샷 (거래 당사자 갑)
            InventorySnapshot  m_b;      // 값 스냅샷 (거래 당사자 을)
            TradeAuditSnapshot m_audit;  // 거래 감사 행 묶음 (누가 누구에게 무엇을 - old/new uid 계보 포함)
            IChannel*          m_targetChannel; // 커밋 실패 시 양측 dirty 재표시 콜백 회신 채널 (비소유, nullptr=회신 안 함)
        };
    }
}
