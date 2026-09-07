#pragma once
#include <WinSock2.h>                              // SOCKET / WSABUF / OVERLAPPED / SRWLOCK
#include "../../GameServer/Types/Defines.h"        // BYTE / RECV_BUFFER_SIZE / MAX_PACKET_SIZE
#include "../../GameCommon/GameTypes.h"            // EMoveDirection / PlayerId (봇 이동/식별 멤버)
#include "../../GameCommon/Protocol/CPacket.h"              // m_parsePkt (수신 재사용 파싱 버퍼 - per-packet 할당 회피)
#include "../../GameCommon/Protocol/PacketObfuscator.h"     // ObfKeyState 멤버 + InitObfKey/ObfuscateOpcode/DeobfuscateOpcode (opcode 난독화 방향별 rolling 키)

// 봇 주기 이동 보고 간격(ms). 서버 30Hz tick(33.3ms)보다 길게 - 한 보고당 서버 ~5tick 외삽.
//   봇별 위상 offset(AssociateAndStart에서 시드)으로 송신 시점을 분산 -> 전 봇 동시 송신(thundering herd) 회피.
static const int BOT_MOVE_INTERVAL_MS = 167;
// TimerProc 고해상도 sweep 간격(ms). 봇별 nextActionMs(위상 분산)를 16ms 해상도로 체크해 송신을 흩는다.
static const int BOT_SWEEP_INTERVAL_MS = 16;

// 봇 keepalive CS_PING 주기 = GameCommon KEEPALIVE_INTERVAL_MS(SSOT, 실클라 GameClient 와 동일 10s).
//   봇은 CS_PING 이 없어 부하 정점에 idle-kick 당하던 것을 이 keepalive 로 해소 - 서버 무음 소켓 reaper 데드라인보다 짧게 유지해 조용한 인게임 구간에도 연결을 살린다.

// reconnect churn (슬롯 재사용 재현, 측정 파라미터). 봇이 LIFETIME 후 자발 Close -> DELAY 대기 -> 재연결.
//   on/off 는 입력 구동(StressTestManager::SetTestMode -> TickReconnect 의 churnEnabled 인자) - 재빌드 없이 토글.
static const int RECONNECT_LIFETIME_TICKS = 50;   // 50*100ms = 5s 연결 수명 (churn ON 일 때만)
static const int RECONNECT_DELAY_TICKS    = 10;   // 10*100ms = 1s Close 후 재연결 대기 (churn ON 일 때만)

// hotspot 수렴 목표 좌표 (맵 중앙, 4000x4000 기준). ON/OFF 토글은 검증 하니스 HarnessConfig::hotspot(아래)로 이관.
static const int  HOTSPOT_X = 2000;
static const int  HOTSPOT_Y = 2000;
// hotspot ON 시 이동 보고 간격(ms). 평시 167ms 보다 짧게 - 고빈도 CS_MOVE 로 채널 tick shed 압력.
static const int  HOTSPOT_MOVE_INTERVAL_MS = 33;
// H12: bad-cred negative control 로 지정할 봇 슬롯 수 (앞 N개만 가짜 토큰으로 게임 서버 admission 거부 유발).
static const int  HARNESS_BADCRED_COUNT = 2;
// H1: STOP 전에 이동(START)할 워밍업 tick 수. 서버에 이동 상태(속도)를 먼저 심어야 이후 STOP shed 시 dead-reckon 표류가 성립한다
//   (idle/STOP 상태 플레이어는 서버가 외삽하지 않으므로 START 없이 STOP 만 보내면 표류가 아예 안 생겨 재현이 실패).
static const int  H1_WARMUP_MOVE_TICKS = 30;
// H1: STOP 프로브 재전송 간격(ms). 프로브 사이의 침묵이 (STOP 이 movement 레인서 shed 될 때) 서버 dead-reckon drift 를 키운다.
static const int  H1_PROBE_INTERVAL_MS = 500;
// H1: self-echo 가 의도 STOP 위치에서 이만큼(px) 넘게 벗어나면 drift 로 집계. 서버 위치 허용오차(64px)의 2배 - 명백한 표류만 센다.
static const int  H1_DRIFT_THRESHOLD = 128;
// H7: bogus 귓속말 사이클 간격(ms). 채팅 직후(짧은 간격) 귓속말로 채팅 rate-limit(500ms)을 트립한다.
static const int  H7_CYCLE_MS = 1500;

// 한 봇이 추적하는 근처 몬스터 최대 수 (SC_MONSTER_SPAWN add / SC_DEATH, SC_DESPAWN remove). 스킬 타겟 선정용.
static const int MAX_TRACKED_MONSTERS = 32;

// 한 봇이 추적하는 근처 바닥 드랍 최대 수 (SC_GROUND_ITEM_SPAWN add / SC_DESPAWN remove). 줍기 대상 선정용.
static const int MAX_TRACKED_GROUND_ITEMS = 32;

// ===== 검증 하니스 설정 (B-1) =====
// 프로세스 전역 - main 이 Run 전 1회 설정하고 이후 read-only. 전부 false = 프로덕션 부하 프로파일과 바이트 동일.
//   불변식: 각 플래그 분기는 off 면 난수(NextBounded)를 한 번도 소비하지 않는다. 그래서 off 상태의
//   봇별 LCG 스트림/결정 분포가 오늘과 동일 -> 1000봇 회귀 기준선이 보존된다(분기의 코드 위치와 무관).
struct HarnessConfig
{
    bool hotspot;    // H2: 전 봇 채널0 수렴 + 고빈도 이동 -> AOI 폭발로 과부하 shed 유발 (H1 STOP 드롭의 전제)
    bool muteIdle;   // H10: 인증/배치 후 완전 침묵(CS_PING 포함) -> 서버 무수신 sweep idle-kick 유발 (이중 leave 재현)
    bool badCred;    // H12: 앞 N개 봇이 가짜 토큰으로 게임 서버 admission 거부 유발 -> 게임 소켓 SC_LOGIN_RESULT{FAIL} (spurious arm 음성 검증)
    bool stopMove;     // H1: 도착 후 CS_MOVE{STOP} 1회 + 주기 프로브 -> STOP 이 movement 레인서 shed 되면 dead-reckon drift 를 self-echo 로 관측 (과부하 H2 와 병행)
    bool bogusWhisper; // H7: 없는 이름 귓속말 + 채팅 직후 근접 귓속말 -> 서버 무통지(거짓 성공) 재현. SC_WHISPER_FAIL 수신 카운터는 opcode 신설(B1) 후 배선
    bool skipChannel;  // H9: 재접속 시 CS_CHANNEL_SELECT 생략하고 캐시된 charId 로 CS_CHARACTER_SELECT 직송 -> 채널 미선택 admission(ch0 우회) 재현
    bool rawInject;    // H3: admission 후 raw 절단 프레임(00 03 FF) 1회 주입 -> Channel::OnRecv size<4 로 PROTOCOL_VIOLATION 종료(B0 전 FIN / B0 후 RST). flip 은 Wireshark 로 관측
    bool b3repro;      // B3: 실peer 귓속말 매 틱 + 채널홉 0.4s -> PostToChannel TOCTOU 창(서버 #ifdef _DEBUG 확대)에 이관 겹침 -> 소유권 가드 없으면 대상 keystream desync(parseErr)
    bool wallProbe;    // HW: 봇0 이 통행 격자를 무시하고 옛 수식(경계 클램프만)으로 전진 -> 벽 너머 좌표 보고 -> 서버 벽 게이트(IsPathWalkable 거부)가 wallgate 로그로 발화하는지 관측
};

// 봇 I/O 완료 종류. 워커가 GQCS 후 CONTAINING_RECORD 로 이 타입을 읽어 분기한다.
//   RECV(drain+파싱) / SEND(login 송신) / ACTION(타이머 PQCS 주기 행동 트리거) / ACTION_SEND(행동 송신 완료).
//   ACTION 과 ACTION_SEND 를 나눈 이유: 행동 사이클(트리거->결정->송신->완료)을 봇당 1개로 게이트하려고
//   ACTION_SEND 완료에서 가드를 해제한다(m_actionSendCtx overlapped 재사용을 송신 완료까지 막음 - 포화 시 안전).
enum class EBotOp : int
{
    RECV,
    SEND,
    ACTION,
    ACTION_SEND,
};

// per-IO overlapped 컨텍스트. 서버 IOCPContext 패턴 미러 - OVERLAPPED 가 첫 멤버라
//   CONTAINING_RECORD(offset 0)로 완료된 OVERLAPPED* -> BotIoContext* 로 환원한다.
struct BotIoContext
{
    OVERLAPPED m_overlapped;   // 반드시 첫 멤버 (CONTAINING_RECORD offset 0)
    EBotOp     m_op;           // RECV / SEND / ACTION / ACTION_SEND
    WSABUF     m_wsaBuf;       // WSARecv/WSASend 버퍼 기술자
};

// 봇 1개 = 1 TCP 연결 = 1 가상 플레이어. 자족 에이전트 - 소켓 per-IO 컨텍스트 I/O 를 자기가 소유.
//   부하 발생기 + e2e 검증기: 연결 -> 로그인 -> 인증/자기위치 수신 -> 주기 행동(이동/스킬/포탈/채팅/귓속말)
//   + 수신 패킷 파싱(framing 정합 + 종류별 카운트). 송신은 행동 1개/tick(m_actionPending 가드).
class DummySession
{
public:
    DummySession();
    ~DummySession();

    // 소켓 OVERLAPPED 를 보유하므로 복사 이동 전면 차단(Rule of Five 4줄).
    DummySession(const DummySession&) = delete;
    DummySession& operator=(const DummySession&) = delete;
    DummySession(DummySession&&) = delete;
    DummySession& operator=(DummySession&&) = delete;

    bool AcquireToken(const wchar_t* loginIp, unsigned short loginPort, int accountIndex, bool badCred = false);   // Stage 1: LoginServer blocking 핸드셰이크 -> m_token 획득 (로그인 계정=accountIndex, bot%05d). badCred=H12 가짜 토큰(LoginServer 건너뜀)
    bool Connect(const wchar_t* ip, unsigned short port);   // blocking connect (ramp 가 페이싱)
    bool AssociateAndStart(HANDLE iocp, int botIndex);       // Stage 2: 게임 서버 IOCP 등록 + recv 무장 + CS_GAME_AUTH(토큰) 송신 + 행동상태 시드
    void OnRecvComplete(int transferred);                    // framing 으로 경계 자르고 종류별 파싱/카운트 -> recv 재무장
    void OnSendComplete(int transferred);                    // 로그인 송신 완료 (할 일 없음)
    void Close();
    void TickReconnect(HANDLE iocp, const wchar_t* ip, unsigned short port, bool churnEnabled);   // 수명 기반 churn (TimerProc 호출, churnEnabled=입력 구동 토글)

    // 주기 행동: 타이머 스레드가 PostAction 으로 행동 틱을 IOCP에 게시 -> 워커가 OnActionTick 실행.
    void PostAction(HANDLE iocp, UINT64 nowMs, int peerBotCount);   // 타이머 호출 - 위상 게이트 + 행동 가드 + PQCS (peerBotCount=귓속말 대상 범위)
    void OnActionTick();            // 워커 호출 - 인증/배치 후 행동 결정(이동/스킬/포탈/채팅/귓속말) + 송신
    void OnActionSendComplete(int transferred);   // 워커 호출 - 행동 송신 완료 -> 행동 사이클 가드 해제

    void ArmObfuscation();   // SC_ENTER_WORLD(admission-ack 성공) 수신 직후 - m_token 으로 방향별 키 심고 무장 (서버 SeedObfKeys 와 대칭)

    SOCKET GetSocket() const { return m_sock; }

    // 수신 e2e 검증 카운터 (워커 다수 -> Interlocked). 부하기지만 "신규 wire 가 framing/필드 깨짐 없이 도달"을 확인.
    static volatile LONG64 s_recvPackets;        // 총 수신 패킷 (framing 성공 경계 수)
    static volatile LONG64 s_recvParseErrors;    // framing 이상 (길이 비정상 - wire 깨짐 신호)
    static volatile LONG64 s_recvMonsterSpawns;  // SC_MONSTER_SPAWN (monsterType 신규 wire)
    static volatile LONG64 s_recvMapChange;      // SC_MAP_CHANGE (자기 맵/위치 통지)
    static volatile LONG64 s_recvMoveBroadcast;  // SC_MOVE_BROADCAST (self-echo 포함)
    static volatile LONG64 s_recvSpawn;          // SC_SPAWN (타 플레이어 시야 등장)
    static volatile LONG64 s_recvDespawn;        // SC_DESPAWN (시야 이탈/파괴)
    static volatile LONG64 s_recvMonsterMove;    // SC_MONSTER_MOVE
    static volatile LONG64 s_recvDamage;         // SC_DAMAGE
    static volatile LONG64 s_recvDeath;          // SC_DEATH
    static volatile LONG64 s_recvRespawn;        // SC_RESPAWN
    static volatile LONG64 s_recvChat;           // SC_CHAT_BROADCAST
    static volatile LONG64 s_recvWhisper;        // SC_WHISPER
    static volatile LONG64 s_recvWhisperFail;    // SC_WHISPER_FAIL (오프라인/도배 실패 통지 - B4 H7 검증)
    static volatile LONG64 s_authSuccess;        // 인증 성공 봇 수 (SC_ENTER_WORLD 수신 = admission 성공)
    static volatile LONG64 s_authFail;           // 인증 거부 봇 수 (result==FAIL/SERVER_FULL)
    // 송신/결과 카운터
    static volatile LONG64 s_sentSkill;          // CS_SKILL 송신 수
    static volatile LONG64 s_sentPortal;         // CS_PORTAL 송신 수
    static volatile LONG64 s_sentChat;           // CS_CHAT 송신 수
    static volatile LONG64 s_sentWhisper;        // CS_WHISPER 송신 수
    static volatile LONG64 s_sentChannelChange;  // CS_CHANNEL_CHANGE 송신 수 (채널 이동 시도)
    static volatile LONG64 s_recvChannelChange;  // SC_CHANNEL_CHANGE_RESULT 수신 수 (서버 회신 - OK/거부 합산 총계)
    // 채널 변경 결과 코드별 분리 (합산 s_recvChannelChange 를 result 로 쪼갬 - 실제 이동 vs 거부 구분).
    static volatile LONG64 s_ccOk;               // result=OK (실제 채널 이동 성공)
    static volatile LONG64 s_ccSame;             // result=SAME (이미 그 채널)
    static volatile LONG64 s_ccFull;             // result=FULL (대상 채널 cap 초과)
    static volatile LONG64 s_ccInvalid;          // result=INVALID (채널 번호 범위 밖)
    static volatile LONG64 s_ccNotInGame;        // result=NOT_IN_GAME (인게임 미입장)
    static volatile LONG64 s_ccRateLimited;      // result=RATE_LIMITED (채널변경 쿨다운 거부)
    // 수신 content 정합 이상 - framing(길이)은 정상인데 내용이 오염된 경우. parseErr 로는 못 잡는 배치 교차오염/삼켜진 UAF 탐지.
    static volatile LONG64 s_recvContentErrors;  // SC_SPAWN 좌표 범위 밖, id 0, self-id 등 (0이어야 정상)
    static volatile LONG64 s_serializeFail;      // 송신 직렬화 실패 (CPacket::End false = 버퍼 초과. 0이어야 정상 - 보내는 쪽 버그 신호)
    static volatile LONG64 s_mapChanges;         // SC_MAP_CHANGE 로 mapId 가 실제로 바뀐 수 (포탈 성공 증거)
    // 아이템/줍기 (드랍-줍기 순환 e2e - 뿌린 수 >= 주운 수 정합 관측)
    static volatile LONG64 s_recvGroundItemSpawn;// SC_GROUND_ITEM_SPAWN (바닥 드랍 시야 등장 - 봇별 sightings)
    static volatile LONG64 s_sentPickup;         // CS_ITEM_PICKUP 송신 수 (줍기 시도)
    static volatile LONG64 s_recvInventory;      // SC_INVENTORY (입장 + 줍기/거래 성공 후 전량 스냅샷 - 성공 줍기 근사)

    // 검증 하니스 카운터 (B-1). H12 negative control.
    static volatile LONG64 s_badCredFail;        // H12: 가짜 토큰 봇이 게임 소켓 SC_LOGIN_RESULT{FAIL} 을 받은 수 (FAIL 경로 실제 행사 증거)
    static volatile LONG64 s_spuriousArm;        // H12: 거부돼야 할 봇에 arm 이 발동하면 증가 - 0이어야 정상 (arm 트리거 회귀 tripwire)
    static volatile LONG64 s_h1Divergence;       // H1: STOP 후 self-echo 가 STOP 위치에서 128px 초과 벗어난 수 (dead-reckon drift 버그. 수정 후 0)
    static volatile LONG64 s_h9SkipAdmitted;     // H9: 채널선택 생략하고 admission 성공한 수 (ch0 우회 버그. 수정 후 0=거부+종료)
    static volatile LONG64 s_h9SkipAttempted;    // H9: 채널선택 생략을 시도한 수 (0 이면 churn 부재로 미발화 - skipAdmitted 0 이 "수정됨"인지 "미시도"인지 구분)
    static volatile LONG64 s_h3Injected;         // H3: raw 절단 프레임 주입 횟수 (PROTOCOL_VIOLATION 트리거 발화 확인 - RST/FIN 구분은 Wireshark)

    // 검증 하니스 설정 (프로세스 전역, main 이 Run 전 1회 설정). 전부 false = 오늘 부하와 동일.
    static HarnessConfig s_harness;

private:
    bool PostRecv();                // WSARecv 게시(재무장)
    bool SendGameAuth();            // CS_GAME_AUTH(m_token) 송신 (Stage 2 첫 패킷, overlapped)
    static bool BlockingSendAll(SOCKET s, const BYTE* data, int size);                  // Stage 1 blocking 전송(부분송신 이어보냄)
    static bool BlockingRecvFrame(SOCKET s, BYTE* outFrame, int outCap, int& outLen);   // Stage 1 blocking 프레임 1개 수신(길이규약 2B)
    UINT32 NextRandom();            // 봇별 LCG (전역 rand 비-thread-safe 회피)
    UINT32 NextBounded(UINT32 bound);  // [0,bound) 균일 난수 - 상위 16비트 사용 (LCG 하위비트 % 편향 회피)

    // 행동 송신 - 모두 공용 m_actionSendCtx/m_actionSendBuf 사용(행동 1개/tick 가드라 동시 in-flight 1개=단일 버퍼 안전).
    bool SendBuilt(KYS::GAMECOMMON::PROTOCOL::CPacket& out);   // memcpy + WSASend (op=ACTION_SEND). 게시 성공 시 true
    bool BuildAndSendMove(int x, int y);                       // CS_MOVE (외삽 위치)
    bool BuildAndSendSkill();                                  // CS_SKILL (기본 공격)
    bool BuildAndSendPortal(int portalId);                     // CS_PORTAL
    bool BuildAndSendChat();                                   // CS_CHAT
    bool BuildAndSendWhisper(int targetBotIndex);              // CS_WHISPER (대상=bot%05d)
    bool BuildAndSendChannelChange(BYTE targetChannel);        // CS_CHANNEL_CHANGE (cross-thread 마이그레이션 부하 검증)
    bool BuildAndSendPickup(UINT32 objectId);                  // CS_ITEM_PICKUP (근처 바닥 드랍 줍기)
    bool BuildAndSendPing();                                   // CS_PING (keepalive - 조용한 구간에도 연결 유지, idle-kick 회피)
    bool BuildAndSendMoveStop(int x, int y);                   // H1: CS_MOVE{STOP} 프로브 (dead-reckon drift 관측용)
    bool BuildAndSendWhisperName(const wchar_t* targetName);   // H7: 임의 대상 이름 귓속말 (없는 이름/self 로 실패 경로 행사)
    bool BuildAndSendRawMalformed();                           // H3: raw 절단 프레임(00 03 FF) 직송 (PROTOCOL_VIOLATION 유발)

    // 검증 하니스 행동 분기 (각 플래그 on 일 때만 OnActionTick 이 위임 - off 면 미호출, 난수 미소비)
    void OnActionTickH1Stop(UINT64 now);      // H1: STOP 1회 후 주기 프로브 재전송 (침묵 사이 dead-reckon drift 유발)
    void OnActionTickH7Whisper(UINT64 now);   // H7: bogus 귓속말 사이클 (없는 이름 / 채팅 직후 rate-limit)
    void OnActionTickH3Inject(UINT64 now);    // H3: admission 후 raw 절단 프레임 1회 주입

    // 수신 파싱 (OnRecvComplete 가 완성 패킷마다 호출). 종류별 카운트 + 필요한 것(자기위치, 몬스터)만 deserialize.
    void HandlePacket(UINT16 type, const BYTE* payload, int payloadLen);

    // 몬스터 추적 (m_stateLock 보유 상태에서 호출 - 이름의 _NoLock 이 그 계약을 표시).
    void TrackMonster_NoLock(UINT32 id, int x, int y);    // spawn/move - 있으면 위치 갱신, 없으면 추가
    void UntrackMonster_NoLock(UINT32 id);                // death/despawn - 제거
    bool FindNearestMonster_NoLock(int& outX, int& outY) const;   // 내 위치 기준 최근접 추적 몬스터 (없으면 false)

    // 바닥 드랍 추적 (몬스터 추적과 같은 계약 - m_stateLock 보유 상태에서 호출). 줍기 대상 선정용.
    void TrackGroundItem_NoLock(UINT32 id, int x, int y);   // SC_GROUND_ITEM_SPAWN - 있으면 갱신, 없으면 추가
    void UntrackGroundItem_NoLock(UINT32 id);               // SC_DESPAWN - 제거 (줍힘/만료)
    bool FindNearestGroundItem_NoLock(int& outX, int& outY, UINT32& outId) const;   // 내 위치 기준 최근접 바닥 드랍 (없으면 false)

    // 항법 (m_stateLock 보유 상태) - 목표로 방향을 맞추고(dominant axis) 한 step 외삽.
    void SteerToward_NoLock(int tx, int ty);   // 목표 방향으로 m_dir 설정
    void AdvancePosition_NoLock();             // m_dir 로 한 걸음 (공유 AdvanceMove - 벽 정지·막히면 방향 재추첨. HW 프로브 봇0 만 벽 무시 옛 수식)
    bool FindNearestWallSpot_NoLock(int& outX, int& outY) const;   // 자기 맵에서 가장 가까운 벽 칸 중심 (없으면 false - HW 벽 게이트 프로브 목표)

    SOCKET       m_sock;
    BotIoContext m_recvCtx;
    BotIoContext m_sendCtx;
    BYTE         m_recvBuf[RECV_BUFFER_SIZE];   // framing 누적 버퍼 (경계 자르기)
    BYTE         m_sendBuf[MAX_PACKET_SIZE];   // 인증 overlapped send 버퍼 (CS_GAME_AUTH)
    UINT64       m_token;                       // Stage 1에서 획득한 일회용 토큰 (Stage 2 CS_GAME_AUTH로 제출)

    // 행동 사이클
    BotIoContext m_actionCtx;       // 타이머 PQCS 트리거 전용 (op=ACTION, 실제 WSASend 아님)
    BotIoContext m_actionSendCtx;   // 행동 WSASend 전용 (op=ACTION_SEND -> 완료=OnActionSendComplete)
    BYTE         m_actionSendBuf[MAX_PACKET_SIZE];   // 행동 overlapped send 버퍼 (행동 1개/tick -> in-flight 1개)
    EMoveDirection m_dir;           // 현재 이동 방향 (8방향 0..7) - action 워커 전용(가드라 단일 진입)
    UINT32         m_seq;           // clientSeq (매 CS_MOVE 단조 증가) - action 전용
    UINT32         m_rngState;      // 봇별 LCG 상태 - action 전용
    int            m_stepsUntilTurn;// 방향 유지 남은 틱 - action 전용
    volatile LONG  m_actionPending; // 봇당 in-flight 행동 1개 가드 (timer<->worker Interlocked)

    // recv 워커 <-> action 워커 공유 상태 보호 (둘이 동시에 다른 워커서 한 봇을 처리 가능 -> SRWLOCK).
    SRWLOCK m_stateLock;
    int m_x;        // 봇 자기 위치 (SC_MAP_CHANGE=서버 권위로 snap, OnActionTick=외삽) - 공유
    int m_y;
    int m_mapId;    // 내가 있는 맵 (SC_MAP_CHANGE 로 세팅) - 공유
    int m_targetMap;// 봇이 향하는 목표 맵 (도착하면 새로 뽑음) - 선형 체인서 한 방향 전진해 전 맵 행사 - 공유
    UINT32 m_selfPlayerId;   // 내 playerId (SC_MAP_CHANGE - self-echo/자기피격 구분) - 공유
    int m_channelCount;      // SC_CHANNEL_LIST 로 받은 채널 수 (채널 변경 대상 범위) - 로그인 초반 1회 세팅 후 불변
    // 추적 몬스터 (스킬 타겟 선정) - 공유 (m_stateLock)
    struct TrackedMonster { UINT32 id; int x; int y; };
    TrackedMonster m_monsters[MAX_TRACKED_MONSTERS];
    int m_monsterCount;
    // 추적 바닥 드랍 (줍기 대상 선정) - 공유 (m_stateLock)
    struct TrackedGroundItem { UINT32 id; int x; int y; };
    TrackedGroundItem m_groundItems[MAX_TRACKED_GROUND_ITEMS];
    int m_groundItemCount;

    // 행동 페이싱 게이트 (GetTickCount64 기준, action 전용)
    UINT64 m_nextSkillMs;     // 다음 스킬 허용 시각 (서버 쿨다운 0.5s 보다 약간 길게)
    UINT64 m_nextPortalMs;    // 다음 포탈 허용 시각 (즉시 재포탈 방지)
    UINT64 m_nextChatMs;      // 다음 채팅 허용 시각 (서버 500ms 도배제한 준수)
    UINT64 m_nextWhisperMs;   // 다음 귓속말 허용 시각
    UINT64 m_nextChannelChangeMs;   // 다음 채널 변경 허용 시각 (드물게 - 마이그레이션 부하 검증)
    UINT64 m_nextPickupMs;          // 다음 줍기 허용 시각 (근처 드랍 줍기 페이싱)
    UINT64 m_nextPingMs;            // 다음 keepalive CS_PING 허용 시각 (조용해도 연결 유지 - idle-kick 회피)
    volatile LONG m_peerBotCount;   // 현재 활성 봇 수 (PostAction 가 갱신, 귓속말 대상 범위)

    // reconnect churn 상태 (TimerProc 단일 스레드 접근)
    int m_botIndex;            // 슬롯 인덱스 - 위상 시드/채널 선택/재연결 식별 (로그인 계정과 분리)
    int m_accountIndex;        // 로그인 계정 인덱스 (bot%05d) - 중복 로그인 모드면 슬롯과 달리 좁은 범위를 공유. 재연결도 이 계정으로
    bool m_badCred;            // H12: 이 봇이 가짜 토큰으로 게임 서버 admission 거부를 유발하는 negative control 인가 (arm tripwire 표식)
    int m_aliveTicks;          // 연결 수명 카운터
    int m_reconnectCountdown;  // Close 후 재연결 대기

    // 검증 하니스 상태 (H1/H7/H9 - 각 플래그 off 면 미사용, 미접근). H1 3필드는 m_stateLock 보호(recv self-echo 와 action 워커 공유).
    int    m_h1MoveTicks;      // H1: STOP 전 이동(START) 워밍업 진행 tick (서버 이동 상태 확립용 - 이게 없으면 서버가 dead-reckon 안 해 표류 미발생)
    int    m_h1Stopped;        // H1: STOP 을 보내고 프로브 모드에 든 상태인가 (self-echo divergence 게이트)
    int    m_h1StopX;          // H1: STOP 을 보낸 의도 위치 (self-echo drift 비교 기준)
    int    m_h1StopY;
    UINT64 m_h1NextProbeMs;    // H1: 다음 STOP 프로브 재전송 시각 (action 워커 전용)
    int    m_h7Step;           // H7: bogus 귓속말 사이클 단계 (0=채팅 1=직후 귓속말[rate-limit] 2=없는 이름 귓속말[offline]) (action 워커 전용)
    UINT64 m_h7NextMs;         // H7: 다음 H7 행동 시각 (action 워커 전용)
    UINT32 m_cachedCharId;     // H9: 첫 정상 로그인서 캐시한 charId - 재접속 시 채널선택 생략 직송용 (재접속 넘어 보존, recv 워커 전용)
    int    m_h9Skipped;        // H9: 이번 접속서 채널선택을 생략했는가 (admission 성공 시 카운트 게이트, recv 워커 전용)
    int    m_h3Injected;       // H3: 이번 접속서 raw 프레임을 이미 주입했는가 (1회성, action 워커 전용)

    // 송신 위상 분산 + recv framing
    UINT64 m_nextActionMs;   // 다음 행동 허용 시각(GetTickCount64) - 봇별 위상 offset
    int    m_recvLen;        // recv framing 누적 길이 (TCP 스트림 경계 자르기)
    volatile LONG m_authenticated;   // 인증 완료 (SC_ENTER_WORLD 수신)
    volatile LONG m_placed;          // 자기위치 수신 완료 (SC_MAP_CHANGE) - 그 전엔 게임패킷 송신 안 함

    KYS::GAMECOMMON::PROTOCOL::CPacket m_parsePkt;   // 수신 deserialize 재사용 버퍼 (recv 워커 전용, per-bot 직렬)

    // opcode 난독화 방향별 rolling 키 (서버 GameSession 과 대칭). armed 전엔 평문(pre-auth), SC_ENTER_WORLD 수신 후 무장.
    //   send 키=action 워커 접근(SendBuilt), recv 키=recv 워커 접근(OnRecvComplete) - 방향별 단일 접근이라 락 불요.
    KYS::GAMECOMMON::PROTOCOL::ObfKeyState m_sendKey;   // C2S 방향 (내가 보냄 = 서버 recv 키와 같은 시드)
    KYS::GAMECOMMON::PROTOCOL::ObfKeyState m_recvKey;   // S2C 방향 (내가 받음 = 서버 send 키와 같은 시드)
    bool                                   m_obfArmed;  // 키 무장 여부 (false=평문 / true=scramble)
};
