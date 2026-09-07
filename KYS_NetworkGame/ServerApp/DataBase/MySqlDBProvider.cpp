#include "pch_serverapp.h"
#include "MySqlDBProvider.h"
#include "../../GameCommon/Protocol/GamePackets.h"   // ECharCreateResult / ECharDeleteResult (전역 enum)
#include "../../GameCommon/MapData/MapTable.h"      // MapTableAt (신규 캐릭 시작 좌표 = 맵0 부활 지점 - maps.csv 원천)
#include "../GameServer/Core/Log/Logger.h"  // DB 장애/이관 이력 파일 로그 (DBThread 문맥)
#include <cstring>   // memset / strlen (PCH 전이 의존 회피)
#include <cstdlib>   // strtol (information_schema COUNT 파싱)

namespace KYS
{
    namespace SERVERAPP
    {
        using KYS::GAMESERVER::LOG::LogLevel;

        // 파일-로컬 로그 헬퍼 - DB 계층 이벤트를 server 채널에 [db] 태그로 남긴다 (%S = mysql_error narrow 문자열 그대로).
        template<typename... Args>
        static void DbLog(LogLevel level, const wchar_t* format, Args&&... args)
        {
            KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                KYS::GAMESERVER::LOG::LogChannel::SERVER, level, L"db", format, std::forward<Args>(args)...);
        }

        // 재접속 정책: 연결 끊김 감지 시 최대 5회, 3초 간격으로 재접속을 시도하고
        //   전부 실패하면 프로세스를 종료한다(RecoverIfConnectionLost 참조).
        static const int   DB_RECONNECT_MAX_TRIES = 5;
        static const DWORD DB_RECONNECT_RETRY_MS  = 3000;

        // 거래 감사 hot 보존 일수 - 이 기간이 지난 행은 부팅 시 archive 로 이관(hot/cold 2계층).
        //   90일 = 상용 게임 복구 신청 마감 상단(15~90일) 벤치마크. 운영 조정 가능한 숫자 파라미터.
        static const int AUDIT_HOT_DAYS = 90;

        volatile LONG MySqlDBProvider::s_reconnectCount = 0;
        volatile LONG MySqlDBProvider::s_queryFailCount = 0;

        UINT32 MySqlDBProvider::GetReconnectCount() { return static_cast<UINT32>(s_reconnectCount); }
        UINT32 MySqlDBProvider::GetQueryFailCount() { return static_cast<UINT32>(s_queryFailCount); }

#ifdef _DEBUG
        // H4 (검증 하니스): 인벤 저장 강제 실패 무장/관측. DB 스레드(SaveInventoryOnce)와 main 콘솔 스레드가 공유하므로 Interlocked.
        //   g_h4FaultedCharId = 마지막 강제 실패 char. 이후 그 char 가 다시 저장되면 dirty 가 유지됐다는 뜻(fix 작동).
        //   수정 전엔 enqueue 시점 낙관적 ClearDbDirty 로 재저장이 안 와 ResaveObserved 가 0 으로 남는다.
        static volatile LONG g_h4FaultArm = 0;        // 1 = 다음 저장 1회 강제 실패
        static volatile LONG g_h4FaultInjected = 0;   // 강제 실패 주입 누적
        static volatile LONG g_h4ResaveObserved = 0;  // 강제 실패했던 char 재저장 누적
        static volatile LONG g_h4FaultedCharId = 0;   // 마지막 강제 실패 char (재저장 관측 기준)
        void MySqlDBProvider::ArmInventoryFaultOnce() { ::InterlockedExchange(&g_h4FaultArm, 1); }
        long MySqlDBProvider::GetH4FaultInjected() { return g_h4FaultInjected; }
        long MySqlDBProvider::GetH4ResaveObserved() { return g_h4ResaveObserved; }
#endif

        // 파일-로컬 인코딩 헬퍼: wchar_t(UTF-16) -> UTF-8 char.
        //   반환 = 변환된 UTF-8 바이트 수(널 제외). 바인딩 길이로 그대로 쓴다.
        //   실패(버퍼 오버플로 / 깨진 입력)면 0 + 빈 문자열 -> 빈 값 바인딩(안전).
        static int ToUtf8(const wchar_t* w, char* out, int outCap)
        {
            if (w == nullptr) { out[0] = '\0'; return 0; }
            // -1 = 널 종단까지 변환. CP_UTF8 은 마지막 두 인자가 반드시 NULL.
            int n = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out, outCap, nullptr, nullptr);
            return (n > 0) ? (n - 1) : 0;   // n 은 널 포함 -> 바이트 길이는 n-1
        }
        // 파일-로컬 인코딩 헬퍼: UTF-8 char -> wchar_t(UTF-16). ToUtf8 의 역방향.
        //   char_id 키로 바뀌어 Load 가 char_name 컬럼을 SELECT 해 오므로(키=이름 복사 트릭 폐기) 변환이 필요하다.
        //   실패(깨진 입력 / 버퍼 부족)면 빈 문자열(안전). -1 = 널 종단까지 변환(널 포함 기록).
        static void ToWide(const char* utf8, wchar_t* out, int outCap)
        {
            if (out == nullptr || outCap <= 0) { return; }
            if (utf8 == nullptr) { out[0] = L'\0'; return; }
            int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, outCap);
            if (n <= 0) { out[0] = L'\0'; }   // 변환 실패(버퍼 부족 등) -> 빈 문자열
        }

        // 실패 직후의 연결 에러 텍스트를 지역 버퍼로 복사해 둔다 - 이후의 mysql_ping(생사 판정)이나
        //   ROLLBACK 이 성공하면서 연결 핸들의 에러 상태를 리셋해 원인이 소실되기 때문(로그는 복사본으로 찍는다).
        //   서버측 오류(제약 위반/권한/단절)는 연결 핸들에 남으므로 여기서 잡히고, 클라이언트측 stmt 바인딩
        //   오류(프로그래밍 실수)만 빈 문자열이 될 수 있다.
        static void CopyConnError(MYSQL* conn, char* out, size_t cap)
        {
            const char* msg = (conn != nullptr) ? mysql_error(conn) : "";
            if (msg == nullptr) { msg = ""; }
            strncpy_s(out, cap, msg, _TRUNCATE);
        }

        // 평문 SELECT COUNT(*) 한 줄을 정수로 (information_schema 스키마 점검 전용 - 사용자 입력 없음, 평문 안전).
        //   반환 = COUNT 값 / 조회 실패면 -1(호출측 fail-fast).
        static int QueryScalarCount(MYSQL* conn, const char* sql)
        {
            if (mysql_query(conn, sql) != 0) { return -1; }
            MYSQL_RES* res = mysql_store_result(conn);
            if (res == nullptr) { return -1; }
            int value = -1;
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row != nullptr && row[0] != nullptr)
            {
                value = static_cast<int>(strtol(row[0], nullptr, 10));
            }
            mysql_free_result(res);
            return value;
        }

        MySqlDBProvider::MySqlDBProvider()
            : m_conn(nullptr)
            , m_config()      // POD 라 명시 초기화 - 값이 없는 상태에서 읽히면 쓰레기 값이 된다
        {
            // 연결은 ctor 가 아니라 Connect()에서(DB 스레드 소유). 여기선 핸들만 null.
        }

        MySqlDBProvider::~MySqlDBProvider()
        {
            // 폴백 - 정상 경로에서는 DB 스레드가 Disconnect()로 이미 닫아 m_conn 이 nullptr 이라 아무 일도 안 한다.
            //   여기까지 오는 것은 스레드가 뜨지 못한 비정상 경로뿐이다(그 경우 mysql_library_end 전에 소멸한다).
            Disconnect();
        }

        // 연결 해제 - 멱등. 연 스레드가 닫는다(DB 스레드 루프 종료부).
        void MySqlDBProvider::Disconnect()
        {
            if (m_conn != nullptr)
            {
                mysql_close(m_conn);
                m_conn = nullptr;
            }
        }

        // 연결 + 스키마 부트스트랩 (DB 스레드에서 1회). 성공=true / 실패=false(호출측 fail-fast).
        bool MySqlDBProvider::Connect(const DbConfig& config)
        {
            m_config = config;   // 연결 정보 보관

            m_conn = mysql_init(nullptr);
            if (m_conn == nullptr)
            {
                return false;   // 핸들 할당 실패(OOM)
            }

            // charset 은 connect '전' 옵션으로 - 한글(utf8mb4) + 재연결에도 유지. 날 SET NAMES 금지.
            mysql_options(m_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

            // 네트워크 타임아웃 - 응답 없는 단절(silent-drop)에서도 연결/쿼리/ping 각 시도가 유한 시간에
            //   끝나게 해 재접속 정책(5회x3초)의 시간 상한을 지킨다. 값은 운영 조정 가능한 숫자 파라미터.
            unsigned int connectTimeoutSec = 10;
            unsigned int rwTimeoutSec      = 30;
            mysql_options(m_conn, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeoutSec);
            mysql_options(m_conn, MYSQL_OPT_READ_TIMEOUT,    &rwTimeoutSec);
            mysql_options(m_conn, MYSQL_OPT_WRITE_TIMEOUT,   &rwTimeoutSec);

            if (mysql_real_connect(m_conn, m_config.host, m_config.user, m_config.password,
                                   m_config.dbname, static_cast<unsigned int>(m_config.port),
                                   nullptr, 0) == nullptr)
            {
                // 연결 실패. 치명 여부는 호출측이 판정한다(부팅 = fail-fast / 재접속 루프 = 재시도).
                //   실제 MySQL 에러를 찍어 진단 가능하게(%S = wprintf 의 char* 형식).
                DbLog(LogLevel::LL_ERROR, L"mysql_real_connect 실패: %S", mysql_error(m_conn));
                mysql_close(m_conn);
                m_conn = nullptr;
                return false;
            }

            return EnsureSchema();   // 게임 서버 소유 테이블(characters/아이템/감사) 보장 - accounts 는 로그인 서버 소유
        }

        // 연결 끊김 복구. 작업 실패 직후 호출 - mysql_ping 왕복으로 연결 생사를 판정한다.
        //   MYSQL_OPT_RECONNECT 는 쓰지 않으므로(8.0.34+ deprecated + 무통지 세션 리셋 위험)
        //   ping 이 몰래 재접속하는 일은 없다 - 순수 생사 확인.
        bool MySqlDBProvider::RecoverIfConnectionLost()
        {
            if (m_conn != nullptr && mysql_ping(m_conn) == 0)
            {
                return false;   // 연결 정상 - 끊김이 아닌 다른 실패(SQL 오류 등)라 재실행 대상 아님
            }

            DbLog(LogLevel::LL_WARN, L"연결 끊김 감지 - 재접속 시도 (최대 %d회, %lums 간격)",
                      DB_RECONNECT_MAX_TRIES, DB_RECONNECT_RETRY_MS);
            for (int attempt = 1; attempt <= DB_RECONNECT_MAX_TRIES; ++attempt)
            {
                if (m_conn != nullptr)
                {
                    mysql_close(m_conn);   // 죽은 핸들 폐기 - Connect 가 새로 init
                    m_conn = nullptr;
                }
                if (Connect(m_config))   // 부팅과 같은 경로로 복구 (charset 옵션 + 스키마 멱등 점검 포함)
                {
                    InterlockedIncrement(&s_reconnectCount);
                    DbLog(LogLevel::LL_WARN, L"재접속 성공 (%d번째 시도)", attempt);
                    return true;
                }
                ::Sleep(DB_RECONNECT_RETRY_MS);
            }

            // 전부 실패 - DB 없이는 로그인/저장이 전부 막힌 반쪽 서버. 조용한 성능저하 대신 즉시 종료해
            //   운영자가 바로 알게 한다. 원인은 위 로그로 자명(외부 DB 다운)이라 덤프 없이 ExitProcess.
            DbLog(LogLevel::LL_FATAL, L"재접속 %d회 전부 실패 - 프로세스 종료", DB_RECONNECT_MAX_TRIES);
            ::ExitProcess(1);
        }

        // characters 스키마 자동 생성. 계정당 여러 캐릭터(슬롯)를 위해 char_id surrogate PK 로 바뀜.
        //   accounts 는 로그인 서버 전용 소유 - 게임 서버는 안 만든다(LoginServer EnsureAccountsSchema 가 생성).
        // DDL 실행 헬퍼 - CREATE 문 1개 실행, 실패면 실제 에러를 FATAL 로 찍고 false(부팅 중단). EnsureSchema 의 CREATE 5벌 수렴.
        static bool RunDdl(MYSQL* conn, const char* sql, const wchar_t* label)
        {
            if (mysql_query(conn, sql) != 0)
            {
                DbLog(LogLevel::LL_FATAL, L"CREATE %s 실패: %S", label, mysql_error(conn));
                return false;
            }
            return true;
        }

        bool MySqlDBProvider::EnsureSchema()
        {
            // 마이그레이션: 이전 characters(PK=name, char_id 없음)가 남아 있으면 통째 교체.
            //   char_id 컬럼 유무로 신/구 스키마를 가른다 - 없으면(구 스키마거나 테이블 미존재) DROP IF EXISTS 후
            //   재생성, 있으면(신 스키마) 아래 CREATE IF NOT EXISTS 가 no-op (멱등 - 매 부팅 안전).
            //   학습/개발 DB 전제(봇/테스트 캐릭만) - 1회 통째 교체가 안전하고 시드는 seed_bots.sql 이 재적재.
            static const char* const kHasCharId =
                "SELECT COUNT(*) FROM information_schema.COLUMNS "
                "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'char_id'";
            int hasCharId = QueryScalarCount(m_conn, kHasCharId);
            if (hasCharId < 0)
            {
                DbLog(LogLevel::LL_FATAL, L"characters 스키마 점검 실패: %S", mysql_error(m_conn));
                return false;
            }
            if (hasCharId == 0)
            {
                // 구 스키마(name PK)거나 테이블 미존재 - 비호환이라 비우고 재생성(IF EXISTS 라 미존재면 무해).
                if (mysql_query(m_conn, "DROP TABLE IF EXISTS characters") != 0)
                {
                    DbLog(LogLevel::LL_FATAL, L"구 characters DROP 실패: %S", mysql_error(m_conn));
                    return false;
                }
            }

            // 신 스키마: char_id surrogate PK + account_id(논리 FK) + char_name UNIQUE + slot_id.
            //   account_id 는 enforced FOREIGN KEY 가 아니라 인덱스만 둔다 - accounts 는 LoginServer 소유라
            //   부팅 순서 결합(게임 서버 characters 가 먼저 떠도 무방)을 피하고, 소유 검증은 앱(WHERE account_id)이
            //   한다(rAthena 등 게임 서버 관행 - 앱 enforce). char_name UNIQUE 로 중복 이름 차단.
            static const char* const kCreateCharacters =
                "CREATE TABLE IF NOT EXISTS characters ("
                " char_id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
                " account_id INT UNSIGNED NOT NULL,"
                " char_name VARCHAR(16) NOT NULL,"
                " slot_id TINYINT UNSIGNED NOT NULL,"
                " map_id INT NOT NULL DEFAULT 0,"
                " x INT NOT NULL DEFAULT 0,"
                " y INT NOT NULL DEFAULT 0,"
                " hp INT NOT NULL DEFAULT 100,"
                " max_hp INT NOT NULL DEFAULT 100,"
                " mp INT NOT NULL DEFAULT 0,"
                " PRIMARY KEY (char_id),"
                " UNIQUE KEY uq_char_name (char_name),"
                " UNIQUE KEY uq_account_slot (account_id, slot_id)"   // 계정당 슬롯 1캐릭 강제 + account_id 조회 인덱스 겸용
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci";

            if (!RunDdl(m_conn, kCreateCharacters, L"characters")) { return false; }

            // 아이템 실물 - 한 행 = 실존 아이템 하나(스택 포함, 수량은 quantity 컬럼).
            //   item_uid 는 서버 인메모리 발급기가 정하므로 AUTO_INCREMENT 가 아니다 (비동기 INSERT 완료를
            //   기다리지 않고 획득 순간 uid 확정 - LoadMaxItemUid 로 부팅 시 발급기를 시드).
            //   template_id 는 DB 밖(Data/items.csv) 참조라 FK 불가 - 로드 시 앱이 검증.
            static const char* const kCreateItemInstance =
                "CREATE TABLE IF NOT EXISTS item_instance ("
                " item_uid BIGINT UNSIGNED NOT NULL,"
                " template_id INT NOT NULL,"
                " quantity INT NOT NULL DEFAULT 1,"
                " PRIMARY KEY (item_uid)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci";
            if (!RunDdl(m_conn, kCreateItemInstance, L"item_instance")) { return false; }

            // 배치 - 어느 캐릭터의 어느 칸에 어떤 실물이 있나. PRIMARY KEY(item_uid) 가
            //   "한 실물 = 정확히 한 위치"를 스키마 차원에서 강제한다 (같은 아이템이 두 캐릭터/두 칸에
            //   동시에 배치된 복제 상태는 표현 자체가 불가). char_id 는 enforced FK 가 아니라 앱 검증
            //   (characters.account_id 와 같은 관행 - 정적 재시드/마이그레이션 절차 단순화).
            static const char* const kCreateCharacterInventory =
                "CREATE TABLE IF NOT EXISTS character_inventory ("
                " char_id INT UNSIGNED NOT NULL,"
                " slot SMALLINT UNSIGNED NOT NULL,"
                " item_uid BIGINT UNSIGNED NOT NULL,"
                " PRIMARY KEY (item_uid),"
                " UNIQUE KEY uq_char_slot (char_id, slot),"   // 한 칸 = 실물 하나 + char_id 조회 인덱스 겸용
                " KEY idx_char (char_id)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci";
            if (!RunDdl(m_conn, kCreateCharacterInventory, L"character_inventory")) { return false; }

            // 거래 감사(hot) - 한 행 = 거래에서 이동한 아이템 하나("누가 누구에게 무엇을, 언제").
            //   old/new uid 짝이 아이템 계보를 잇는다(dupe/사기 조사 = uid 생애 추적). CS 복구 조회가
            //   캐릭터/기간/uid 축으로 들어오므로 그 축들에 인덱스를 둔다.
            //   (trade_uid,row_idx) UNIQUE = 중복 차단 키 - 커밋 응답 유실 후 같은 거래를 재실행해도 증거 행이 두 벌 안 남는다.
            static const char* const kCreateTradeAudit =
                "CREATE TABLE IF NOT EXISTS trade_audit ("
                " audit_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
                " trade_uid BIGINT UNSIGNED NOT NULL,"
                " row_idx INT NOT NULL,"
                " traded_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                " giver_char_id INT UNSIGNED NOT NULL,"
                " receiver_char_id INT UNSIGNED NOT NULL,"
                " old_item_uid BIGINT UNSIGNED NOT NULL,"
                " new_item_uid BIGINT UNSIGNED NOT NULL,"
                " template_id INT NOT NULL,"
                " quantity INT NOT NULL,"
                " prev_quantity INT NOT NULL,"
                " PRIMARY KEY (audit_id),"
                " UNIQUE KEY uq_trade_row (trade_uid, row_idx),"
                " KEY idx_giver (giver_char_id, traded_at),"
                " KEY idx_receiver (receiver_char_id, traded_at),"
                " KEY idx_new_uid (new_item_uid)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci";
            if (!RunDdl(m_conn, kCreateTradeAudit, L"trade_audit")) { return false; }

            // 거래 감사(cold) - hot 에서 보존 기간이 지난 행이 이관되는 아카이브. 증거는 소멸하지 않고
            //   접근만 느려진다. audit_id 는 이관 값 그대로 보존(AUTO_INCREMENT 없음).
            static const char* const kCreateTradeAuditArchive =
                "CREATE TABLE IF NOT EXISTS trade_audit_archive ("
                " audit_id BIGINT UNSIGNED NOT NULL,"
                " trade_uid BIGINT UNSIGNED NOT NULL,"
                " row_idx INT NOT NULL,"
                " traded_at TIMESTAMP NOT NULL,"
                " giver_char_id INT UNSIGNED NOT NULL,"
                " receiver_char_id INT UNSIGNED NOT NULL,"
                " old_item_uid BIGINT UNSIGNED NOT NULL,"
                " new_item_uid BIGINT UNSIGNED NOT NULL,"
                " template_id INT NOT NULL,"
                " quantity INT NOT NULL,"
                " prev_quantity INT NOT NULL,"
                " PRIMARY KEY (audit_id)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci";
            if (!RunDdl(m_conn, kCreateTradeAuditArchive, L"trade_audit_archive")) { return false; }

            // 두 감사 테이블이 만들어진(또는 이미 있던) 뒤 - 중복 차단 키가 없는 선재 DB 를 마이그레이션한다.
            if (!EnsureTradeAuditDedupKey()) { return false; }
            return true;
        }

        // information_schema 존재 확인 - COUNT 쿼리 1개를 실행해 0 초과 여부를 outExists 로 돌려준다.
        //   반환 = 쿼리 자체 성공 여부(실패 = 부팅 중단 신호 - 존재를 모른 채 ALTER 를 강행하지 않는다).
        static bool QueryExists(MYSQL* conn, const char* sql, bool& outExists)
        {
            outExists = false;
            if (mysql_query(conn, sql) != 0) { return false; }
            MYSQL_RES* res = mysql_store_result(conn);
            if (res == nullptr) { return false; }
            bool ok = false;
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row != nullptr && row[0] != nullptr)
            {
                outExists = (strtol(row[0], nullptr, 10) > 0);
                ok = true;
            }
            mysql_free_result(res);
            return ok;
        }

        // 선재 DB 마이그레이션 - 중복 차단 키(trade_uid/row_idx + UNIQUE)가 없는 기존 감사 테이블에 추가한다.
        //   기존 행은 trade_uid = audit_id 로 backfill (audit_id 가 유일이라 (trade_uid, row_idx=0) 유일성 충족).
        //   컬럼/인덱스는 각각 확인해 건너뛰되 backfill 은 매 부팅 무조건 실행한다(멱등: 신규 형식 행은
        //   trade_uid >= 1 이라 WHERE trade_uid = 0 대상이 없다) - ALTER(암묵 커밋) 직후 끊긴 부팅의
        //   잔여 (0,0) 행도 다음 부팅 backfill 이 채워 UNIQUE 생성이 항상 성립한다(재개 보장).
        //   신규 설치는 CREATE 가 컬럼/UNIQUE 를 이미 포함하므로 ALTER/UNIQUE 는 건너뛰고 backfill 은 대상 0건.
        bool MySqlDBProvider::EnsureTradeAuditDedupKey()
        {
            bool exists = false;

            // (1) hot 컬럼 추가
            if (!QueryExists(m_conn,
                "SELECT COUNT(*) FROM information_schema.COLUMNS"
                " WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'trade_audit' AND COLUMN_NAME = 'trade_uid'", exists))
            {
                DbLog(LogLevel::LL_FATAL, L"trade_audit 마이그레이션 - 컬럼 확인 실패: %S", mysql_error(m_conn));
                return false;
            }
            if (!exists)
            {
                if (mysql_query(m_conn, "ALTER TABLE trade_audit"
                        " ADD COLUMN trade_uid BIGINT UNSIGNED NOT NULL DEFAULT 0,"
                        " ADD COLUMN row_idx INT NOT NULL DEFAULT 0") != 0)
                {
                    DbLog(LogLevel::LL_FATAL, L"trade_audit 중복 차단 키 컬럼 마이그레이션 실패: %S", mysql_error(m_conn));
                    return false;
                }
            }

            // (1b) hot backfill - 컬럼 게이트 밖에서 무조건 실행 (ALTER 만 되고 끊긴 부팅의 잔여 0 행을 이어서 채움)
            if (mysql_query(m_conn, "UPDATE trade_audit SET trade_uid = audit_id WHERE trade_uid = 0") != 0)
            {
                DbLog(LogLevel::LL_FATAL, L"trade_audit 중복 차단 키 backfill 실패: %S", mysql_error(m_conn));
                return false;
            }

            // (2) hot UNIQUE - 컬럼과 별도 확인 (컬럼/backfill 후 끊긴 부팅이 여기서 이어짐)
            if (!QueryExists(m_conn,
                "SELECT COUNT(*) FROM information_schema.STATISTICS"
                " WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'trade_audit' AND INDEX_NAME = 'uq_trade_row'", exists))
            {
                DbLog(LogLevel::LL_FATAL, L"trade_audit 마이그레이션 - 인덱스 확인 실패: %S", mysql_error(m_conn));
                return false;
            }
            if (!exists)
            {
                if (mysql_query(m_conn, "ALTER TABLE trade_audit ADD UNIQUE KEY uq_trade_row (trade_uid, row_idx)") != 0)
                {
                    DbLog(LogLevel::LL_FATAL, L"trade_audit 중복 차단 키 UNIQUE 마이그레이션 실패: %S", mysql_error(m_conn));
                    return false;
                }
                DbLog(LogLevel::LL_INFO, L"trade_audit 중복 차단 키 마이그레이션 완료 (trade_uid/row_idx + UNIQUE)");
            }

            // (3) archive 컬럼 - UNIQUE 불요 (PK audit_id 가 이관 값 그대로 유일)
            if (!QueryExists(m_conn,
                "SELECT COUNT(*) FROM information_schema.COLUMNS"
                " WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'trade_audit_archive' AND COLUMN_NAME = 'trade_uid'", exists))
            {
                DbLog(LogLevel::LL_FATAL, L"trade_audit_archive 마이그레이션 - 컬럼 확인 실패: %S", mysql_error(m_conn));
                return false;
            }
            if (!exists)
            {
                if (mysql_query(m_conn, "ALTER TABLE trade_audit_archive"
                        " ADD COLUMN trade_uid BIGINT UNSIGNED NOT NULL DEFAULT 0,"
                        " ADD COLUMN row_idx INT NOT NULL DEFAULT 0") != 0)
                {
                    DbLog(LogLevel::LL_FATAL, L"trade_audit_archive 중복 차단 키 컬럼 마이그레이션 실패: %S", mysql_error(m_conn));
                    return false;
                }
            }

            // (3b) archive backfill - hot 과 같은 이유로 컬럼 게이트 밖에서 무조건 실행 (잔여 0 행 이어서 채움)
            if (mysql_query(m_conn, "UPDATE trade_audit_archive SET trade_uid = audit_id WHERE trade_uid = 0") != 0)
            {
                DbLog(LogLevel::LL_FATAL, L"trade_audit_archive 중복 차단 키 backfill 실패: %S", mysql_error(m_conn));
                return false;
            }
            return true;
        }

        // 캐릭터 1행 로드 (재시도 래퍼). 실패가 연결 끊김이면 재접속 후 같은 조회를 1회 재실행한다(읽기라 안전).
        bool MySqlDBProvider::Load(UINT32 charId, UINT32 accountId, DBResult& out)
        {
            if (LoadOnce(charId, accountId, out)) { return true; }
            char errText[256];
            CopyConnError(m_conn, errText, sizeof(errText));   // ping(생사 판정)이 에러를 리셋하기 전에 원인 확보
            if (RecoverIfConnectionLost())
            {
                if (LoadOnce(charId, accountId, out)) { return true; }
                CopyConnError(m_conn, errText, sizeof(errText));   // 재실행도 실패 - 그 원인으로 갱신
            }
            InterlockedIncrement(&s_queryFailCount);
            DbLog(LogLevel::LL_ERROR, L"캐릭터 로드 실패 (char_id=%u): %S", charId, errText);
            return false;
        }

        // 캐릭터 1행 로드 (1회 시도 본체). char_id 로 찾되 account_id 가 일치해야 한다(타 계정 캐릭터 로드 차단 = 소유 검증).
        //   반환 = backend 도달 성공 여부 / found 는 out 으로 운반(미발견 = 캐릭터 없음 또는 타 계정 = 로그인 실패).
        bool MySqlDBProvider::LoadOnce(UINT32 charId, UINT32 accountId, DBResult& out)
        {
            out.found = false;   // 기본 미발견(호출측이 로그인 실패 처리 - 신규 자동생성 폐기)

            MYSQL_STMT* stmt = mysql_stmt_init(m_conn);
            if (stmt == nullptr) { return false; }

            // 키가 숫자(char_id)라 이름은 더 이상 키 복사로 못 채운다 -> char_name 컬럼을 SELECT 해 ToWide 로 변환.
            const char* sql = "SELECT char_name, map_id, x, y, hp, max_hp, mp "
                              "FROM characters WHERE char_id = ? AND account_id = ?";
            if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(strlen(sql))) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            // 입력 파라미터(?) 2개 = char_id, account_id. 둘 다 UNSIGNED INT.
            unsigned int pin[2] = { charId, accountId };
            MYSQL_BIND pbind[2];
            memset(pbind, 0, sizeof(pbind));
            for (int i = 0; i < 2; ++i)
            {
                pbind[i].buffer_type = MYSQL_TYPE_LONG;
                pbind[i].buffer      = &pin[i];
                pbind[i].is_unsigned  = 1;   // char_id/account_id 는 UNSIGNED
            }
            if (mysql_stmt_bind_param(stmt, pbind) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            if (mysql_stmt_execute(stmt) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            // 결과 = char_name(문자열) + 6 INT(map_id,x,y,hp,max_hp,mp). 문자열 result bind 는 버퍼+length.
            char    nameUtf8[64] = { 0 };   // char_name VARCHAR(16) -> UTF-8 최대 ~48바이트, 64 면 충분(잘림 없음)
            unsigned long nameLen = 0;
            int v[6] = { 0, 0, 0, 0, 0, 0 };   // map_id, x, y, hp, max_hp, mp
            MYSQL_BIND rbind[7];
            memset(rbind, 0, sizeof(rbind));
            rbind[0].buffer_type   = MYSQL_TYPE_STRING;   // char_name
            rbind[0].buffer        = nameUtf8;
            rbind[0].buffer_length = sizeof(nameUtf8);
            rbind[0].length        = &nameLen;
            for (int i = 0; i < 6; ++i)
            {
                rbind[i + 1].buffer_type = MYSQL_TYPE_LONG;
                rbind[i + 1].buffer      = &v[i];
            }
            if (mysql_stmt_bind_result(stmt, rbind) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            // char_id 는 PK 라 최대 1행. 버퍼가 충분(64>=48)이라 문자열 절단 없음.
            int fetch = mysql_stmt_fetch(stmt);
            if (fetch == 0)
            {
                // UTF-8 char_name 을 널 종단 후 wchar_t 로 변환.
                unsigned long n = (nameLen < sizeof(nameUtf8)) ? nameLen : (sizeof(nameUtf8) - 1);
                nameUtf8[n] = '\0';
                ToWide(nameUtf8, out.name, WHISPER_NAME_MAX);
                out.charId = charId;
                out.mapId  = v[0];
                out.x      = v[1];
                out.y      = v[2];
                out.hp     = v[3];
                out.maxHp  = v[4];
                out.mp     = v[5];
                out.found  = true;
            }

            mysql_stmt_close(stmt);
            // fetch 0(행 있음) / MYSQL_NO_DATA(미발견) 둘 다 backend 도달 성공. 1/에러면 false(연결 끊김 등).
            return (fetch == 0 || fetch == MYSQL_NO_DATA);
        }

        // 캐릭터 상태 통째 저장 (재시도 래퍼). UPDATE 는 멱등이라 연결 끊김 재접속 후 1회 재실행이 안전.
        //   최종 실패는 로그 + 카운터로 관측 - 인메모리가 권위라 다음 저장이 따라잡는다.
        void MySqlDBProvider::SaveFull(const DBResult& snapshot)
        {
            if (SaveFullOnce(snapshot)) { return; }
            char errText[256];
            CopyConnError(m_conn, errText, sizeof(errText));   // ping(생사 판정)이 에러를 리셋하기 전에 원인 확보
            if (RecoverIfConnectionLost())
            {
                if (SaveFullOnce(snapshot)) { return; }
                CopyConnError(m_conn, errText, sizeof(errText));   // 재실행도 실패 - 그 원인으로 갱신
            }
            InterlockedIncrement(&s_queryFailCount);
            DbLog(LogLevel::LL_ERROR, L"캐릭터 저장 실패 (char_id=%u): %S", snapshot.charId, errText);
        }

        // 캐릭터 상태 통째 저장 (1회 시도 본체). 캐릭터 행은 CreateCharacter 가 미리 만들어 두므로(신규 자동생성 폐기) 순수 UPDATE.
        //   char_id 는 전역 PK 라 WHERE char_id=? 한 줄로 정확히 그 캐릭터만 갱신(소유 위반 write 구조적 불가).
        //   account_id/char_name/slot_id 는 불변이라 UPDATE 대상이 아니다 -> 스냅샷에 char_id + 가변 6필드만 필요.
        bool MySqlDBProvider::SaveFullOnce(const DBResult& snapshot)
        {
            MYSQL_STMT* stmt = mysql_stmt_init(m_conn);
            if (stmt == nullptr) { return false; }

            const char* sql =
                "UPDATE characters SET map_id=?, x=?, y=?, hp=?, max_hp=?, mp=? WHERE char_id=?";
            if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(strlen(sql))) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            int iv[6] = { snapshot.mapId, snapshot.x, snapshot.y, snapshot.hp, snapshot.maxHp, snapshot.mp };
            unsigned int charId = snapshot.charId;

            MYSQL_BIND b[7];
            memset(b, 0, sizeof(b));
            for (int i = 0; i < 6; ++i)
            {
                b[i].buffer_type = MYSQL_TYPE_LONG;   // map_id, x, y, hp, max_hp, mp (signed INT)
                b[i].buffer      = &iv[i];
            }
            b[6].buffer_type = MYSQL_TYPE_LONG;   // char_id (UNSIGNED)
            b[6].buffer      = &charId;
            b[6].is_unsigned  = 1;

            // 실행 성공 여부를 반환한다 - 래퍼가 실패 시 재시도/로깅/카운터를 처리.
            bool ok = false;
            if (mysql_stmt_bind_param(stmt, b) == 0)
            {
                ok = (mysql_stmt_execute(stmt) == 0);
            }
            mysql_stmt_close(stmt);
            return ok;
        }

        // 계정의 캐릭터 목록 로드 (재시도 래퍼). 읽기 전용 + 본체가 out 을 처음부터 다시 채우므로 재실행 안전.
        bool MySqlDBProvider::LoadCharacterList(UINT32 accountId, CharListResult& out)
        {
            if (LoadCharacterListOnce(accountId, out)) { return true; }
            char errText[256];
            CopyConnError(m_conn, errText, sizeof(errText));   // ping(생사 판정)이 에러를 리셋하기 전에 원인 확보
            if (RecoverIfConnectionLost())
            {
                if (LoadCharacterListOnce(accountId, out)) { return true; }
                CopyConnError(m_conn, errText, sizeof(errText));   // 재실행도 실패 - 그 원인으로 갱신
            }
            InterlockedIncrement(&s_queryFailCount);
            DbLog(LogLevel::LL_ERROR, L"캐릭터 목록 로드 실패 (account_id=%u): %S", accountId, errText);
            return false;   // backend 도달 실패 - 호출측(SendCharacterList)이 빈/부분 목록 권위 송신을 건너뛴다
        }

        // 계정의 캐릭터 목록 로드 (1회 시도 본체). slot_id 순서로 최대 MAX_CHAR_SLOTS 행.
        //   소유 범위 = WHERE account_id (그 계정 캐릭만). 결과는 CharListResult 에 채워 호출 명령이 SC_CHARACTER_LIST 로 송신.
        bool MySqlDBProvider::LoadCharacterListOnce(UINT32 accountId, CharListResult& out)
        {
            out.count = 0;
            MYSQL_STMT* stmt = mysql_stmt_init(m_conn);
            if (stmt == nullptr) { return false; }

            const char* sql = "SELECT char_id, char_name, slot_id, map_id, hp, max_hp "
                              "FROM characters WHERE account_id = ? ORDER BY slot_id";
            if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(strlen(sql))) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            unsigned int accountIdBind = accountId;
            MYSQL_BIND pbind[1];
            memset(pbind, 0, sizeof(pbind));
            pbind[0].buffer_type = MYSQL_TYPE_LONG;
            pbind[0].buffer      = &accountIdBind;
            pbind[0].is_unsigned  = 1;
            if (mysql_stmt_bind_param(stmt, pbind) != 0 || mysql_stmt_execute(stmt) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            // 결과 = char_id, char_name(문자열), slot_id, map_id, hp, max_hp. 행마다 같은 버퍼에 fetch 후 out 으로 복사.
            unsigned int charId = 0;
            char         nameUtf8[64] = { 0 };
            unsigned long nameLen = 0;
            unsigned int slotId = 0;
            int          mapId = 0, hp = 0, maxHp = 0;
            MYSQL_BIND rbind[6];
            memset(rbind, 0, sizeof(rbind));
            rbind[0].buffer_type = MYSQL_TYPE_LONG;   rbind[0].buffer = &charId; rbind[0].is_unsigned = 1;
            rbind[1].buffer_type = MYSQL_TYPE_STRING; rbind[1].buffer = nameUtf8; rbind[1].buffer_length = sizeof(nameUtf8); rbind[1].length = &nameLen;
            rbind[2].buffer_type = MYSQL_TYPE_LONG;   rbind[2].buffer = &slotId; rbind[2].is_unsigned = 1;
            rbind[3].buffer_type = MYSQL_TYPE_LONG;   rbind[3].buffer = &mapId;
            rbind[4].buffer_type = MYSQL_TYPE_LONG;   rbind[4].buffer = &hp;
            rbind[5].buffer_type = MYSQL_TYPE_LONG;   rbind[5].buffer = &maxHp;
            if (mysql_stmt_bind_result(stmt, rbind) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            // UNIQUE(account_id,slot_id) 라 행 수 <= MAX_CHAR_SLOTS. cap 으로 over-write 방어.
            int fetch = 0;
            while (out.count < MAX_CHAR_SLOTS && (fetch = mysql_stmt_fetch(stmt)) == 0)
            {
                CharRow& r = out.rows[out.count];
                r.charId = charId;
                unsigned long n = (nameLen < sizeof(nameUtf8)) ? nameLen : (sizeof(nameUtf8) - 1);
                nameUtf8[n] = '\0';
                ToWide(nameUtf8, r.name, WHISPER_NAME_MAX);
                r.slotId = static_cast<BYTE>(slotId);
                r.mapId  = mapId;
                r.hp     = hp;
                r.maxHp  = maxHp;
                ++out.count;
            }
            mysql_stmt_close(stmt);
            // 정상 종료 = 행 소진(MYSQL_NO_DATA) 또는 상한 도달(fetch==0). 그 외는 fetch 중 오류(연결 끊김 포함) -
            //   성공으로 오인하면 부분/빈 목록이 클라에 서버 권위로 송신된다(LoadOnce/LoadInventoryOnce 와 같은 검사).
            return (fetch == MYSQL_NO_DATA || fetch == 0);
        }

        // 캐릭터 생성 (재시도 래퍼). DB_ERROR 이고 원인이 연결 끊김이면 재접속 후 1회 재실행 -
        //   본체가 검사(슬롯/이름)부터 다시 하므로 이중 INSERT 는 구조적으로 불가.
        BYTE MySqlDBProvider::CreateCharacter(UINT32 accountId, const wchar_t* name, BYTE slotId, UINT32& outCharId)
        {
            BYTE result = CreateCharacterOnce(accountId, name, slotId, outCharId);
            if (result == static_cast<BYTE>(ECharCreateResult::DB_ERROR))
            {
                char errText[256];
                CopyConnError(m_conn, errText, sizeof(errText));   // ping(생사 판정)이 에러를 리셋하기 전에 원인 확보
                if (RecoverIfConnectionLost())
                {
                    result = CreateCharacterOnce(accountId, name, slotId, outCharId);
                    if (result == static_cast<BYTE>(ECharCreateResult::DB_ERROR))
                    {
                        CopyConnError(m_conn, errText, sizeof(errText));   // 재실행도 실패 - 그 원인으로 갱신
                    }
                }
                if (result == static_cast<BYTE>(ECharCreateResult::DB_ERROR))
                {
                    InterlockedIncrement(&s_queryFailCount);
                    DbLog(LogLevel::LL_ERROR, L"캐릭터 생성 실패 (account_id=%u): %S", accountId, errText);
                }
            }
            return result;
        }

        // 캐릭터 생성 (1회 시도 본체). 반환 = ECharCreateResult 값(BYTE). 성공이면 outCharId 에 새 char_id(AUTO_INCREMENT).
        //   단일 DBThread 직렬 실행이라 "검사 후 INSERT" 사이 race 없음 -> 사전 검사(슬롯/이름 중복)가 정확.
        BYTE MySqlDBProvider::CreateCharacterOnce(UINT32 accountId, const wchar_t* name, BYTE slotId, UINT32& outCharId)
        {
            outCharId = 0;
            // 입력 검증: 이름 비었거나 너무 길거나(16=15자+널) 슬롯 범위 밖이면 INVALID.
            if (name == nullptr || name[0] == L'\0' || ::wcslen(name) >= WHISPER_NAME_MAX)
            {
                return static_cast<BYTE>(ECharCreateResult::INVALID);
            }
            if (slotId >= MAX_CHAR_SLOTS)
            {
                return static_cast<BYTE>(ECharCreateResult::INVALID);
            }
            char nameUtf8[64];
            unsigned long nameLen = static_cast<unsigned long>(ToUtf8(name, nameUtf8, sizeof(nameUtf8)));
            if (nameLen == 0)
            {
                return static_cast<BYTE>(ECharCreateResult::INVALID);   // 변환 실패/빈 이름
            }

            // (1) 슬롯 점유 검사: 그 계정의 그 슬롯에 이미 캐릭이 있으면 SLOT_TAKEN.
            {
                MYSQL_STMT* st = mysql_stmt_init(m_conn);
                if (st == nullptr) { return static_cast<BYTE>(ECharCreateResult::DB_ERROR); }
                const char* q = "SELECT COUNT(*) FROM characters WHERE account_id = ? AND slot_id = ?";
                if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(strlen(q))) != 0) { mysql_stmt_close(st); return static_cast<BYTE>(ECharCreateResult::DB_ERROR); }
                unsigned int pin[2] = { accountId, slotId };
                MYSQL_BIND pb[2]; memset(pb, 0, sizeof(pb));
                for (int i = 0; i < 2; ++i) { pb[i].buffer_type = MYSQL_TYPE_LONG; pb[i].buffer = &pin[i]; pb[i].is_unsigned = 1; }
                int cnt = 0; MYSQL_BIND rb[1]; memset(rb, 0, sizeof(rb)); rb[0].buffer_type = MYSQL_TYPE_LONG; rb[0].buffer = &cnt;
                if (mysql_stmt_bind_param(st, pb) != 0 || mysql_stmt_execute(st) != 0 || mysql_stmt_bind_result(st, rb) != 0)
                { mysql_stmt_close(st); return static_cast<BYTE>(ECharCreateResult::DB_ERROR); }
                mysql_stmt_fetch(st);
                mysql_stmt_close(st);
                if (cnt > 0) { return static_cast<BYTE>(ECharCreateResult::SLOT_TAKEN); }
            }

            // (2) 이름 중복 검사: char_name 은 전역 UNIQUE -> 이미 있으면 DUP_NAME.
            {
                MYSQL_STMT* st = mysql_stmt_init(m_conn);
                if (st == nullptr) { return static_cast<BYTE>(ECharCreateResult::DB_ERROR); }
                const char* q = "SELECT COUNT(*) FROM characters WHERE char_name = ?";
                if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(strlen(q))) != 0) { mysql_stmt_close(st); return static_cast<BYTE>(ECharCreateResult::DB_ERROR); }
                MYSQL_BIND pb[1]; memset(pb, 0, sizeof(pb)); pb[0].buffer_type = MYSQL_TYPE_STRING; pb[0].buffer = nameUtf8; pb[0].buffer_length = sizeof(nameUtf8); pb[0].length = &nameLen;
                int cnt = 0; MYSQL_BIND rb[1]; memset(rb, 0, sizeof(rb)); rb[0].buffer_type = MYSQL_TYPE_LONG; rb[0].buffer = &cnt;
                if (mysql_stmt_bind_param(st, pb) != 0 || mysql_stmt_execute(st) != 0 || mysql_stmt_bind_result(st, rb) != 0)
                { mysql_stmt_close(st); return static_cast<BYTE>(ECharCreateResult::DB_ERROR); }
                mysql_stmt_fetch(st);
                mysql_stmt_close(st);
                if (cnt > 0) { return static_cast<BYTE>(ECharCreateResult::DUP_NAME); }
            }

            // (3) INSERT - 신규 캐릭 기본값(맵 0 부활 지점, 풀피). char_id 는 AUTO_INCREMENT.
            //   시작 좌표는 maps.csv respawn 을 원천으로 쓴다 - 하드코딩 중앙 좌표는 통행 검증 밖이라
            //   지형 파일이 바뀌면 신규 캐릭 전원이 벽 안에서 시작하는 사각이 된다 (부활 좌표는 부팅 검증이 통행 보장).
            {
                MYSQL_STMT* st = mysql_stmt_init(m_conn);
                if (st == nullptr) { return static_cast<BYTE>(ECharCreateResult::DB_ERROR); }
                const char* q = "INSERT INTO characters(account_id, char_name, slot_id, map_id, x, y, hp, max_hp, mp) "
                                "VALUES(?,?,?,?,?,?,?,?,?)";
                if (mysql_stmt_prepare(st, q, static_cast<unsigned long>(strlen(q))) != 0) { mysql_stmt_close(st); return static_cast<BYTE>(ECharCreateResult::DB_ERROR); }
                unsigned int accountIdBind = accountId, slot = slotId;
                const Position startPos = MapTableAt(0).respawn;
                int iv[6] = { 0, startPos.x, startPos.y, PLAYER_DEFAULT_MAX_HP, PLAYER_DEFAULT_MAX_HP, 0 };   // map_id,x,y,hp,max_hp,mp
                MYSQL_BIND b[9]; memset(b, 0, sizeof(b));
                b[0].buffer_type = MYSQL_TYPE_LONG;   b[0].buffer = &accountIdBind; b[0].is_unsigned = 1;
                b[1].buffer_type = MYSQL_TYPE_STRING; b[1].buffer = nameUtf8; b[1].buffer_length = sizeof(nameUtf8); b[1].length = &nameLen;
                b[2].buffer_type = MYSQL_TYPE_LONG;   b[2].buffer = &slot; b[2].is_unsigned = 1;
                for (int i = 0; i < 6; ++i) { b[i + 3].buffer_type = MYSQL_TYPE_LONG; b[i + 3].buffer = &iv[i]; }
                if (mysql_stmt_bind_param(st, b) != 0 || mysql_stmt_execute(st) != 0)
                { mysql_stmt_close(st); return static_cast<BYTE>(ECharCreateResult::DB_ERROR); }   // race 로 들어온 UNIQUE 위반도 여기서 DB_ERROR
                outCharId = static_cast<UINT32>(mysql_stmt_insert_id(st));
                mysql_stmt_close(st);
            }
            return static_cast<BYTE>(ECharCreateResult::OK);
        }

        // 캐릭터 삭제 (재시도 래퍼). DB_ERROR 가 연결 끊김이면 재접속 후 트랜잭션 전체를 1회 재실행한다 -
        //   hard delete 는 멱등(이미 지운 행 재삭제 = 대상 0). 단 모호 COMMIT(실제로는 지워졌는데 응답 유실) 뒤의
        //   재실행은 NOT_OWNED 로 돌아온다(행이 이미 없음) - 캐릭터는 실제 삭제된 상태고 클라 목록은 재접속 시 정리된다.
        BYTE MySqlDBProvider::DeleteCharacter(UINT32 accountId, UINT32 charId)
        {
            BYTE result = DeleteCharacterOnce(accountId, charId);
            if (result == static_cast<BYTE>(ECharDeleteResult::DB_ERROR) && RecoverIfConnectionLost())
            {
                result = DeleteCharacterOnce(accountId, charId);
            }
            if (result == static_cast<BYTE>(ECharDeleteResult::DB_ERROR))
            {
                InterlockedIncrement(&s_queryFailCount);
            }
            return result;
        }

        // 캐릭터 삭제 (1회 시도 본체 - 즉시 hard delete). 반환 = ECharDeleteResult 값(BYTE).
        //   캐릭터 행과 인벤토리(배치 character_inventory + 실물 item_instance) 행을 한 트랜잭션으로 지운다 -
        //   characters 만 지우면 고아 인벤 행이 영구 누적되고 item_uid 시드 범위에도 계속 남기 때문.
        //   순서 = characters 먼저(affected_rows 가 소유 검증을 겸함) -> 인벤 wipe.
        //   소유가 아니면(affected 0) 롤백으로 끝나 인벤은 손대지 않는다.
        BYTE MySqlDBProvider::DeleteCharacterOnce(UINT32 accountId, UINT32 charId)
        {
            if (m_conn == nullptr) { return static_cast<BYTE>(ECharDeleteResult::DB_ERROR); }

            Begin();
            if (mysql_errno(m_conn) != 0)   // START TRANSACTION 실패 - 진입 중단 (SaveInventoryOnce 와 대칭)
            {
                DbLog(LogLevel::LL_ERROR, L"캐릭터 삭제 - 트랜잭션 시작 실패 (char_id=%u): %S", charId, mysql_error(m_conn));
                return static_cast<BYTE>(ECharDeleteResult::DB_ERROR);
            }

            // (1) characters 행 삭제 - affected_rows == 0 이면 미존재거나 타 계정 캐릭 -> NOT_OWNED.
            MYSQL_STMT* stmt = mysql_stmt_init(m_conn);
            if (stmt == nullptr) { Rollback(); return static_cast<BYTE>(ECharDeleteResult::DB_ERROR); }

            const char* sql = "DELETE FROM characters WHERE char_id = ? AND account_id = ?";
            if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(strlen(sql))) != 0)
            {
                mysql_stmt_close(stmt); Rollback(); return static_cast<BYTE>(ECharDeleteResult::DB_ERROR);
            }
            unsigned int pin[2] = { charId, accountId };
            MYSQL_BIND pbind[2]; memset(pbind, 0, sizeof(pbind));
            for (int i = 0; i < 2; ++i) { pbind[i].buffer_type = MYSQL_TYPE_LONG; pbind[i].buffer = &pin[i]; pbind[i].is_unsigned = 1; }
            if (mysql_stmt_bind_param(stmt, pbind) != 0 || mysql_stmt_execute(stmt) != 0)
            {
                mysql_stmt_close(stmt); Rollback(); return static_cast<BYTE>(ECharDeleteResult::DB_ERROR);
            }
            unsigned long long affected = mysql_stmt_affected_rows(stmt);
            mysql_stmt_close(stmt);
            if (affected == 0)
            {
                Rollback();   // 아무것도 안 지웠음 - 인벤도 건드리지 않는다
                return static_cast<BYTE>(ECharDeleteResult::NOT_OWNED);
            }

            // (2) 인벤토리(실물+배치) 행 삭제 - 전량 교체 저장과 같은 helper (트랜잭션 안 전용).
            if (!WipeInventoryRows(charId))
            {
                char errText[256];
                CopyConnError(m_conn, errText, sizeof(errText));   // ROLLBACK 성공이 에러를 리셋하기 전에 원인 확보
                Rollback();   // characters 삭제까지 되돌아감 - 캐릭터는 그대로 남는다
                DbLog(LogLevel::LL_ERROR, L"캐릭터 삭제 - 인벤토리 정리 실패, 롤백 (char_id=%u): %S", charId, errText);
                return static_cast<BYTE>(ECharDeleteResult::DB_ERROR);
            }

            Commit();
            if (mysql_errno(m_conn) != 0)   // COMMIT 실패 = 미확정 (연결 단절 등) - 실패로 보고
            {
                DbLog(LogLevel::LL_ERROR, L"캐릭터 삭제 - COMMIT 실패 (char_id=%u): %S", charId, mysql_error(m_conn));
                return static_cast<BYTE>(ECharDeleteResult::DB_ERROR);
            }
            return static_cast<BYTE>(ECharDeleteResult::OK);
        }

        // 변경분 저장. 1차는 SaveFull 위임(CSV 동일). 부분 UPDATE 이득은 학습 시 교체.
        void MySqlDBProvider::SaveDelta(const DBResult& snapshot, UINT32 dirtyMask)
        {
            (void)dirtyMask;   // 1차 미사용(부분 UPDATE 최적화는 후속). 통째 위임.
            SaveFull(snapshot);
        }

        // -- 트랜잭션 (현재 호출자 없음 - 아이템 시스템 때 활성. 인터페이스 선설계) --
        // m_conn null 가드 = Connect 전 호출(UB) 방어. dtor 의 m_conn 취급과 일관.
        void MySqlDBProvider::Begin()
        {
            if (m_conn == nullptr) { return; }
            mysql_query(m_conn, "START TRANSACTION");
        }

        void MySqlDBProvider::Commit()
        {
            if (m_conn == nullptr) { return; }
            mysql_query(m_conn, "COMMIT");
        }

        void MySqlDBProvider::Rollback()
        {
            if (m_conn == nullptr) { return; }
            mysql_query(m_conn, "ROLLBACK");
        }

        // -- 인벤토리 --

        // 캐릭터의 인벤토리 전량 로드 (재시도 래퍼). 읽기 전용 + 본체가 out 을 처음부터 다시 채우므로 재실행 안전.
        bool MySqlDBProvider::LoadInventory(UINT32 charId, InventorySnapshot& out)
        {
            
            if (LoadInventoryOnce(charId, out)) { return true; }
            char errText[256];
            CopyConnError(m_conn, errText, sizeof(errText));   // ping(생사 판정)이 에러를 리셋하기 전에 원인 확보
            if (RecoverIfConnectionLost())
            {
                if (LoadInventoryOnce(charId, out)) { return true; }
                CopyConnError(m_conn, errText, sizeof(errText));   // 재실행도 실패 - 그 원인으로 갱신
            }
            InterlockedIncrement(&s_queryFailCount);
            DbLog(LogLevel::LL_ERROR, L"인벤토리 로드 실패 (char_id=%u): %S", charId, errText);
            return false;
        }

        // 캐릭터의 인벤토리 전량 로드 (1회 시도 본체). 배치(character_inventory)와 실물(item_instance)을 JOIN 해 한 번에.
        bool MySqlDBProvider::LoadInventoryOnce(UINT32 charId, InventorySnapshot& out)
        {
            out.count = 0;
            if (m_conn == nullptr) { return false; }

            MYSQL_STMT* stmt = mysql_stmt_init(m_conn);
            if (stmt == nullptr) { return false; }

            const char* sql =
                "SELECT ci.slot, ci.item_uid, ii.template_id, ii.quantity "
                "FROM character_inventory ci JOIN item_instance ii ON ci.item_uid = ii.item_uid "
                "WHERE ci.char_id = ?";
            if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(strlen(sql))) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            unsigned int inCharId = charId;
            MYSQL_BIND pbind[1];
            memset(pbind, 0, sizeof(pbind));
            pbind[0].buffer_type = MYSQL_TYPE_LONG;
            pbind[0].buffer      = &inCharId;
            pbind[0].is_unsigned = 1;
            if (mysql_stmt_bind_param(stmt, pbind) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }
            if (mysql_stmt_execute(stmt) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            // 결과 4컬럼: slot(SMALLINT) / item_uid(BIGINT) / template_id(INT) / quantity(INT)
            unsigned int       slot = 0;
            unsigned long long uid = 0;
            int                templateId = 0;
            int                quantity = 0;
            MYSQL_BIND rbind[4];
            memset(rbind, 0, sizeof(rbind));
            rbind[0].buffer_type = MYSQL_TYPE_LONG;      rbind[0].buffer = &slot;       rbind[0].is_unsigned = 1;
            rbind[1].buffer_type = MYSQL_TYPE_LONGLONG;  rbind[1].buffer = &uid;        rbind[1].is_unsigned = 1;
            rbind[2].buffer_type = MYSQL_TYPE_LONG;      rbind[2].buffer = &templateId;
            rbind[3].buffer_type = MYSQL_TYPE_LONG;      rbind[3].buffer = &quantity;
            if (mysql_stmt_bind_result(stmt, rbind) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            int fetch = 0;
            while ((fetch = mysql_stmt_fetch(stmt)) == 0)
            {
                if (out.count >= InventorySnapshot::MAX_ROWS) { break; }   // 규약보다 많은 행은 무시 (오염 방어)
                InventoryRow& row = out.rows[out.count];
                row.slot       = static_cast<BYTE>(slot);
                row.uid        = static_cast<UINT64>(uid);
                row.templateId = templateId;
                row.quantity   = quantity;
                ++out.count;
            }
            mysql_stmt_close(stmt);
            // 정상 종료 = 행 소진(MYSQL_NO_DATA) 또는 상한 도달(fetch==0 에서 break). 그 외는 backend 오류.
            return (fetch == MYSQL_NO_DATA || fetch == 0);
        }

        // 이 캐릭터의 배치 행과 그에 연결된 실물 행을 삭제 (전량 교체의 지우기 단계 - 트랜잭션 안에서만 호출).
        //   실물 행을 배치 JOIN 으로 먼저 지운다 - 배치를 먼저 지우면 실물을 찾을 방법이 없다.
        bool MySqlDBProvider::WipeInventoryRows(UINT32 charId)
        {
            static const char* const kSqls[2] =
            {
                "DELETE ii FROM item_instance ii JOIN character_inventory ci ON ii.item_uid = ci.item_uid WHERE ci.char_id = ?",
                "DELETE FROM character_inventory WHERE char_id = ?",
            };
            for (int q = 0; q < 2; ++q)
            {
                MYSQL_STMT* stmt = mysql_stmt_init(m_conn);
                if (stmt == nullptr) { return false; }
                if (mysql_stmt_prepare(stmt, kSqls[q], static_cast<unsigned long>(strlen(kSqls[q]))) != 0)
                {
                    mysql_stmt_close(stmt); return false;
                }
                unsigned int inCharId = charId;
                MYSQL_BIND b[1];
                memset(b, 0, sizeof(b));
                b[0].buffer_type = MYSQL_TYPE_LONG;
                b[0].buffer      = &inCharId;
                b[0].is_unsigned = 1;
                if (mysql_stmt_bind_param(stmt, b) != 0 || mysql_stmt_execute(stmt) != 0)
                {
                    mysql_stmt_close(stmt); return false;
                }
                mysql_stmt_close(stmt);
            }
            return true;
        }

        // 스냅샷의 행들을 실물/배치 테이블에 삽입 (전량 교체의 채우기 단계 - 트랜잭션 안에서만 호출).
        //   실물은 REPLACE - 거래에서 소유만 바뀐 uid 도 한 경로로 처리 (PK 충돌 시 삭제 후 삽입).
        bool MySqlDBProvider::InsertInventoryRows(const InventorySnapshot& snapshot)
        {
            MYSQL_STMT* itemStmt = mysql_stmt_init(m_conn);
            MYSQL_STMT* slotStmt = mysql_stmt_init(m_conn);
            if (itemStmt == nullptr || slotStmt == nullptr)
            {
                if (itemStmt != nullptr) { mysql_stmt_close(itemStmt); }
                if (slotStmt != nullptr) { mysql_stmt_close(slotStmt); }
                return false;
            }

            const char* itemSql = "REPLACE INTO item_instance (item_uid, template_id, quantity) VALUES (?, ?, ?)";
            const char* slotSql = "INSERT INTO character_inventory (char_id, slot, item_uid) VALUES (?, ?, ?)";

            bool ok = (mysql_stmt_prepare(itemStmt, itemSql, static_cast<unsigned long>(strlen(itemSql))) == 0)
                   && (mysql_stmt_prepare(slotStmt, slotSql, static_cast<unsigned long>(strlen(slotSql))) == 0);

            // 바인딩 버퍼는 루프 변수 - 한 번 bind 하고 행마다 값만 바꿔 재실행 (버퍼 주소 불변).
            unsigned long long uid = 0;
            int                templateId = 0;
            int                quantity = 0;
            unsigned int       inCharId = snapshot.charId;
            unsigned int       slot = 0;

            MYSQL_BIND ib[3];
            memset(ib, 0, sizeof(ib));
            ib[0].buffer_type = MYSQL_TYPE_LONGLONG; ib[0].buffer = &uid;        ib[0].is_unsigned = 1;
            ib[1].buffer_type = MYSQL_TYPE_LONG;     ib[1].buffer = &templateId;
            ib[2].buffer_type = MYSQL_TYPE_LONG;     ib[2].buffer = &quantity;

            MYSQL_BIND sb[3];
            memset(sb, 0, sizeof(sb));
            sb[0].buffer_type = MYSQL_TYPE_LONG;     sb[0].buffer = &inCharId;   sb[0].is_unsigned = 1;
            sb[1].buffer_type = MYSQL_TYPE_LONG;     sb[1].buffer = &slot;       sb[1].is_unsigned = 1;
            sb[2].buffer_type = MYSQL_TYPE_LONGLONG; sb[2].buffer = &uid;        sb[2].is_unsigned = 1;

            ok = ok && (mysql_stmt_bind_param(itemStmt, ib) == 0)
                    && (mysql_stmt_bind_param(slotStmt, sb) == 0);

            for (int i = 0; ok && i < snapshot.count; ++i)
            {
                const InventoryRow& row = snapshot.rows[i];
                if (row.uid == 0) { continue; }   // 빈 칸은 저장 안 함
                uid        = row.uid;
                templateId = row.templateId;
                quantity   = row.quantity;
                slot       = row.slot;
                ok = (mysql_stmt_execute(itemStmt) == 0) && (mysql_stmt_execute(slotStmt) == 0);
            }

            mysql_stmt_close(itemStmt);
            mysql_stmt_close(slotStmt);
            return ok;
        }

        // 거래 감사 행 삽입 (TradeCommitOnce 의 트랜잭션 안에서만 호출 - 실패 시 false, 호출측이 Rollback).
        //   바인딩 버퍼는 루프 변수 - 한 번 bind 하고 행마다 값만 바꿔 재실행 (InsertInventoryRows 관용구).
        //   traded_at 은 DEFAULT CURRENT_TIMESTAMP 라 바인딩하지 않는다 (DB 서버 시각 권위).
        bool MySqlDBProvider::InsertTradeAuditRows(const TradeAuditSnapshot& audit)
        {
            if (audit.count <= 0) { return true; }   // 빈 거래(양측 오퍼 0) - 기록할 것 없음

            MYSQL_STMT* stmt = mysql_stmt_init(m_conn);
            if (stmt == nullptr) { return false; }

            // ON DUPLICATE KEY UPDATE no-op: COMMIT 응답 유실(실제 커밋됨) 후 같은 거래를 재실행하면
            //   같은 (trade_uid, row_idx) 로 다시 들어온다 - 그 INSERT 를 조용히 성공 처리해 증거 중복을 막는다.
            const char* sql =
                "INSERT INTO trade_audit"
                " (trade_uid, row_idx, giver_char_id, receiver_char_id, old_item_uid, new_item_uid, template_id, quantity, prev_quantity)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
                " ON DUPLICATE KEY UPDATE audit_id = audit_id";
            if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(strlen(sql))) != 0)
            {
                mysql_stmt_close(stmt); return false;
            }

            unsigned long long tradeUid = audit.tradeUid;
            int                rowIdx = 0;
            unsigned int       giver = 0, receiver = 0;
            unsigned long long oldUid = 0, newUid = 0;
            int                templateId = 0, quantity = 0, prevQuantity = 0;

            MYSQL_BIND b[9];
            memset(b, 0, sizeof(b));
            b[0].buffer_type = MYSQL_TYPE_LONGLONG; b[0].buffer = &tradeUid;     b[0].is_unsigned = 1;
            b[1].buffer_type = MYSQL_TYPE_LONG;     b[1].buffer = &rowIdx;
            b[2].buffer_type = MYSQL_TYPE_LONG;     b[2].buffer = &giver;        b[2].is_unsigned = 1;
            b[3].buffer_type = MYSQL_TYPE_LONG;     b[3].buffer = &receiver;     b[3].is_unsigned = 1;
            b[4].buffer_type = MYSQL_TYPE_LONGLONG; b[4].buffer = &oldUid;       b[4].is_unsigned = 1;
            b[5].buffer_type = MYSQL_TYPE_LONGLONG; b[5].buffer = &newUid;       b[5].is_unsigned = 1;
            b[6].buffer_type = MYSQL_TYPE_LONG;     b[6].buffer = &templateId;
            b[7].buffer_type = MYSQL_TYPE_LONG;     b[7].buffer = &quantity;
            b[8].buffer_type = MYSQL_TYPE_LONG;     b[8].buffer = &prevQuantity;

            bool ok = (mysql_stmt_bind_param(stmt, b) == 0);
            for (int i = 0; ok && i < audit.count && i < TradeAuditSnapshot::MAX_ROWS; ++i)
            {
                const TradeAuditRecord& row = audit.rows[i];
                rowIdx       = i;
                giver        = row.giverCharId;
                receiver     = row.receiverCharId;
                oldUid       = row.oldUid;
                newUid       = row.newUid;
                templateId   = row.templateId;
                quantity     = row.quantity;
                prevQuantity = row.prevQuantity;
                ok = (mysql_stmt_execute(stmt) == 0);
            }
            mysql_stmt_close(stmt);
            return ok;
        }

        // 인벤토리 전량 교체 저장 (재시도 래퍼). 실패가 연결 끊김이면 재접속 후 트랜잭션 전체를 1회 재실행한다 -
        //   재접속 = 서버측 전체 롤백이라 반쪽이 없고, 전량 교체(지우고 다시 쓰기)라 COMMIT 응답 유실 뒤의
        //   재실행도 같은 결과다(멱등). 최종 실패 = DB 는 저장 전 상태(인메모리가 권위)이고 커맨드가 dirty 를
        //   재표시해 다음 주기가 따라잡는다. leave/종료 저장(플레이어 부재 - dirty 재표시 불가)은 이 재실행이 마지막 방어.
        bool MySqlDBProvider::SaveInventory(const InventorySnapshot& snapshot)
        {
            if (SaveInventoryOnce(snapshot)) { return true; }
            if (RecoverIfConnectionLost() && SaveInventoryOnce(snapshot)) { return true; }
            InterlockedIncrement(&s_queryFailCount);
            return false;   // 커밋 실패 - 커맨드가 dirty clear 콜백을 발행하지 않아 dirty 유지로 다음 주기 재저장
        }

        // 인벤토리 전량 교체 저장 (1회 시도 본체) - 한 트랜잭션으로 지우고 채운다.
        //   중간에 서버가 죽어도 DB 는 "저장 전 전체" 아니면 "저장 후 전체" - 슬롯 절반만 남는 상태가 없다.
        bool MySqlDBProvider::SaveInventoryOnce(const InventorySnapshot& snapshot)
        {
            if (m_conn == nullptr) { return false; }

#ifdef _DEBUG
            // H4 (검증 하니스): 무장돼 있으면 이번 저장을 커밋 없이 강제 실패시킨다(재현). 그 char 를 기억해 두고,
            //   이후 같은 char 가 다시 저장되면 재저장 관측(dirty 유지 = 수정 작동). 수정 전엔 재저장이 오지 않는다.
            if (::InterlockedExchange(&g_h4FaultArm, 0) == 1)
            {
                ::InterlockedExchange(&g_h4FaultedCharId, static_cast<LONG>(snapshot.charId));
                ::InterlockedIncrement(&g_h4FaultInjected);
                DbLog(LogLevel::LL_WARN, L"[H4] 인벤 저장 강제 실패 1회 (char_id=%u) - 재현", snapshot.charId);
                return false;
            }
            if (g_h4FaultedCharId != 0 && static_cast<LONG>(snapshot.charId) == g_h4FaultedCharId)
            {
                ::InterlockedExchange(&g_h4FaultedCharId, 0);   // 1회만 관측/로그 (재-fault 무장 전까지)
                ::InterlockedIncrement(&g_h4ResaveObserved);
                DbLog(LogLevel::LL_WARN, L"[H4] 강제 실패했던 char_id=%u 재저장 관측 - dirty 유지(fix 작동)", snapshot.charId);
            }
#endif

            Begin();
            if (mysql_errno(m_conn) != 0)   // START TRANSACTION 자체가 실패하면 autocommit 낱개 반영 위험 - 진입 중단
            {
                DbLog(LogLevel::LL_ERROR, L"인벤토리 저장 - 트랜잭션 시작 실패 (char_id=%u): %S",
                          snapshot.charId, mysql_error(m_conn));
                return false;
            }

            if (WipeInventoryRows(snapshot.charId) && InsertInventoryRows(snapshot))
            {
                Commit();
                if (mysql_errno(m_conn) != 0)   // COMMIT 실패 = 미확정 (연결 단절 등) - 실패로 보고 (TradeCommit 과 대칭)
                {
                    DbLog(LogLevel::LL_ERROR, L"인벤토리 저장 - COMMIT 실패 (char_id=%u): %S",
                              snapshot.charId, mysql_error(m_conn));
                    return false;
                }
                return true;
            }
            char errText[256];
            CopyConnError(m_conn, errText, sizeof(errText));   // ROLLBACK 성공이 에러를 리셋하기 전에 원인 확보
            Rollback();   // DB 는 저장 전 상태 그대로 - 인메모리가 권위라 다음 저장이 따라잡는다
            DbLog(LogLevel::LL_ERROR, L"인벤토리 저장 실패 - 롤백 (char_id=%u): %S", snapshot.charId, errText);
            return false;
        }

        // 거래 커밋 (재시도 래퍼). 실패가 연결 끊김이면 재접속 후 트랜잭션 전체를 1회 재실행한다 -
        //   양측 인벤은 전량 교체라 멱등이고, 감사 행은 (trade_uid,row_idx) UNIQUE 가 모호 COMMIT(응답 유실)
        //   재실행의 중복 INSERT 를 걸러낸다. 최종 실패 시 호출측(TradeCommitCommand)이 양측 dirty 를 재표시해
        //   다음 저장이 인벤을 따라잡는다(단 그 경로로 영속된 거래엔 감사 행이 없다: 커밋 실패 로그가 보조 단서).
        bool MySqlDBProvider::TradeCommit(const InventorySnapshot& a, const InventorySnapshot& b, const TradeAuditSnapshot& audit)
        {
            if (TradeCommitOnce(a, b, audit)) { return true; }
            if (RecoverIfConnectionLost() && TradeCommitOnce(a, b, audit)) { return true; }
            InterlockedIncrement(&s_queryFailCount);
            return false;
        }

        // 거래 커밋 (1회 시도 본체) - 양측 인벤토리 전량 교체 + 거래 감사 행을 단일 트랜잭션으로.
        //   지우기를 양측 먼저 전부 끝내고 삽입한다 - 수령분에는 매번 새 uid 를 발급하므로 지금은 충돌이 날 수
        //   없지만, 이 순서를 고정해 두면 "한 uid 는 어느 순간에도 한 캐릭터에만" 이라는 PRIMARY KEY(item_uid)
        //   불변식이 uid 발급 방식과 무관하게 트랜잭션 중간 상태에서도 유지된다(선제 방어).
        //   커밋 전 어디서 죽어도 DB 는 거래 전 상태로 온전(반쪽 없음).
        //   감사 행이 같은 트랜잭션이라 "거래 결과는 있는데 증거가 없다"도 구조적으로 불가.
        bool MySqlDBProvider::TradeCommitOnce(const InventorySnapshot& a, const InventorySnapshot& b, const TradeAuditSnapshot& audit)
        {
            if (m_conn == nullptr) { return false; }

            Begin();
            if (mysql_errno(m_conn) != 0)
            {
                DbLog(LogLevel::LL_ERROR, L"거래 커밋 - 트랜잭션 시작 실패: %S", mysql_error(m_conn));
                return false;
            }

            const bool ok = WipeInventoryRows(a.charId)
                         && WipeInventoryRows(b.charId)
                         && InsertInventoryRows(a)
                         && InsertInventoryRows(b)
                         && InsertTradeAuditRows(audit);
            if (ok)
            {
                Commit();
                if (mysql_errno(m_conn) != 0)   // COMMIT 실패 = 미확정 (연결 단절 등) - 실패로 보고
                {
                    DbLog(LogLevel::LL_ERROR, L"거래 커밋 - COMMIT 실패: %S", mysql_error(m_conn));
                    return false;
                }
                return true;
            }
            char errText[256];
            CopyConnError(m_conn, errText, sizeof(errText));   // ROLLBACK 성공이 에러를 리셋하기 전에 원인 확보
            Rollback();
            DbLog(LogLevel::LL_ERROR, L"거래 커밋 실패 - 롤백 (char %u <-> %u): %S", a.charId, b.charId, errText);
            return false;
        }

        // 부팅 1회 - 보존 기간(AUDIT_HOT_DAYS)이 지난 거래 감사 행을 hot(trade_audit)에서 cold(trade_audit_archive)로 이관.
        //   기준 시각을 세션 변수(@audit_cutoff)로 한 번만 확정한다 - 이관 SELECT 와 삭제 DELETE 가 각자
        //   NOW() 를 평가하면 두 시각 사이 경계에 걸친 행이 "복사 안 되고 삭제만" 될 수 있기 때문.
        //   이관+삭제를 한 트랜잭션으로 묶어 "양쪽에 있거나 hot 에만 있거나"만 가능(유실/중복 없음).
        //   실패는 비치명 - hot 에 그대로 남고 다음 부팅이 재시도. 평문 SQL(사용자 입력 없음).
        void MySqlDBProvider::ArchiveOldTradeAudit()
        {
            if (m_conn == nullptr) { return; }

            char cutoffSql[128];
            sprintf_s(cutoffSql, sizeof(cutoffSql),
                      "SET @audit_cutoff = NOW() - INTERVAL %d DAY", AUDIT_HOT_DAYS);
            static const char* const kMoveSql =
                "INSERT INTO trade_audit_archive"
                " (audit_id, trade_uid, row_idx, traded_at, giver_char_id, receiver_char_id,"
                "  old_item_uid, new_item_uid, template_id, quantity, prev_quantity)"
                " SELECT audit_id, trade_uid, row_idx, traded_at, giver_char_id, receiver_char_id,"
                " old_item_uid, new_item_uid, template_id, quantity, prev_quantity"
                " FROM trade_audit WHERE traded_at < @audit_cutoff";
            static const char* const kDeleteSql =
                "DELETE FROM trade_audit WHERE traded_at < @audit_cutoff";

            Begin();
            if (mysql_errno(m_conn) != 0)
            {
                DbLog(LogLevel::LL_ERROR, L"감사 이관 - 트랜잭션 시작 실패 (다음 부팅 재시도): %S", mysql_error(m_conn));
                return;
            }
            if (mysql_query(m_conn, cutoffSql) != 0
                || mysql_query(m_conn, kMoveSql) != 0
                || mysql_query(m_conn, kDeleteSql) != 0)
            {
                char errText[256];
                CopyConnError(m_conn, errText, sizeof(errText));   // ROLLBACK 성공이 에러를 리셋하기 전에 원인 확보
                Rollback();
                DbLog(LogLevel::LL_ERROR, L"감사 이관 실패 - 롤백 (다음 부팅 재시도): %S", errText);
                return;
            }
            const unsigned long long moved = mysql_affected_rows(m_conn);   // 마지막 문장(DELETE) 행 수 = 이관 행 수
            Commit();
            if (mysql_errno(m_conn) != 0)
            {
                DbLog(LogLevel::LL_ERROR, L"감사 이관 - COMMIT 실패 (다음 부팅 재시도): %S", mysql_error(m_conn));
                return;
            }
            if (moved > 0)
            {
                DbLog(LogLevel::LL_INFO, L"거래 감사 이관: %llu행 (hot %d일 초과 -> archive)", moved, AUDIT_HOT_DAYS);
            }
        }

        // 부팅 1회 - 발급된 최대 item_uid 조회 (인메모리 발급기 시드. 실패 시 부팅 중단해야 uid 충돌이 없다).
        //   살아있는 실물(item_instance)만 보면 안 된다 - 거래 감사(trade_audit/archive)의 old/new uid 는
        //   실물이 소비돼 사라진 뒤에도 남는 "역사의 키"라, 최고 uid 실물이 소비된 채 재부팅하면
        //   같은 uid 가 재발급돼 무관한 두 계보가 한 uid 를 공유하게 된다(증거 식별자 충돌).
        //   trade_uid 도 같은 발급기에서 나오는 역사의 키(감사 행에만 남음 - 전량 스택 합류 거래는 새 실물 행이
        //   없어 실물만 보면 시드가 되감긴다)라 함께 시드한다. 그래서 실물 + 감사 uid 컬럼 전부의 최댓값.
        bool MySqlDBProvider::LoadMaxItemUid(UINT64& outMaxUid)
        {
            outMaxUid = 0;
            if (m_conn == nullptr) { return false; }
            static const char* const kMaxUidSql =
                "SELECT GREATEST("
                " COALESCE((SELECT MAX(item_uid) FROM item_instance), 0),"
                " COALESCE((SELECT MAX(old_item_uid) FROM trade_audit), 0),"
                " COALESCE((SELECT MAX(new_item_uid) FROM trade_audit), 0),"
                " COALESCE((SELECT MAX(trade_uid) FROM trade_audit), 0),"
                " COALESCE((SELECT MAX(old_item_uid) FROM trade_audit_archive), 0),"
                " COALESCE((SELECT MAX(new_item_uid) FROM trade_audit_archive), 0),"
                " COALESCE((SELECT MAX(trade_uid) FROM trade_audit_archive), 0))";
            if (mysql_query(m_conn, kMaxUidSql) != 0) { return false; }
            MYSQL_RES* res = mysql_store_result(m_conn);
            if (res == nullptr) { return false; }
            bool ok = false;
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row != nullptr && row[0] != nullptr)
            {
                outMaxUid = static_cast<UINT64>(_strtoui64(row[0], nullptr, 10));
                ok = true;
            }
            mysql_free_result(res);
            return ok;
        }
    }
}
