-- Data/seed_bots.sql
-- 로드테스트 전용 봇 계정 시드 (DummyClient 의 bot00000.. 계정과 정합).
--
-- 무엇인가:
--   accounts 테이블에 bot00000 ~ bot05999 (6000개) 계정을 넣는다.
--   pw_hash = '$test$' 는 PasswordHasher::Verify 가 prefix dispatch 로 즉시 통과시키는
--   로드테스트 전용 스킴이다 (PBKDF2 ~200ms 해시를 우회 -> 6000봇 빠른 램프업).
--   클라 입력(CS_REGISTER)으로는 '$test$' 가 절대 생성되지 않는다(항상 '$pbkdf2$'). bot* 이름은 로드테스트 예약.
--
-- 이 파일은 '데이터 전용'이다 - accounts 스키마(CREATE TABLE)는 LoginServer 부팅의
--   EnsureAccountsSchema 가 단일 소유한다(스키마 DDL 이중화 회피·SSOT). 그래서 테이블이 먼저 있어야 한다.
--
-- 절차 (왜 이 순서인가):
--   LoginServer 는 부팅 1회 LoadFromDb 로 accounts 를 in-memory 맵(m_accounts)에 적재하고
--   로그인 검증은 그 맵을 조회한다(라이브 MySQL 질의가 아님). 따라서 시드는 'LoadFromDb 전'에 DB 에 있어야 한다.
--     1) LoginServer 1회 부팅       -> EnsureAccountsSchema 가 accounts 테이블 생성
--     2) LoginServer 종료
--     3) mysql -u kys_app -p kys_game < Data/seed_bots.sql   -> 봇 6000행 적재(아래 INSERT)
--     4) LoginServer 재부팅          -> LoadFromDb 가 봇 accounts 를 m_accounts 에 적재(이때부터 봇 로그인 가능)
--     5) DummyClient 로드테스트 실행
--   ServerApp 의 characters 는 per-login 라이브 질의라 재부팅 불요 - 재적재가 필요한 건 LoginServer accounts 한정.
--
-- 멱등: ON DUPLICATE KEY UPDATE 라 여러 번 실행해도 안전(옛 행의 pw_hash 도 '$test$' 로 교정).

USE kys_game;

-- 재귀 CTE 기본 깊이 상한(1000)을 봇 수 이상으로 상향(6000행 생성).
SET SESSION cte_max_recursion_depth = 1000000;

-- 봇 6000개 = MAX_CCU 커버. 숫자는 DummySession 의 bot%05d 및 LoginServer 의 옛 BOT_SEED_COUNT 와 정합.
INSERT INTO accounts (login_name, pw_hash)
WITH RECURSIVE seq(n) AS (
    SELECT 0
    UNION ALL
    SELECT n + 1 FROM seq WHERE n + 1 < 6000
)
SELECT CONCAT('bot', LPAD(n, 5, '0')), '$test$' FROM seq
ON DUPLICATE KEY UPDATE pw_hash = '$test$';
