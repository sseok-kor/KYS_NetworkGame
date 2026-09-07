#pragma once

// ===== 메모리 풀 용량 / 동시접속 상한 =====
static const size_t DEFAULT_PLAYER_POOL_CAPACITY  = 7000;    // Player 풀이 미리 잡아두는 슬롯 수 (MAX_CCU보다 크게)
static const size_t DEFAULT_MONSTER_POOL_CAPACITY = 10000;   // Monster 풀이 미리 잡아두는 슬롯 수
static const int    MAX_CCU = 6000;                          // 동시접속 최대 인원, 넘으면 새 로그인 거부 (풀보다 작게). 5500봇 스케일 측정 + 천장 초과 관찰용 상향
static const int    MAX_CHANNEL_COUNT       = 16;            // 채널 배열 컴파일타임 상한 (실제 채널 수=main.cpp CHANNEL_COUNT) - 채널별 인구 카운터 배열 크기용
static const int    MAX_PLAYERS_PER_CHANNEL = 1000;         // 게임 채널 1개가 받는 최대 플레이어 수 (넘으면 다음 채널, 전부 차면 SERVER_FULL 거부). 측정 후 조정
static const BYTE   CHANNEL_ID_UNSELECTED   = 0xFF;         // "아직 채널 미선택" sentinel (유효범위 0..MAX_CHANNEL_COUNT-1 밖) - pending 기본값. 채널 선택 없이 캐릭터 선택 직송 시 IsChannelJoinable이 거부(ch0 무단 우회 차단)

// ===== 에러 코드 =====
enum class GameErrorCode : int
{
    PLAYER_NOT_FOUND  = -200,    // 해당 플레이어 못 찾음
    MONSTER_NOT_FOUND = -201,    // 해당 몬스터 못 찾음
};

// ===== 맵 크기 / 맵 개수 =====
//   맵 크기는 맵마다 다르다 (Data/maps.csv width/height - 좌표 유효 범위는 [0, width]x[0, height]).
//   실제 크기 조회는 MapWidthFor/MapHeightFor(MapTable.h) - 아래 상수는 "상한/하한"이지 실크기가 아니다.
static const int MAX_MAP_SIZE_PX  = 4000;   // 맵 한 변 최대 px (배열 차원·격자 칸 수의 컴파일타임 상한. 통행 격자/AOI 격자가 이 값 기준)
static const int MIN_MAP_SIZE_PX  = 1000;   // 맵 한 변 최소 px (로더 검증 하한 - 시야 400 대비 최소 활동 공간)
static const int MAP_SIZE_STEP_PX = 250;    // 맵 크기 단위 px (로더가 이 배수만 허용 - AOI 셀 250/통행 셀 50 을 정수 분할)
static const int MAX_MAP_COUNT = 16;  // 맵 배열 컴파일타임 상한 (실제 맵 수 = Data/maps.csv 행 수 = MapTableCount()) - MAX_CHANNEL_COUNT와 같은 "고정 상한 + 런타임 수" 규약
static const int MAP_NAME_MAX  = 16;  // 맵 표시 이름 최대 글자 수 (wchar, 15자+널)
static const int MAP_FILE_MAX  = 32;  // 맵 부속 파일명(walkable 격자 등) 최대 글자 수

// ===== 통행 격자 (walkable - 맵 내부의 벽/장애물. 이동/스폰/포탈 좌표가 밟을 수 있는 칸인지 검사) =====
//   AOI 격자(CELL_SIZE 250, 아래)와는 별개 시스템 - 해상도만 다른 독립 격자 (250 = 50 x 5 정수배).
//   비트 격자: 칸 하나 = 1비트 ('1'=통행 / '0'=벽). 행 바이트 수는 폭과 무관하게 상한 고정(stride) -
//   파일과 메모리가 같은 공식(row * WALK_ROW_STRIDE_BYTES + col / 8)을 쓰게 해 인덱스 산술 불일치를 차단.
static const int WALK_CELL_SIZE        = 50;    // 통행 격자 한 칸 한 변 px
static const int WALK_CELLS_MAX        = 80;    // 한 변 최대 칸 수 (= MAX_MAP_SIZE_PX / WALK_CELL_SIZE)
static const int WALK_ROW_STRIDE_BYTES = 10;    // 한 행의 바이트 수 (= WALK_CELLS_MAX / 8. 폭이 좁아도 고정 - 산술 단일화)
static const int WALK_BYTES_PER_MAP    = 800;   // 맵 하나의 통행 비트 총량 (= WALK_CELLS_MAX x WALK_ROW_STRIDE_BYTES)
static const int WALK_SPOT_RETRY_MAX   = 10;    // 랜덤 지점(몬스터 스폰/배회 목적지)이 벽이면 다시 뽑는 최대 횟수 - 무한 재추첨 금지(전부 실패 = 그 tick 포기)

// ===== 격자 (가까운 객체만 추리려고 맵을 칸으로 나눔) =====
//   grid(맵 전체 격자) > cell(한 칸). 셀 크기는 공간 색인 해상도일 뿐 시야와 분리(시야=VIEW_RANGE).
//   셀이 시야보다 작아야(cell < view) 밀집 맵에서도 근처만 정확히 추려진다.
static const int CELL_SIZE = 250;     // 격자 한 칸 한 변 px (공간 색인 해상도, 시야와 별개)
static const int GRID_COLS = 16;      // 격자 가로 칸 수 상한 (= MAX_MAP_SIZE_PX / CELL_SIZE. 작은 맵은 위쪽 칸이 빈 채 남음 - 무해)
static const int GRID_ROWS = 16;      // 격자 세로 칸 수 상한 (위와 동일)
static const int VIEW_RANGE = 400;    // 플레이어 시야 반경 px (서로 보이는 거리 = broadcast 대상 범위)

// ===== 이동 / 위치 동기화 =====
static const int MAX_MOVE_SPEED      = 200;   // 이동 속도 px/s (서버와 클라가 같은 값으로 다음 위치를 예측)
static const int MOVE_SYNC_TOLERANCE = 64;    // 클라 보고 위치와 서버 예측 위치 차이 허용치 px (넘으면 서버 위치로 되돌림)

// ===== 플레이어 사망 / 부활 =====
static const int   PLAYER_DEFAULT_MAX_HP    = 100;    // 새 캐릭터 시작 최대 HP
static const float PLAYER_RESPAWN_DELAY_SEC = 3.0f;   // 죽은 뒤 다시 살아나기까지 시간 (초)
static const float PLAYER_INVULN_SEC        = 1.0f;   // 부활 직후 무적 시간 (초)

// ===== 몬스터 AI / 재등장 (종류별로 다른 능력치는 ServerApp MonsterTemplate, 여긴 종류 무관 공통값) =====
static const int    MONSTER_LEASH_RANGE        = 1200;  // 집에서 이만큼 멀어지면 추격 포기하고 돌아감 (px)
static const float  MONSTER_THINK_INTERVAL     = 0.3f;  // 적과 목표를 다시 살피는 간격 (초)
static const int    MONSTER_PATROL_RADIUS      = 400;   // 평소 어슬렁대는 반경 (집 기준 px)
static const float  MONSTER_DEATH_GRACE_SEC    = 1.0f;  // 죽은 뒤 시신이 남아있는 시간 (초, 그 후 사라짐 - 회수 시점에 그 몹의 부활 예약이 걸림)
static const int    MAX_GROUP_COUNT            = 32;    // 스폰 그룹 하나가 유지할 수 있는 최대 마릿수 (부활 예약 슬롯 배열 크기 - 예약 수 <= count <= 이 값이라 넘칠 수 없음)
static const int    MAX_SPAWN_GROUPS           = 256;   // 스폰 그룹 수 컴파일타임 상한 (spawns.csv 행 수 상한 = 로더/MapManager 배열 크기 공유)

// ===== 몬스터 길찾기 (Phase D - 벽에 막히면 우회 경로 요청 -> 추종. 숫자는 측정 후 조정 대상) =====
static const float  MONSTER_PATH_PENDING_TTL_SEC = 2.0f;  // 경로 요청 후 결과가 이 시간 안 오면 pending 강제 해제 (결과 드랍 유실 자기치유)
static const int    MONSTER_PATH_REPATH_DRIFT    = 100;   // 타깃이 경로 목적지에서 이만큼(px·2셀) 벗어나면 경로 재요청
static const float  MONSTER_PATH_RETRY_SEC       = 1.0f;  // NOPATH(경로 없음) 수신 후 재요청까지 대기 (초)
static const float  MONSTER_PATH_GIVEUP_SEC      = 5.0f;  // 이 시간 연속 경로 실패면 추격 포기하고 집으로 (leash 복귀 재사용)

// ===== 스킬 / 전투 =====
static const int   SKILL_HIT_RANGE    = 120;    // 스킬이 닿는 반경 px
static const float SKILL_COOLDOWN_SEC = 0.5f;   // 모든 스킬 공통 재사용 대기 시간 (초)
static const int   PLAYER_BASE_ATTACK_POWER = 50;   // 플레이어 맨손 기준 공격력 (최종 공격력 = 이 값 + 장착 무기)

// ===== DB 저장 =====
static const int    DB_SAVE_INTERVAL  = 60;     // 자동 저장 간격 (초) - 손실 윈도우 축소(300->60). 중요 이벤트(맵 이동)는 즉시 저장.
static const UINT32 DB_DIRTY_POSITION  = 1;     // 저장 때 좌표가 바뀌었음 표시
static const UINT32 DB_DIRTY_STATS     = 2;     // 저장 때 hp/maxHp/mp가 바뀌었음 표시
static const UINT32 DB_DIRTY_INVENTORY = 4;     // 저장 때 인벤토리(가방/장착)가 바뀌었음 표시

// ===== 채팅 =====
static const int WHISPER_NAME_MAX     = 16;     // 캐릭터 이름이나 귓속말 대상 이름 최대 글자 수
static const int CHAT_MSG_MAX         = 80;     // 채팅 한 줄 최대 글자 수
static const int CHAT_MIN_INTERVAL_MS = 500;    // 채팅 사이 최소 간격 ms (도배 막기)
static const int PORTAL_MIN_INTERVAL_MS         = 300;    // 포탈 통과 사이 최소 간격 ms (맵이동 + 즉시 DB저장 도배 차단)
static const int CHANNEL_CHANGE_MIN_INTERVAL_MS = 1000;   // 채널 변경 사이 최소 간격 ms (비싼 in-place handoff 도배 차단)
static const int CHANNEL_LIST_MIN_INTERVAL_MS   = 500;    // 채널 목록 재조회 사이 최소 간격 ms (패킷 빌드+Send 도배 차단)

// 클라 keepalive (CS_PING) 송신 주기 ms. 서버 무음 소켓 reaper 의 NO_RECV_TIMEOUT(게임 35s)보다 충분히 짧아야
//   활동 없는 정상 연결의 m_lastRecvTick 이 CS_PING 수신으로 갱신돼 오-회수(idle RST)되지 않는다 (35s / 10s = 3회 여유). 늘리면 서버 데드라인도 함께 재검토.
static const int KEEPALIVE_INTERVAL_MS = 10000;

// per-IP 연결 레이트 (게임 서버 accept 훅) - 외부 IP 연결 폭주/DDoS 거부. loopback(로컬 봇/테스트)은 면제.
static const UINT64       CONNECT_RATE_WINDOW_MS         = 10000;   // per-IP 연결 레이트 측정 창 ms
static const UINT32       MAX_CONNECTS_PER_IP_PER_WINDOW = 30;      // 창 안에서 한 외부 IP가 여는 최대 연결 수 (초과=거부)
static const unsigned int CONNECT_IP_TABLE_SOFT_CAP      = 4096;    // IP 창 맵 정리 트리거 크기 (스푸핑 다수 IP 메모리 상한)

// ===== 계정 / 로그인 =====
static const int PASSWORD_MAX = 32;   // 비밀번호 평문 최대 글자 수 (고정 배열 wchar_t[32]=31자+널). 입력 cap 겸용(긴 입력 PBKDF2 DoS 차단). 서버가 받아 PBKDF2 해시

// pre-auth flood 방어 (게임 서버측 - 로그인 서버 AllowLoginAttempt 미러. 계정 잠금 없음[lockout DoS 회피], 연결/큐 단위로만)
//   per-connection/per-pending 게이트라 봇마다 자기 sid(1회 인증)라 loopback 충돌이 없다 -> 1000봇 부하테스트는 넉넉한 상수 크기로 자연 보존(AllowLoginAttempt도 loopback 면제 없이 cap 10)
static const UINT32 MAX_PREAUTH_ATTEMPTS_PER_CONN  = 10;      // 한 연결의 CS_GAME_AUTH(토큰 검증) 시도 상한 - 초과 시 인터서버 verify enqueue 스킵(증폭 차단). 정상은 1회 성공
static const UINT64 PREAUTH_ACTION_MIN_INTERVAL_MS = 200;     // pending 상태 채널선택/캐릭터생성/삭제 사이 최소 간격 ms - 선택 화면 도배 차단. 정상 클릭/봇 왕복 간격보다 짧음 (UINT64: now-last 델타와 직접 비교, 부호혼합 경고 회피)
static const size_t MAX_LINK_QUEUE_DEPTH           = 16384;   // LoginLinkThread 요청 큐(m_requests) 최대 적재 - 초과분 유계 drop(flood 방어). 정상 피크(1000봇 동시접속)보다 충분히 큼

// ===== 캐릭터 =====
//   캐릭터 이름 길이는 WHISPER_NAME_MAX(16) 재사용 (char_name VARCHAR(16) 정합, DBResult.name 과 동일 상수).
static const int MAX_CHAR_SLOTS = 3;   // 계정당 캐릭터 슬롯 수 (slot_id 0..2). MapleStory 정합. 목록/생성/삭제 cap

// ===== 로그인 서버 분리 (2-프로세스 핸드오프) =====
static const int    MAX_SERVER_LIST    = 8;       // 로그인 서버가 클라에 보내는 게임 서버 목록 최대 개수 (고정 배열)
static const UINT64 LOGIN_TOKEN_TTL_MS = 60000;   // 발급된 일회용 토큰 유효 시간 ms (이 안에 게임 서버에 토큰 제출 안 하면 만료)

// ===== 로그인 서버 분리 - 인터서버 링크 (LoginServer <-> ServerApp 전용, 클라엔 안 감) =====
//   LoginServer 는 포트 2개를 듣는다: 클라 대면(7001) + 인터서버 전용(아래). 인터서버 포트는 게임 서버만 접속하므로
//   loopback/LAN 바인딩 + 방화벽으로 외부 클라가 IS_* 핸들러에 닿지 못하게 막는다(신뢰 경계를 포트로 물리화).
static const USHORT LOGIN_CLIENT_PORT      = 7001;        // LoginServer 가 클라(GameClient/DummyClient) 대면으로 여는 포트
static const USHORT LOGIN_INTERSERVER_PORT = 7002;        // LoginServer 가 게임 서버 전용으로 여는 2번째 포트 (클라 포트와 분리)
// 인터서버 링크 공유 비밀은 하드코딩 상수에서 제거 - 두 프로세스가 conf/Server_Config.ini 의 [interserver] secret 으로 외부화(가짜 게임 서버 등록 차단, 부팅 시 non-zero 검증)

// online ghost 수렴 (게임->로그인 권위 스냅샷 + grace sweep). 게임 서버가 ONLINE_SYNC_INTERVAL_MS 마다 admitted+pending 계정 집합을 push,
//   로그인 서버는 받은 계정의 lastSyncMs 를 갱신하고 ONLINE_GRACE_MS 넘게 미갱신된 online 엔트리(ghost)를 sweep 한다.
static const UINT64 ONLINE_SYNC_INTERVAL_MS = 5000;    // 권위 online 스냅샷 주기 ms (ghost 수명 상한 = 약 1 interval)
static const UINT64 ONLINE_GRACE_MS         = 15000;   // online 엔트리 미갱신 만료 유예 ms (>= interval + jitter. 단일 누락 스냅샷이 산 계정을 안 죽이게)

// 로그인 중 DB 캐릭터 로드가 실패/지연돼 admission 됐는데 Player 미부착(player-less)으로 고착한 좀비를, admission 후 이 시간 넘으면 reaper 가 회수.
//   wall-clock(GetTickCount64) 기준이라 CS_PING heartbeat 가 못 갱신(activity 무관). 정상 admission(<1s) >> 이 값이라 윈도우 보호. rAthena 30s/60s, HeavenMS 30s 2선 방어 정합.
static const UINT64 LOADING_DEADLINE_MS     = 30000;

// ===== 아이템 / 인벤토리 =====
static const int MAX_INVENTORY_SLOTS = 24;    // 가방 슬롯 수 (MapleStory v83 탭당 24, 클라 4x6 그리드)
static const int ITEM_NAME_MAX       = 16;    // 아이템 표시 이름 최대 글자 수 (wchar, 15자+널)
static const int EQUIP_SLOT_WEAPON   = 100;   // 장착 무기 슬롯 번호 (DB slot 컬럼 규약 - 가방 0..23 과 겹치지 않게)
static const int EQUIP_SLOT_ARMOR    = 101;   // 장착 방어구 슬롯 번호

// ===== 거래 =====
static const int MAX_TRADE_SLOTS = 9;         // 거래창 한쪽이 올릴 수 있는 아이템 칸 수 (MapleStory 거래창)

// ===== 바닥 아이템 (드랍) =====
//   운영값 (e2e 검증 완료 후 전환 - 테스트 단계엔 10초/5개로 짧게 잡아 despawn/상한을 빨리 관찰했음).
static const int    GROUND_ITEM_EXPIRE_MS = 60000;   // 바닥 아이템이 사라지기까지 시간 ms (맵 tick 스윕이 회수)
static const int    GROUND_ITEM_MAP_CAP   = 100;     // 맵당 바닥 아이템 최대 개수 (초과 시 가장 오래된 것부터 제거 - 버리기 도배 방어)
static const int    GROUND_ITEM_OWNER1_MS = 3000;    // 드랍 후 이 시간까지는 기여 1순위만 주울 수 있음 (rAthena 계단식)
static const int    GROUND_ITEM_OWNER2_MS = 5000;    // 이 시간까지는 기여 1~2순위까지 허용
static const int    GROUND_ITEM_OWNER3_MS = 7000;    // 이 시간까지는 기여 1~3순위까지 허용, 이후 누구나
static const int    PICKUP_RANGE          = 120;     // 줍기 허용 거리 px (서버 검증 - 멀리서 줍기 차단)
static const size_t DEFAULT_GROUND_ITEM_POOL_CAPACITY = 2000;   // GroundItem 풀 슬롯 수 (맵 상한 MAX_MAP_COUNT x 맵당 cap 100 = 1600 + 여유)
