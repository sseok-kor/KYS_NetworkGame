#pragma once
#include "DBResult.h"   // struct DBResult (KYS::SERVERAPP) - 로드/저장 함수들의 인자 타입

namespace KYS
{
    namespace SERVERAPP
    {
        // DB 백엔드 추상화 - 게임 로직은 CSV 인지 RDB 인지 모르고 이 인터페이스로만 캐릭터를 로드/저장한다.
        class IDBProvider
        {
        public:
            // 가상 소멸자 - 다형 base 라 basePtr 로 delete 해도 파생 자원까지 해제.
            virtual ~IDBProvider() = default;

            // 단일 소유 backend 핸들 - 복사/이동 4줄 차단 (다형 base 라 슬라이싱 방지).
            IDBProvider(const IDBProvider&) = delete;
            IDBProvider& operator=(const IDBProvider&) = delete;
            IDBProvider(IDBProvider&&) = delete;
            IDBProvider& operator=(IDBProvider&&) = delete;

            // 캐릭터 1행 로드. char_id 로 찾되 account_id 가 일치해야(소유 검증). 캐릭터 선택 후 입장 시 사용.
            //   반환 = backend 도달 성공 여부 (true = 조회함, 미발견 포함 / false = 치명적 backend 오류).
            //   행 존재 여부는 out.found (없으면 false = 캐릭터 없음/타 계정 -> 호출측이 로그인 실패 처리, 자동생성 폐기).
            virtual bool Load(UINT32 charId, UINT32 accountId, DBResult& out) = 0;

            // 캐릭터 상태 통째 저장 (로그아웃/주기). 값 스냅샷 입력 (Player* 금지, use-after-free 봉인). char_id 키 UPDATE.
            virtual void SaveFull(const DBResult& snapshot) = 0;

            // 변경분만 저장 (나중 대비 골격). dirtyMask = 바뀐 항목 비트마스크.
            //   CSV 백엔드에선 행 단위로 다시 쓰므로 SaveFull 에 위임 (변경분 저장 이득은 나중에 RDB 에서).
            virtual void SaveDelta(const DBResult& snapshot, UINT32 dirtyMask) = 0;

            // -- 캐릭터 관리 (목록/생성/삭제) - 캐릭터 선택 화면 단계(pre-auth)에서 호출 --
            // 계정의 캐릭터 목록 로드. out.count + out.rows 에 채운다(최대 MAX_CHAR_SLOTS).
            virtual bool LoadCharacterList(UINT32 accountId, CharListResult& out) = 0;   // 반환 = backend 도달 성공(false = 목록 송신 skip, 빈/부분 목록 권위 오송신 방지)
            // 캐릭터 생성. 반환 = ECharCreateResult 값(BYTE - OK/DUP_NAME/INVALID/SLOT_TAKEN/DB_ERROR). 성공 시 outCharId = 새 char_id.
            virtual BYTE CreateCharacter(UINT32 accountId, const wchar_t* name, BYTE slotId, UINT32& outCharId) = 0;
            // 캐릭터 삭제(즉시 hard delete). 소유 검증 = account_id. 반환 = ECharDeleteResult 값(BYTE - OK/NOT_OWNED/DB_ERROR).
            //   캐릭터 행과 그 인벤토리 행(배치+실물)을 함께 지운다(고아 인벤 행 방지 - 백엔드가 원자성 보장).
            virtual BYTE DeleteCharacter(UINT32 accountId, UINT32 charId) = 0;

            // -- 인벤토리 (아이템 시스템) --
            // 캐릭터의 인벤토리 전량 로드 (character_inventory + item_instance JOIN). 반환 = backend 도달 여부.
            virtual bool LoadInventory(UINT32 charId, InventorySnapshot& out) = 0;
            // 인벤토리 전량 교체 저장 - 기존 행 삭제 후 스냅샷 행 삽입을 한 트랜잭션으로 (슬롯 절반만 저장되는 크래시 윈도우 봉인).
            //   반환 = 커밋 성공 (커맨드가 성공 시에만 dirty clear 콜백 발행 - 롤백 시 dirty 유지로 다음 주기 재저장).
            virtual bool SaveInventory(const InventorySnapshot& snapshot) = 0;
            // 거래 커밋 - 양측 인벤토리 전량 교체 + 거래 감사 행을 단일 트랜잭션으로 (반쪽 영속 = 복제/증발
            //   dupe 의 뿌리 - 원천 봉쇄. 감사가 같은 트랜잭션이라 "거래는 됐는데 증거가 없다"도 구조적 불가). 반환 = 커밋 성공.
            virtual bool TradeCommit(const InventorySnapshot& a, const InventorySnapshot& b, const TradeAuditSnapshot& audit) = 0;

            // 부팅 1회 - 오래된 거래 감사 행을 hot(trade_audit)에서 cold(trade_audit_archive)로 이관.
            //   증거는 소멸하지 않고 접근만 느려진다(hot/cold 2계층). 실패해도 부팅은 계속(다음 부팅이 재시도).
            virtual void ArchiveOldTradeAudit() = 0;
            // 부팅 1회 - 지금까지 발급된 최대 item_uid 조회 (인메모리 uid 발급기 시드). 반환 = backend 도달 여부.
            virtual bool LoadMaxItemUid(UINT64& outMaxUid) = 0;

            // 트랜잭션 경계 - 여러 SQL 을 원자적으로(전부 성공 or 전부 취소). SaveInventory/TradeCommit 이 사용.
            virtual void Begin() = 0;
            virtual void Commit() = 0;
            virtual void Rollback() = 0;

        protected:
            // 추상 -> 파생만 생성. 복사 생성자 delete 가 암시적 기본 ctor 를 억제하므로 명시 필요.
            IDBProvider() = default;
        };
    }
}
