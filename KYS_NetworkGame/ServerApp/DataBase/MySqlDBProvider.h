#pragma once
#include "IDBProvider.h"   // base (Load/SaveFull/SaveDelta/Begin/Commit/Rollback override)
#include "../../GameServer/Core/Config/DbConfig.h"   // 연결 정보(값 멤버)
// mysql.h 는 pch_serverapp.h 에서 winsock2 뒤에 포함됨 - MYSQL* 멤버에 필요.

namespace KYS
{
    namespace SERVERAPP
    {
        // MySQL 백엔드 provider. CsvDBProvider 를 대체한다(IDBProvider 시그니처 동일 -> 상류 무변경).
        //   연결은 이 객체를 만든 스레드가 아니라 Connect()를 호출하는 스레드(=DB 스레드) 소유다.
        //     m_provider 는 main 스레드에서 생성되므로 ctor 에서 connect 하면 안 된다(스레드별 1연결 규칙).
        //     -> DBThread::ThreadLoop 진입부에서 Connect() 1회 호출.
        class MySqlDBProvider : public IDBProvider
        {
        public:
            MySqlDBProvider();
            virtual ~MySqlDBProvider();   // mysql_close

            // 연결 + 스키마 부트스트랩(DB 스레드에서 1회). 성공=true / 실패=false(호출측 fail-fast).
            bool Connect(const DbConfig& config);

            // 연결 해제. 연 스레드가 닫는다 - DB 스레드가 루프를 빠져나올 때 mysql_thread_end() 직전에 부른다.
            //   소멸자보다 먼저 닫아야 하는 이유: 이 객체는 싱글턴 DBThread 의 멤버라 main 이 반환한 뒤에나
            //   소멸하는데, main 은 그 전에 mysql_library_end() 로 라이브러리를 이미 정리한다.
            //   두 번 불러도 안전(멱등) - 소멸자는 비정상 경로용 폴백으로 남는다.
            void Disconnect();

            // IDBProvider 작업 함수 (override) - C API + 인코딩.
            bool Load(UINT32 charId, UINT32 accountId, DBResult& out) override;   // char_id 키 + 소유(account_id) 검증
            void SaveFull(const DBResult& snapshot) override;                     // 순수 UPDATE WHERE char_id
            void SaveDelta(const DBResult& snapshot, UINT32 dirtyMask) override;

            // 캐릭터 관리 (override) - 목록/생성/삭제. 결과는 호출 명령(IDBCommand)이 raw 소켓으로 송신.
            bool LoadCharacterList(UINT32 accountId, CharListResult& out) override;
            BYTE CreateCharacter(UINT32 accountId, const wchar_t* name, BYTE slotId, UINT32& outCharId) override;
            BYTE DeleteCharacter(UINT32 accountId, UINT32 charId) override;

            // 인벤토리 (override) - 로드/전량 교체 저장/거래 커밋/uid 시드 조회.
            bool LoadInventory(UINT32 charId, InventorySnapshot& out) override;
            bool SaveInventory(const InventorySnapshot& snapshot) override;       // Begin -> 교체 -> Commit (실패 시 Rollback). 반환=커밋 성공
            bool TradeCommit(const InventorySnapshot& a, const InventorySnapshot& b, const TradeAuditSnapshot& audit) override;   // 양측 + 감사 행을 한 트랜잭션에
            bool LoadMaxItemUid(UINT64& outMaxUid) override;
            void ArchiveOldTradeAudit() override;   // 부팅 1회 - 90일 초과 감사 행을 archive 테이블로 이관 (hot/cold 2계층)

            // 트랜잭션 (override) - 본문은 사용자 손코딩(START TRANSACTION/COMMIT/ROLLBACK).
            void Begin() override;
            void Commit() override;
            void Rollback() override;

            // 운영 지표 - 재접속 성공/최종 실패 작업 누적 (모니터 표시용).
            //   DB 스레드만 쓰고(Interlocked) main 이 읽는다(x64 정렬 read - 전역 Interlocked write + main 평문 read 관용구).
            static UINT32 GetReconnectCount();
            static UINT32 GetQueryFailCount();

#ifdef _DEBUG
            // H4 (검증 하니스, Debug 전용): 다음 SaveInventoryOnce 1회를 커밋 없이 강제 롤백해 낙관적 ClearDbDirty 재현.
            //   콘솔 'd' 키로 무장. 강제 실패한 char 가 이후 재저장되는지로 fix 효능 관측(수정 전 미재저장=ResaveObserved 0).
            static void ArmInventoryFaultOnce();   // 다음 인벤 저장 1회 강제 실패 무장 (main 콘솔 스레드 -> Interlocked)
            static long GetH4FaultInjected();      // 강제 실패 주입 횟수
            static long GetH4ResaveObserved();     // 강제 실패한 char 가 이후 재저장된 횟수 (수정 전 0 / 수정 후 >0)
#endif

            // 복사/이동 = base IDBProvider 4줄 =delete 상속 (재선언 불요).

        private:
            // 연결 끊김 복구 - 작업 실패 직후 호출. mysql_ping 으로 연결 생사를 판정한다:
            //   살아있으면 false(끊김이 아닌 실패 - SQL 오류 등이라 재실행 대상 아님) /
            //   죽었으면 명시 재접속(최대 5회, 3초 간격) 성공 시 true(호출측이 같은 작업을 1회 재실행).
            //   재접속이 전부 실패하면 프로세스 종료 - DB 없이는 로그인/저장이 전부 막힌 반쪽 서버라
            //   조용히 버티는 대신 즉시 크게 알린다(TrinityCore 의 5회 후 중단과 같은 정책).
            bool RecoverIfConnectionLost();

            // 1회 시도 본체들 - public 래퍼가 실패 시 RecoverIfConnectionLost() 후 1회 재실행한다.
            //   재실행은 DBThread 가 커맨드를 손에 든 채 이 안에서 일어난다(큐 재삽입 없음 - FIFO 순서 보존).
            //   트랜잭션 본체(SaveInventoryOnce/TradeCommitOnce/DeleteCharacterOnce)도 같은 정책이다 -
            //   재접속 = 서버측 전체 롤백이라 반쪽이 없고, 전량 교체(지우고 다시 쓰기)라 COMMIT 응답 유실 후의
            //   재실행도 같은 결과(멱등). 감사 행만 append 라 비멱등인데 (trade_uid,row_idx) UNIQUE 가 중복을 걸러낸다.
            bool LoadOnce(UINT32 charId, UINT32 accountId, DBResult& out);
            bool SaveFullOnce(const DBResult& snapshot);
            bool LoadCharacterListOnce(UINT32 accountId, CharListResult& out);
            BYTE CreateCharacterOnce(UINT32 accountId, const wchar_t* name, BYTE slotId, UINT32& outCharId);
            BYTE DeleteCharacterOnce(UINT32 accountId, UINT32 charId);
            bool LoadInventoryOnce(UINT32 charId, InventorySnapshot& out);
            bool SaveInventoryOnce(const InventorySnapshot& snapshot);
            bool TradeCommitOnce(const InventorySnapshot& a, const InventorySnapshot& b, const TradeAuditSnapshot& audit);

            // 거래 감사 행 삽입 (TradeCommitOnce 의 트랜잭션 안에서만 호출 - 실패 시 false, 호출측이 Rollback).
            //   ON DUPLICATE KEY UPDATE no-op - 모호 COMMIT(실제 커밋 뒤 응답 유실) 재실행의 중복 INSERT 를 걸러낸다.
            bool InsertTradeAuditRows(const TradeAuditSnapshot& audit);

            // 스키마 부트스트랩 - characters/아이템/거래 감사 5개 테이블 CREATE TABLE IF NOT EXISTS (Connect 내부 호출).
            //   accounts 는 로그인 서버 소유라 게임 서버는 만들지 않는다.
            //   DDL 문자열은 골격에 제공(DBResult 와 1:1), mysql_query 실행은 사용자.
            bool EnsureSchema();

            // 선재 DB 마이그레이션 - 멱등키 컬럼(trade_uid/row_idx)/UNIQUE 가 없는 기존 감사 테이블에 추가.
            //   기존 행은 trade_uid = audit_id 로 backfill(컬럼 게이트 밖 무조건 실행·멱등) - 중간에 끊겨도 다음 부팅이 이어서 완료.
            bool EnsureTradeAuditDedupKey();

            // 인벤토리 전량 교체의 조각 (트랜잭션 안에서 호출 - 실패 시 false, 호출측이 Rollback).
            //   거래는 반드시 양측을 먼저 전부 지우고 나서 삽입해야 한다 - A 가 받은 아이템의 배치 행이
            //   아직 B 소유로 남아 있으면 PRIMARY KEY(item_uid) 충돌이 나기 때문 (wipe 전부 -> insert 전부 순서).
            bool WipeInventoryRows(UINT32 charId);                    // 이 캐릭터의 배치 행 + 연결된 실물 행 삭제
            bool InsertInventoryRows(const InventorySnapshot& snapshot);   // 스냅샷 행들을 실물/배치 테이블에 삽입

            MYSQL*   m_conn;     // 연결 핸들 (DB 스레드 전용). 미연결=nullptr.
            DbConfig m_config;   // 연결 정보 사본 (재접속 시 Connect 재호출에 사용)

            static volatile LONG s_reconnectCount;   // 재접속 성공 누적 (0 이 정상)
            static volatile LONG s_queryFailCount;   // 재실행까지 실패한 작업 누적 (0 이 정상)
        };
    }
}
