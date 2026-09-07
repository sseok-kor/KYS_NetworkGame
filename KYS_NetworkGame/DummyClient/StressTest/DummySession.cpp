#include "pch_dummyclient.h"
#include "DummySession.h"
#include "../../GameCommon/Protocol/CPacket.h"        // CPacket (KYS::GAMECOMMON::PROTOCOL)
#include "../../GameCommon/Protocol/GamePackets.h"    // LoginReq / CS_MOVE / CS_SKILL / CS_PORTAL / CS_CHAT / CS_WHISPER / SC_*
#include "../../GameCommon/Protocol/PacketType.h"     // PacketType
#include "../../GameCommon/GameDefines.h"    // MAX_MOVE_SPEED / MAP_*  / SKILL_HIT_RANGE / CHAT_MSG_MAX / WHISPER_NAME_MAX
#include "../../GameCommon/MapData/MapTable.h"       // MapTableCount(봇 목표 맵 추첨)/MapWidthFor·HeightFor(맵별 크기 - 좌표 검사/포탈 방향)
#include "../../GameCommon/MapData/WalkableTable.h"  // AdvanceMove (봇 이동 - 서버/클라와 같은 수식 + 같은 벽 정지)
#include "../../GameCommon/Pathfinding/Direction.h"      // DirectionToward (봇 조향 8방) / MOVE_DIR_DELTA (hw 프로브 전용 수식)
#include "../../GameCommon/MapData/PortalTable.h"     // PortalTableCount/At (봇 포탈 내비 - 서버와 같은 Data/portals.csv)
#include "../../GameCommon/Protocol/PacketHeader.h"   // PacketHeader (recv framing size/type 경계)
#include <string.h>                          // memcpy / memmove
#include <cwchar>                            // swprintf_s / wcscpy_s

// 봇 1개의 소켓/IO/행동 구현. 부하 발생기 + e2e 검증기.

// 포탈은 서버와 같은 공유 표(Data/portals.csv - LoadPortalTable 을 봇 main 이 부팅 시 적재)를 읽는다.
//   하드코딩 미러 폐기 - 포탈 추가/이동이 파일 한 줄이면 봇도 자동 동기(portalId = 행 순서·dst 는 서버 권위).
//   [!] 방향 판별(트리거 x 가 맵 중앙보다 오른쪽 = 다음 맵)은 선형 체인 배치 전제 - 중앙 근처 포탈이 생기면 재검토.

// 수신 e2e 검증 카운터 정의 (선언=DummySession.h). 워커 다수 -> Interlocked 갱신.
volatile LONG64 DummySession::s_recvPackets = 0;
volatile LONG64 DummySession::s_recvParseErrors = 0;
volatile LONG64 DummySession::s_recvMonsterSpawns = 0;
volatile LONG64 DummySession::s_recvMapChange = 0;
volatile LONG64 DummySession::s_recvMoveBroadcast = 0;
volatile LONG64 DummySession::s_recvSpawn = 0;
volatile LONG64 DummySession::s_recvDespawn = 0;
volatile LONG64 DummySession::s_recvMonsterMove = 0;
volatile LONG64 DummySession::s_recvDamage = 0;
volatile LONG64 DummySession::s_recvDeath = 0;
volatile LONG64 DummySession::s_recvRespawn = 0;
volatile LONG64 DummySession::s_recvChat = 0;
volatile LONG64 DummySession::s_recvWhisper = 0;
volatile LONG64 DummySession::s_recvWhisperFail = 0;
volatile LONG64 DummySession::s_authSuccess = 0;
volatile LONG64 DummySession::s_authFail = 0;
volatile LONG64 DummySession::s_sentSkill = 0;
volatile LONG64 DummySession::s_sentPortal = 0;
volatile LONG64 DummySession::s_sentChat = 0;
volatile LONG64 DummySession::s_sentWhisper = 0;
volatile LONG64 DummySession::s_sentChannelChange = 0;
volatile LONG64 DummySession::s_recvChannelChange = 0;
volatile LONG64 DummySession::s_ccOk = 0;
volatile LONG64 DummySession::s_ccSame = 0;
volatile LONG64 DummySession::s_ccFull = 0;
volatile LONG64 DummySession::s_ccInvalid = 0;
volatile LONG64 DummySession::s_ccNotInGame = 0;
volatile LONG64 DummySession::s_ccRateLimited = 0;
volatile LONG64 DummySession::s_recvContentErrors = 0;
volatile LONG64 DummySession::s_mapChanges = 0;
volatile LONG64 DummySession::s_recvGroundItemSpawn = 0;
volatile LONG64 DummySession::s_sentPickup = 0;
volatile LONG64 DummySession::s_recvInventory = 0;

// 검증 하니스 카운터/설정 정의 (선언=DummySession.h). 기본 0/false = 프로덕션 부하와 동일.
volatile LONG64 DummySession::s_badCredFail = 0;
volatile LONG64 DummySession::s_spuriousArm = 0;
volatile LONG64 DummySession::s_h1Divergence = 0;
volatile LONG64 DummySession::s_h9SkipAdmitted = 0;
volatile LONG64 DummySession::s_h9SkipAttempted = 0;
volatile LONG64 DummySession::s_h3Injected = 0;
HarnessConfig DummySession::s_harness = { false, false, false, false, false, false, false, false, false };

DummySession::DummySession()
    : m_sock(INVALID_SOCKET)
    , m_recvCtx{}
    , m_sendCtx{}
    , m_recvBuf{}
    , m_sendBuf{}
    , m_token(0)
    , m_actionCtx{}
    , m_actionSendCtx{}
    , m_actionSendBuf{}
    , m_dir(EMoveDirection::RIGHT)
    , m_seq(0)
    , m_rngState(1)                       // AssociateAndStart 가 botIndex 로 재시드
    , m_stepsUntilTurn(0)
    , m_actionPending(0)
    , m_x(0)
    , m_y(0)
    , m_mapId(0)
    , m_targetMap(0)
    , m_selfPlayerId(0)
    , m_channelCount(0)
    , m_monsters{}
    , m_monsterCount(0)
    , m_groundItems{}
    , m_groundItemCount(0)
    , m_nextSkillMs(0)
    , m_nextPortalMs(0)
    , m_nextChatMs(0)
    , m_nextWhisperMs(0)
    , m_nextChannelChangeMs(0)
    , m_nextPickupMs(0)
    , m_nextPingMs(0)
    , m_peerBotCount(0)
    , m_botIndex(0)
    , m_accountIndex(0)
    , m_badCred(false)
    , m_aliveTicks(0)
    , m_reconnectCountdown(0)
    , m_h1MoveTicks(0)
    , m_h1Stopped(0)
    , m_h1StopX(0)
    , m_h1StopY(0)
    , m_h1NextProbeMs(0)
    , m_h7Step(0)
    , m_h7NextMs(0)
    , m_cachedCharId(0)
    , m_h9Skipped(0)
    , m_h3Injected(0)
    , m_nextActionMs(0)
    , m_recvLen(0)
    , m_authenticated(0)
    , m_placed(0)
    , m_parsePkt(RECV_BUFFER_SIZE)        // 수신 deserialize 재사용 버퍼
    , m_sendKey{ 0 }
    , m_recvKey{ 0 }
    , m_obfArmed(false)
{
    ::InitializeSRWLock(&m_stateLock);    // SRWLOCK 은 생성자 없는 구조체 -> 본체서 초기화 (소멸 불요)
}

DummySession::~DummySession()
{
    Close();
}

bool DummySession::Connect(const wchar_t* ip, unsigned short port)
{
    m_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_sock == INVALID_SOCKET)
        return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::InetPtonW(AF_INET, ip, &addr.sin_addr) != 1)   // W-버전 (Unicode)
    {
        Close();
        return false;
    }

    // blocking connect - ramp 단계가 초당 K개로 페이싱하므로 ConnectEx 불요.
    if (::connect(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        Close();
        return false;
    }
    return true;
}

bool DummySession::AssociateAndStart(HANDLE iocp, int botIndex)
{
    // (1) 상태 전체 초기화 - recv 무장/login 송신 '이전'에 끝낸다.
    //   이유: PostRecv 가 m_recvLen 을 읽어 무장 오프셋/길이를 정하므로 m_recvLen 리셋이 먼저여야
    //   재연결(churn) 시 직전 연결 잔여로 framing 이 손상되지 않는다. 또한 인증/배치/가드를 먼저 내려
    //   직전 연결의 stale ACTION 완료가 게이트(m_placed=0)에 막혀 공유 상태를 만지지 못하게 한다.
    m_botIndex = botIndex;
    m_seq = 0;
    m_aliveTicks = -(botIndex % RECONNECT_LIFETIME_TICKS);   // 음수 위상 -> 수명 분산
    m_reconnectCountdown = 0;
    // 검증 하니스 per-connection 상태 리셋 (m_cachedCharId 는 제외 - 재접속 넘어 보존이 H9 의 핵심)
    m_h1MoveTicks = 0;
    m_h1Stopped = 0;
    m_h1NextProbeMs = 0;
    m_h7Step = 0;
    m_h7NextMs = 0;
    m_h9Skipped = 0;
    m_h3Injected = 0;
    m_rngState = static_cast<UINT32>(botIndex) * 2654435761u + 1u;   // 봇별 고유 seed (Knuth 승수 해시)
    m_dir = static_cast<EMoveDirection>(NextBounded(8));           // 8방 랜덤 (RNG 소비 1회 유지 - 4->8 로 방향 분포 기준선만 변동)
    m_stepsUntilTurn = 10 + static_cast<int>(NextBounded(20));     // 10~29 틱 후 방향 재선택
    m_nextSkillMs = 0;
    m_nextPortalMs = 0;
    m_nextChatMs = 0;
    m_nextWhisperMs = 0;
    m_nextChannelChangeMs = ::GetTickCount64() + 5000;   // 접속 직후 잠깐은 채널 유지 (배치/스폰 안정 후 이동)
    m_nextPingMs = ::GetTickCount64() + static_cast<UINT64>(NextBounded(static_cast<UINT32>(KEEPALIVE_INTERVAL_MS)));   // 첫 ping 위상 분산 (봇마다 [0,10s) 랜덤 offset -> 동시 ping 버스트 회피), 이후 10s 주기
    m_nextActionMs = ::GetTickCount64() + (NextBounded(static_cast<UINT32>(BOT_MOVE_INTERVAL_MS)));   // 위상 분산
    m_recvLen = 0;                                // recv framing 누적 리셋 (PostRecv 가 읽으므로 먼저)
    m_obfArmed = false;                           // 새 연결 = pre-auth 평문 복귀 (재사용 슬롯의 직전 무장 잔여 봉인 - 서버 슬롯 회수 키 리셋과 대칭)
    ::InterlockedExchange(&m_actionPending, 0);
    ::InterlockedExchange(&m_authenticated, 0);   // 재연결 시 인증 상태 초기화
    ::InterlockedExchange(&m_placed, 0);          // 자기위치 재수신까지 게임패킷 보류 (stale ACTION 게이트 차단)

    // 공유 상태(recv 워커 <-> action 워커)는 락 안에서 초기화 - _NoLock 계약 준수.
    ::AcquireSRWLockExclusive(&m_stateLock);
    m_x = 0;                              // 자기위치는 SC_MAP_CHANGE 수신 시 서버 권위로 snap (그 전엔 행동 안 함)
    m_y = 0;
    m_mapId = 0;
    m_targetMap = static_cast<int>(NextBounded(static_cast<UINT32>(MapTableCount())));   // 향할 목표 맵 - 실제 맵 수(maps.csv)로 추첨 (시작 맵과 같으면 OnActionTick 도착 처리서 새로 뽑힘)
    m_selfPlayerId = 0;
    m_monsterCount = 0;
    m_groundItemCount = 0;
    ::ReleaseSRWLockExclusive(&m_stateLock);

    // (2) 상태가 모두 초기화된 뒤 소켓을 완료 포트에 등록(key=this) + recv 무장 + 로그인 송신.
    if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(m_sock), iocp,
            reinterpret_cast<ULONG_PTR>(this), 0) == NULL)
        return false;
    if (!PostRecv())
        return false;

    return SendGameAuth();   // Stage 2 첫 패킷 = CS_GAME_AUTH(Stage 1에서 받은 m_token)
}

bool DummySession::PostRecv()
{
    m_recvCtx.m_op = EBotOp::RECV;
    ::ZeroMemory(&m_recvCtx.m_overlapped, sizeof(OVERLAPPED));
    // framing 누적: 이미 쌓인 m_recvLen 뒤에 이어받는다(부분 패킷 보존). 남은 공간만큼만 무장.
    m_recvCtx.m_wsaBuf.buf = reinterpret_cast<char*>(m_recvBuf + m_recvLen);
    m_recvCtx.m_wsaBuf.len = static_cast<ULONG>(RECV_BUFFER_SIZE - m_recvLen);

    DWORD flags = 0;
    DWORD received = 0;
    int ret = ::WSARecv(m_sock, &m_recvCtx.m_wsaBuf, 1, &received, &flags,
        &m_recvCtx.m_overlapped, NULL);
    if (ret == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
        return false;

    return true;
}

bool DummySession::SendGameAuth()
{
    KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_GAME_AUTH));   // size 예약 + type(BE 태깅)
    CS_GAME_AUTH req{};
    req.token = m_token;   // Stage 1에서 받은 일회용 토큰
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }

    int size = out.GetSize();
    if (size <= 0 || size > MAX_PACKET_SIZE)
        return false;

    // overlapped 송신은 비동기라 버퍼가 완료까지 살아있어야 한다 -> 멤버 m_sendBuf 로 복사.
    ::memcpy(m_sendBuf, out.GetBuffer(), static_cast<size_t>(size));

    m_sendCtx.m_op = EBotOp::SEND;
    ::ZeroMemory(&m_sendCtx.m_overlapped, sizeof(OVERLAPPED));
    m_sendCtx.m_wsaBuf.buf = reinterpret_cast<char*>(m_sendBuf);
    m_sendCtx.m_wsaBuf.len = static_cast<ULONG>(size);

    DWORD sent = 0;
    int ret = ::WSASend(m_sock, &m_sendCtx.m_wsaBuf, 1, &sent, 0,
        &m_sendCtx.m_overlapped, NULL);
    if (ret == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
        return false;

    return true;
}

// Stage 1: LoginServer로 blocking 2단계 핸드셰이크 - CS_LOGIN -> SC_SERVER_LIST -> CS_SERVER_SELECT -> SC_LOGIN_TOKEN.
//   게임 IOCP 소켓과 별개의 임시 blocking 소켓(ramp 가 페이싱). 성공 시 m_token 채우고 true(0 토큰은 실패).
bool DummySession::AcquireToken(const wchar_t* loginIp, unsigned short loginPort, int accountIndex, bool badCred)
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    m_token = 0;
    m_accountIndex = accountIndex;   // 재연결도 같은 계정으로 로그인하도록 보관 (중복 로그인 모드의 공유 계정 유지)
    m_badCred = badCred;             // H12: 게임 소켓 FAIL 수신 시 arm 안 하는지 감시하는 표식

    if (badCred)
    {
        // H12: LoginServer 를 건너뛰고 가짜 토큰으로 게임 서버 admission 거부 경로를 실제 행사한다.
        //   Connect + CS_GAME_AUTH(가짜 토큰) -> 서버 verify NOT_FOUND -> RejectLogin 이 게임 소켓에
        //   SC_LOGIN_RESULT{FAIL} 회신 -> 클라는 이 FAIL 에 arm 하지 않아야 함(ArmObfuscation tripwire 가 감시).
        //   실토큰과 충돌 없는 명백한 가짜값(실봇은 이 경로를 안 탐).
        m_token = 0xBADC0DE000000000ULL + static_cast<UINT64>(static_cast<UINT32>(accountIndex));
        return true;
    }

    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET)
        return false;
    DWORD timeout = 5000;   // recv 타임아웃 (LoginServer 무응답 시 봇 멈춤 방지)
    ::setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(loginPort);
    if (::InetPtonW(AF_INET, loginIp, &addr.sin_addr) != 1) { ::closesocket(ls); return false; }
    if (::connect(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { ::closesocket(ls); return false; }

    const int headerSize = static_cast<int>(sizeof(PacketHeader));
    BYTE frame[1024];
    int  frameLen = 0;

    // (1) CS_LOGIN 송신
    {
        CPacket out(MAX_PACKET_SIZE);
        out.Begin(static_cast<USHORT>(PacketType::CS_LOGIN));
        LoginReq req{};
        ::swprintf_s(req.id, LoginReq::ID_MAX, L"bot%05d", accountIndex);
        ::wcscpy_s(req.pw, PASSWORD_MAX, L"loadtest");   // 봇은 $test$ 계정이라 pw 내용 무시(PBKDF2 우회 fast-path)
        req.Serialize(out);
        if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); ::closesocket(ls); return false; }
        if (!BlockingSendAll(ls, out.GetBuffer(), out.GetSize())) { ::closesocket(ls); return false; }
    }
    // (2) SC_SERVER_LIST 수신
    if (!BlockingRecvFrame(ls, frame, static_cast<int>(sizeof(frame)), frameLen)) { ::closesocket(ls); return false; }
    if (frameLen < headerSize ||
        ::ntohs(*reinterpret_cast<UINT16*>(frame + sizeof(UINT16))) != static_cast<USHORT>(PacketType::SC_SERVER_LIST))
    { ::closesocket(ls); return false; }
    // (3) CS_SERVER_SELECT(0) 송신
    {
        CPacket out(MAX_PACKET_SIZE);
        out.Begin(static_cast<USHORT>(PacketType::CS_SERVER_SELECT));
        CS_SERVER_SELECT sel{};
        sel.serverId = 0;
        sel.Serialize(out);
        if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); ::closesocket(ls); return false; }
        if (!BlockingSendAll(ls, out.GetBuffer(), out.GetSize())) { ::closesocket(ls); return false; }
    }
    // (4) SC_LOGIN_TOKEN 수신 -> 토큰 추출
    if (!BlockingRecvFrame(ls, frame, static_cast<int>(sizeof(frame)), frameLen)) { ::closesocket(ls); return false; }
    if (frameLen <= headerSize ||
        ::ntohs(*reinterpret_cast<UINT16*>(frame + sizeof(UINT16))) != static_cast<USHORT>(PacketType::SC_LOGIN_TOKEN))
    { ::closesocket(ls); return false; }
    {
        CPacket pkt(frameLen - headerSize);
        pkt.Write(frame + headerSize, frameLen - headerSize);
        SC_LOGIN_TOKEN tok{};
        tok.Deserialize(pkt);
        m_token = tok.token;   // serverIp/serverPort(redirect)는 미사용 - 봇은 게임 서버 IP 를 stdin 입력에서, 포트를 설정 파일에서 받아 ramp 가 지정한다
    }
    ::closesocket(ls);
    return m_token != 0;
}

// blocking 전송: size 바이트 전부 보낼 때까지 이어보냄.
bool DummySession::BlockingSendAll(SOCKET s, const BYTE* data, int size)
{
    int sent = 0;
    while (sent < size)
    {
        const int n = ::send(s, reinterpret_cast<const char*>(data + sent), size - sent, 0);
        if (n == SOCKET_ERROR || n == 0)
            return false;
        sent += n;
    }
    return true;
}

// blocking 수신: 길이규약(앞 2B=전체길이)으로 완성 프레임 1개를 받는다(부분 수신 누적). 실패/타임아웃 false.
//   핸드셰이크는 request-response 라 한 번에 한 프레임만 와서 over-read 잔여 처리는 불요.
bool DummySession::BlockingRecvFrame(SOCKET s, BYTE* outFrame, int outCap, int& outLen)
{
    int have = 0;
    while (have < static_cast<int>(sizeof(UINT16)))   // 길이 2B 먼저 확보
    {
        const int n = ::recv(s, reinterpret_cast<char*>(outFrame + have), outCap - have, 0);
        if (n <= 0) return false;
        have += n;
    }
    const int pktSize = static_cast<int>(::ntohs(*reinterpret_cast<UINT16*>(outFrame)));
    if (pktSize < static_cast<int>(sizeof(PacketHeader)) || pktSize > outCap)
        return false;
    while (have < pktSize)   // 나머지 수신
    {
        const int n = ::recv(s, reinterpret_cast<char*>(outFrame + have), outCap - have, 0);
        if (n <= 0) return false;
        have += n;
    }
    outLen = pktSize;
    return true;
}

void DummySession::OnRecvComplete(int transferred)
{
    if (transferred <= 0)   // 0 = 원격 graceful close, 음수 방어
    {
        Close();
        return;
    }

    // framing: TCP 스트림에서 [size(2B BE)][type(2B BE)][payload] 경계를 자른다 (서버 Session 미러 - ntohs).
    m_recvLen += transferred;
    int offset = 0;
    while (m_recvLen - offset >= static_cast<int>(sizeof(PacketHeader)))   // 헤더 4B 이상 있어야 size 읽음
    {
        const UINT16 pktSize = ::ntohs(*reinterpret_cast<UINT16*>(m_recvBuf + offset));   // 앞 2B = 전체 길이(BE)
        if (pktSize < sizeof(PacketHeader) || pktSize > RECV_BUFFER_SIZE)
        {
            ::InterlockedIncrement64(&s_recvParseErrors);   // 길이 비정상 = wire 깨짐 -> 버퍼 리셋
            m_recvLen = 0;
            offset = 0;
            break;
        }
        if (m_recvLen - offset < pktSize)
        {
            break;   // 부분 패킷 - 다음 recv 까지 보존
        }
        if (m_obfArmed)
            KYS::GAMECOMMON::PROTOCOL::DeobfuscateOpcode(m_recvBuf + offset, m_recvKey);   // opcode 평문화 + recv 키 advance (완성 패킷당 정확히 1회)
        const UINT16 pktType = ::ntohs(*reinterpret_cast<UINT16*>(m_recvBuf + offset + sizeof(UINT16)));   // 다음 2B = type(BE)
        HandlePacket(pktType,
            m_recvBuf + offset + sizeof(PacketHeader),
            static_cast<int>(pktSize) - static_cast<int>(sizeof(PacketHeader)));
        offset += pktSize;   // 완성 패킷 소비
    }
    // 남은 부분 패킷을 버퍼 앞으로 이동 (다음 recv 가 이어서 채움)
    if (offset > 0)
    {
        m_recvLen -= offset;
        if (m_recvLen > 0) { ::memmove(m_recvBuf, m_recvBuf + offset, static_cast<size_t>(m_recvLen)); }
    }

    PostRecv();   // recv 재무장 (framing 누적 위치로)
}

// 완성 패킷 1개 처리 - 종류별 카운트 + 필요한 것(자기위치, 몬스터)만 deserialize. recv 워커 전용(per-bot 직렬).
void DummySession::HandlePacket(UINT16 type, const BYTE* payload, int payloadLen)
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    ::InterlockedIncrement64(&s_recvPackets);

    // deserialize 하는 종류는 wire payload 크기 sanity 를 먼저 본다 (필드 드리프트/순서버그 색출 - parent 목표).
    //   불일치 = s_recvParseErrors 로 집계하고 deserialize skip. (SerializationBuffer Read 는 언더플로 graceful이나
    //   '정확한 크기'를 단언해야 wire 버그가 카운터에 드러난다.)
    switch (static_cast<PacketType>(type))
    {
    case PacketType::SC_ENTER_WORLD:
    {
        // 입장 완료(admission-ack) = 성공 경로만 보내는 전용 opcode. payload 없음 - 수신 자체가 성공 신호.
        if (::InterlockedExchange(&m_authenticated, 1) == 0)
        {
            ::InterlockedIncrement64(&s_authSuccess);
            if (s_harness.skipChannel && m_h9Skipped != 0)
            {
                ::InterlockedIncrement64(&s_h9SkipAdmitted);   // H9: 채널선택 생략했는데 admission 성공 = ch0 우회 버그 (수정 후엔 거부+종료로 여기 못 옴)
                m_h9Skipped = 0;
            }
        }
        // 평문으로 받은 직후 무장 - 이 패킷은 unarmed 라 정상 판독되고, 같은 루프의 다음 패킷(SC_INVENTORY)부터 descramble.
        //   (recv 워커 단일 스레드라 framing 의 다음 반복에 동기 반영. 이미 무장이면 재시드로 키 위치가 어긋나지 않게 skip.)
        if (!m_obfArmed)
            ArmObfuscation();
        break;
    }
    case PacketType::SC_LOGIN_RESULT:
    {
        // 이제 성공은 SC_ENTER_WORLD 로 오고, 이 opcode 는 거부/인증실패(FAIL)만 운반한다.
        if (payloadLen >= 1 && payload[0] != static_cast<BYTE>(ELoginResult::OK))   // payload 첫 바이트 = result (BYTE, endian 무관)
        {
            ::InterlockedIncrement64(&s_authFail);
            if (m_badCred) { ::InterlockedIncrement64(&s_badCredFail); }   // H12: 가짜 토큰 봇이 게임 소켓 FAIL 을 실제로 받음(경로 행사 증거)
        }
        break;
    }
    case PacketType::SC_CHANNEL_LIST:   // 채널 목록 -> 봇은 botIndex 분산으로 자동 선택(UI 없음)
    {
        if (payloadLen < 1) { ::InterlockedIncrement64(&s_recvParseErrors); break; }
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_CHANNEL_LIST body{};
        body.Deserialize(m_parsePkt);
        m_channelCount = static_cast<int>(body.channelCount);   // 채널 변경 대상 범위 (로그인 초반 1회 세팅, 이후 불변)
        if (s_harness.skipChannel && m_cachedCharId != 0)
        {
            // H9: 채널 선택(CS_CHANNEL_SELECT)을 건너뛰고 캐시된 charId 로 캐릭터 선택 직송 -> 채널 미선택 admission(ch0 우회) 시도.
            //   수정 전엔 서버가 기본 채널로 붙여 우회 성공, 수정 후엔 순서 위반으로 거부+종료.
            CPacket skip(MAX_PACKET_SIZE);
            skip.Begin(static_cast<USHORT>(PacketType::CS_CHARACTER_SELECT));
            CS_CHARACTER_SELECT sel{};
            sel.charId = m_cachedCharId;
            sel.Serialize(skip);
            const bool built = skip.End(); if (!built) { ::InterlockedIncrement64(&s_serializeFail); }
            if (built) { SendBuilt(skip); }
            m_h9Skipped = 1;
            ::InterlockedIncrement64(&s_h9SkipAttempted);   // 시도 자체를 센다 - skipAdmitted 0 이 "거부됨(수정)"인지 "미시도(churn 부재)"인지 구분
            break;
        }
        BYTE pick = (body.channelCount > 0) ? static_cast<BYTE>(m_botIndex % static_cast<int>(body.channelCount)) : static_cast<BYTE>(0);
        if (s_harness.hotspot) { pick = 0; }   // H2: 전 봇 채널0 집중 -> 한 채널 tick 과부하 (shed 압력)
        CPacket out(MAX_PACKET_SIZE);
        out.Begin(static_cast<USHORT>(PacketType::CS_CHANNEL_SELECT));
        CS_CHANNEL_SELECT sel{};
        sel.channelId = pick;
        sel.Serialize(out);
        const bool built = out.End(); if (!built) { ::InterlockedIncrement64(&s_serializeFail); }
        if (built) { SendBuilt(out); }
        break;
    }
    case PacketType::SC_CHARACTER_LIST:   // 채널 선택 후 캐릭터 목록 -> 봇은 slot0 자동 선택(없으면 생성)
    {
        if (payloadLen < 1) { ::InterlockedIncrement64(&s_recvParseErrors); break; }
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_CHARACTER_LIST body{};
        body.Deserialize(m_parsePkt);
        if (body.count > 0)
        {
            // 첫 캐릭터(slot 순) 선택 -> admission(GameSession 생성).
            CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::CS_CHARACTER_SELECT));
            CS_CHARACTER_SELECT sel{};
            sel.charId = body.chars[0].charId;
            if (s_harness.skipChannel) { m_cachedCharId = body.chars[0].charId; }   // H9: 재접속 시 채널선택 생략 직송에 쓸 charId 캐시
            sel.Serialize(out);
            const bool built = out.End(); if (!built) { ::InterlockedIncrement64(&s_serializeFail); }
            if (built) { SendBuilt(out); }        // CS_CHARACTER_SELECT 평문 송신 - 무장은 서버 SC_ENTER_WORLD(admission 성공) 수신 시로 미룸
        }
        else
        {
            // 캐릭터 없음(첫 로그인) -> slot0 에 자동 생성(이름=로그인 id). OK 응답이 오면 그 캐릭터 선택.
            //   봇이 자기 캐릭을 self-provision -> seed_bots.sql 은 accounts 만 있으면 됨.
            CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::CS_CHARACTER_CREATE));
            CS_CHARACTER_CREATE req{};
            ::swprintf_s(req.name, WHISPER_NAME_MAX, L"bot%05d", m_accountIndex);
            req.slotId = 0;
            req.Serialize(out);
            const bool built = out.End(); if (!built) { ::InterlockedIncrement64(&s_serializeFail); }
            if (built) { SendBuilt(out); }
        }
        break;
    }
    case PacketType::SC_CHARACTER_CREATE_RESULT:   // 생성 결과 -> OK면 새 캐릭터 선택(입장)
    {
        if (payloadLen < 5) { ::InterlockedIncrement64(&s_recvParseErrors); break; }   // result(1)+charId(4)
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_CHARACTER_CREATE_RESULT body{};
        body.Deserialize(m_parsePkt);
        if (body.result == static_cast<BYTE>(ECharCreateResult::OK))
        {
            CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::CS_CHARACTER_SELECT));
            CS_CHARACTER_SELECT sel{};
            sel.charId = body.charId;
            if (s_harness.skipChannel) { m_cachedCharId = body.charId; }   // H9: 재접속 시 채널선택 생략 직송에 쓸 charId 캐시 (self-provision 경로)
            sel.Serialize(out);
            const bool built = out.End(); if (!built) { ::InterlockedIncrement64(&s_serializeFail); }
            if (built) { SendBuilt(out); }        // CS_CHARACTER_SELECT 평문 송신 - 무장은 서버 SC_ENTER_WORLD(admission 성공) 수신 시로 미룸
        }
        break;
    }
    case PacketType::SC_MAP_CHANGE:   // 자기 맵/위치 통지 - 서버 권위로 snap (이게 와야 행동 시작)
    {
        ::InterlockedIncrement64(&s_recvMapChange);
        if (payloadLen != 16) { ::InterlockedIncrement64(&s_recvParseErrors); break; }   // playerId4+mapId4+x4+y4
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_MAP_CHANGE body{};
        body.Deserialize(m_parsePkt);
        bool mapChanged = false;
        ::AcquireSRWLockExclusive(&m_stateLock);
        const bool wasPlaced = (m_placed != 0);
        if (wasPlaced && body.mapId != m_mapId) { mapChanged = true; }   // 포탈로 맵 이동 = 성공 증거
        m_x = body.x;
        m_y = body.y;
        m_mapId = body.mapId;
        m_selfPlayerId = body.playerId;
        m_monsterCount = 0;   // 맵 바뀌면 옛 맵 몬스터 추적 비움
        m_groundItemCount = 0;   // 옛 맵 바닥 드랍 추적도 비움
        ::ReleaseSRWLockExclusive(&m_stateLock);
        ::InterlockedExchange(&m_placed, 1);
        if (mapChanged) { ::InterlockedIncrement64(&s_mapChanges); }
        break;
    }
    case PacketType::SC_CHAR_INFO:   // 로그인 시 자기 캐릭터 전체 초기 상태(hp 포함) - 봇은 배치(위치/id/맵)만 사용, hp 무시
    {
        ::InterlockedIncrement64(&s_recvMapChange);   // 자기 위치 수신(배치) 카운트 - SC_MAP_CHANGE와 동일 의미(로그인 버전)
        if (payloadLen != 28) { ::InterlockedIncrement64(&s_recvParseErrors); break; }   // playerId4+mapId4+x4+y4+hp4+maxHp4+mp4
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_CHAR_INFO body{};
        body.Deserialize(m_parsePkt);
        ::AcquireSRWLockExclusive(&m_stateLock);
        m_x = body.x;
        m_y = body.y;
        m_mapId = body.mapId;
        m_selfPlayerId = body.playerId;
        m_monsterCount = 0;   // 채널 변경 재수신 시 옛 채널 몬스터 추적 비움 (로그인 땐 이미 0이라 무해)
        m_groundItemCount = 0;   // 옛 채널 바닥 드랍 추적도 비움
        ::ReleaseSRWLockExclusive(&m_stateLock);
        ::InterlockedExchange(&m_placed, 1);   // 자기위치 수신 완료 - 이제 봇 행동 시작(CS_MOVE 등)
        break;
    }
    case PacketType::SC_MONSTER_SPAWN:   // 몬스터 시야 등장 - 스킬 타겟 추적
    {
        ::InterlockedIncrement64(&s_recvMonsterSpawns);
        if (payloadLen != 33) { ::InterlockedIncrement64(&s_recvParseErrors); break; }   // id4+ms1+dir1+x4+y4+hp4+maxHp4+objType1+monsterType1+atkRange4+moveSpeed4+isBoss1
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_MONSTER_SPAWN body{};
        body.Deserialize(m_parsePkt);
        ::AcquireSRWLockExclusive(&m_stateLock);
        TrackMonster_NoLock(body.monsterId, body.x, body.y);
        ::ReleaseSRWLockExclusive(&m_stateLock);
        break;
    }
    case PacketType::SC_MONSTER_MOVE:   // 몬스터 이동 - 추적 위치 갱신
    {
        ::InterlockedIncrement64(&s_recvMonsterMove);
        if (payloadLen != 14) { ::InterlockedIncrement64(&s_recvParseErrors); break; }   // id4+ms1+dir1+x4+y4
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_MONSTER_MOVE body{};
        body.Deserialize(m_parsePkt);
        if (static_cast<unsigned int>(body.direction) >= static_cast<unsigned int>(EMoveDirection::COUNT))
            ::InterlockedIncrement64(&s_recvContentErrors);   // 서버는 0..7 만 보냄 - 밖이면 content 오염 (NE-6)
        ::AcquireSRWLockExclusive(&m_stateLock);
        TrackMonster_NoLock(body.monsterId, body.x, body.y);
        ::ReleaseSRWLockExclusive(&m_stateLock);
        break;
    }
    case PacketType::SC_DESPAWN:   // 시야 이탈 - 몬스터면 추적 제거(playerId 필드에 공용 id)
    {
        ::InterlockedIncrement64(&s_recvDespawn);
        if (payloadLen != 4) { ::InterlockedIncrement64(&s_recvParseErrors); break; }
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_DESPAWN body{};
        body.Deserialize(m_parsePkt);
        ::AcquireSRWLockExclusive(&m_stateLock);
        UntrackMonster_NoLock(body.playerId);      // 몬스터/플레이어 추적 제거
        UntrackGroundItem_NoLock(body.playerId);   // 바닥 드랍도 SC_DESPAWN 공용 - 줍힘/만료 시 제거
        ::ReleaseSRWLockExclusive(&m_stateLock);
        break;
    }
    case PacketType::SC_DEATH:   // 사망 - 추적 제거
    {
        ::InterlockedIncrement64(&s_recvDeath);
        if (payloadLen != 4) { ::InterlockedIncrement64(&s_recvParseErrors); break; }
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_DEATH body{};
        body.Deserialize(m_parsePkt);
        ::AcquireSRWLockExclusive(&m_stateLock);
        UntrackMonster_NoLock(body.targetId);
        ::ReleaseSRWLockExclusive(&m_stateLock);
        break;
    }
    case PacketType::SC_MOVE_BROADCAST:   // 이동 알림 - self-echo면 서버 권위 위치로 snap (dead-reckon reconcile)
    {
        ::InterlockedIncrement64(&s_recvMoveBroadcast);
        if (payloadLen != 18) { ::InterlockedIncrement64(&s_recvParseErrors); break; }   // id4+ms1+dir1+x4+y4+lastSeq4
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_MOVE_BROADCAST body{};
        body.Deserialize(m_parsePkt);
        if (static_cast<unsigned int>(body.direction) >= static_cast<unsigned int>(EMoveDirection::COUNT))
            ::InterlockedIncrement64(&s_recvContentErrors);   // 서버는 0..7 만 보냄 - 밖이면 content 오염 (NE-6)
        // 내 echo면 내 위치를 서버 권위값으로 맞춘다 - 방향 반전 등으로 서버가 보정해도 다시 동기화돼
        //   다음 보고가 tolerance 내(accept 경로 유지). m_selfPlayerId 는 SC_MAP_CHANGE 가 채움.
        ::AcquireSRWLockExclusive(&m_stateLock);
        if (m_selfPlayerId != 0 && body.playerId == m_selfPlayerId)
        {
            m_x = body.x; m_y = body.y;
            if (s_harness.stopMove && m_h1Stopped != 0)
            {
                // H1: STOP 후엔 서버 권위 위치가 의도 STOP 좌표와 같아야 정상. 128px 초과 벌어지면
                //   STOP 이 movement 레인서 shed 되어 서버가 계속 dead-reckon 한 표류(버그).
                const long long ddx = static_cast<long long>(body.x) - m_h1StopX;
                const long long ddy = static_cast<long long>(body.y) - m_h1StopY;
                if (ddx * ddx + ddy * ddy > static_cast<long long>(H1_DRIFT_THRESHOLD) * H1_DRIFT_THRESHOLD)
                    ::InterlockedIncrement64(&s_h1Divergence);
            }
        }
        ::ReleaseSRWLockExclusive(&m_stateLock);
        break;
    }
    case PacketType::SC_SPAWN:   // 타 플레이어 시야 등장 - 카운트 + content 정합 검사 (framing 은 정상인데 내용이 오염된 경우 탐지)
    {
        ::InterlockedIncrement64(&s_recvSpawn);
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_SPAWN body{};
        body.Deserialize(m_parsePkt);   // CPacket Read 는 런타임 경계검사라 짧은 payload 여도 over-read 안전
        // 서버는 id 0 이나 맵 밖 좌표로 spawn 하지 않는다 - 그런 값이 오면 배치 교차오염/삼켜진 UAF 의심.
        //   맵 크기는 맵마다 다르므로 내 맵 기준으로 본다 (spawn 은 내 맵 엔티티만 온다).
        bool bad = (body.playerId == 0);
        int mapW = 0, mapH = 0;
        ::AcquireSRWLockExclusive(&m_stateLock);
        if (m_selfPlayerId != 0 && body.playerId == m_selfPlayerId) { bad = true; }   // 나 자신의 spawn 이 나에게 오는 것도 이상
        mapW = MapWidthFor(m_mapId);
        mapH = MapHeightFor(m_mapId);
        ::ReleaseSRWLockExclusive(&m_stateLock);
        if (body.x < 0 || body.x > mapW || body.y < 0 || body.y > mapH) { bad = true; }
        if (bad) { ::InterlockedIncrement64(&s_recvContentErrors); }
        break;
    }
    case PacketType::SC_GROUND_ITEM_SPAWN:   // 바닥 드랍 시야 등장 - 줍기 대상으로 추적
    {
        ::InterlockedIncrement64(&s_recvGroundItemSpawn);
        if (payloadLen != 20) { ::InterlockedIncrement64(&s_recvParseErrors); break; }   // objectId4+templateId4+x4+y4+quantity4
        m_parsePkt.Reset();
        m_parsePkt.Write(payload, payloadLen);
        SC_GROUND_ITEM_SPAWN body{};
        body.Deserialize(m_parsePkt);
        ::AcquireSRWLockExclusive(&m_stateLock);
        TrackGroundItem_NoLock(body.objectId, body.x, body.y);
        ::ReleaseSRWLockExclusive(&m_stateLock);
        break;
    }
    case PacketType::SC_INVENTORY:   // 전량 인벤 스냅샷 (입장 + 줍기/거래 성공 후) - 성공 줍기 근사 카운트
        ::InterlockedIncrement64(&s_recvInventory);
        break;
    // 아래는 도달/정합 확인용 카운트만 (필드 소비 불요)
    case PacketType::SC_DAMAGE:         ::InterlockedIncrement64(&s_recvDamage);         break;
    case PacketType::SC_RESPAWN:        ::InterlockedIncrement64(&s_recvRespawn);        break;
    case PacketType::SC_CHAT_BROADCAST: ::InterlockedIncrement64(&s_recvChat);           break;
    case PacketType::SC_WHISPER:        ::InterlockedIncrement64(&s_recvWhisper);        break;
    case PacketType::SC_WHISPER_FAIL:   ::InterlockedIncrement64(&s_recvWhisperFail);    break;   // 오프라인/도배 실패 통지 수신 (B4 H7)
    case PacketType::SC_CHANNEL_CHANGE_RESULT:   // 채널 변경 회신 - 총계 + 결과 코드별 분리 (실제 이동 vs 거부 구분)
    {
        ::InterlockedIncrement64(&s_recvChannelChange);
        if (payloadLen >= 1)
        {
            m_parsePkt.Reset();
            m_parsePkt.Write(payload, payloadLen);
            SC_CHANNEL_CHANGE_RESULT body{};
            body.Deserialize(m_parsePkt);
            switch (static_cast<EChannelChangeResult>(body.result))
            {
            case EChannelChangeResult::OK:          ::InterlockedIncrement64(&s_ccOk);         break;
            case EChannelChangeResult::SAME:        ::InterlockedIncrement64(&s_ccSame);       break;
            case EChannelChangeResult::FULL:        ::InterlockedIncrement64(&s_ccFull);       break;
            case EChannelChangeResult::INVALID:     ::InterlockedIncrement64(&s_ccInvalid);    break;
            case EChannelChangeResult::NOT_IN_GAME: ::InterlockedIncrement64(&s_ccNotInGame); break;
            case EChannelChangeResult::RATE_LIMITED: ::InterlockedIncrement64(&s_ccRateLimited); break;   // 쿨다운 거부 통지 (default 삼킴 방지 - 거부 vs 이동 구분)
            default: break;
            }
        }
        else { ::InterlockedIncrement64(&s_recvParseErrors); }
        break;
    }
    default: break;
    }
}

void DummySession::OnSendComplete(int transferred)
{
    (void)transferred;   // 로그인 송신 완료 - 추가 처리 없음. (행동 송신 완료는 OnActionSendComplete.)
}

// TimerProc -> 봇 수명 기반 reconnect churn (슬롯 재사용 재현). 단일 타이머 스레드 호출.
void DummySession::TickReconnect(HANDLE iocp, const wchar_t* ip, unsigned short port, bool churnEnabled)
{
    if (!churnEnabled)
    {
        return;   // churn OFF(입력 구동): 봇은 1회 연결 후 상주
    }
    if (m_sock != INVALID_SOCKET)
    {
        if (++m_aliveTicks >= RECONNECT_LIFETIME_TICKS)
        {
            Close();
            m_reconnectCountdown = RECONNECT_DELAY_TICKS;
        }
        return;
    }
    if (m_reconnectCountdown > 0)
    {
        --m_reconnectCountdown;
        return;
    }
    // churn 재연결도 2단계: 토큰은 일회용이라 매 재접속마다 Stage 1으로 새 토큰을 받아야 한다(옛 토큰 재사용=verify NOT_FOUND).
    //   m_badCred 유지 전달 - h12+churn 조합에서 negative control 봇이 재접속 시 정상 봇으로 바뀌지 않게.
    if (!AcquireToken(ip, LOGIN_CLIENT_PORT, m_accountIndex, m_badCred)
        || !Connect(ip, port) || !AssociateAndStart(iocp, m_botIndex))
    {
        Close();
        m_reconnectCountdown = RECONNECT_DELAY_TICKS;
    }
}

void DummySession::PostAction(HANDLE iocp, UINT64 nowMs, int peerBotCount)
{
    if (m_sock == INVALID_SOCKET)
        return;

    ::InterlockedExchange(&m_peerBotCount, static_cast<LONG>(peerBotCount));   // 귓속말 대상 범위 (게이트 전 항상 갱신)

    // 위상 jitter 게이트: 봇별 다음 행동 시각 전이면 skip (thundering herd 회피).
    if (nowMs < m_nextActionMs)
        return;

    if (::InterlockedExchange(&m_actionPending, 1) == 1)
        return;   // 이미 사이클 진행 중 -> 중복 게시 안 함

    // 다음 송신 예약: 상대 증분(공유 now 절대대입 회피) - 위상 offset 보존.
    //   H2 hotspot ON 이면 고빈도 이동으로 shed 압력을 키운다(off 면 오늘과 동일한 167ms).
    const int actionInterval = s_harness.hotspot ? HOTSPOT_MOVE_INTERVAL_MS : BOT_MOVE_INTERVAL_MS;
    m_nextActionMs += static_cast<UINT64>(actionInterval);
    while (m_nextActionMs <= nowMs) { m_nextActionMs += static_cast<UINT64>(actionInterval); }

    m_actionCtx.m_op = EBotOp::ACTION;
    ::ZeroMemory(&m_actionCtx.m_overlapped, sizeof(OVERLAPPED));
    if (!::PostQueuedCompletionStatus(iocp, 0, reinterpret_cast<ULONG_PTR>(this), &m_actionCtx.m_overlapped))
    {
        ::InterlockedExchange(&m_actionPending, 0);   // 게시 실패 -> 가드 해제
    }
}

// 워커 스레드 -> 봇 행동 결정 + 송신. (m_actionPending 가드로 같은 봇 행동 동시 진입 0.)
//   공유 상태(위치, 몬스터) 읽기/외삽은 m_stateLock 안에서, WSASend 는 락 밖에서(임계구역 짧게).
void DummySession::OnActionTick()
{
    if (m_sock == INVALID_SOCKET)
    {
        ::InterlockedExchange(&m_actionPending, 0);
        return;
    }
    // 인증 + 자기위치(SC_MAP_CHANGE) 수신 전엔 행동 안 함 -> 첫 CS_MOVE 가 서버 권위 위치서 출발(수용 경로 행사).
    if (m_authenticated == 0 || m_placed == 0)
    {
        ::InterlockedExchange(&m_actionPending, 0);
        return;
    }

    // H10 (검증 하니스): 인증/배치 후 완전 침묵 - CS_PING 조차 안 보내 서버 무수신 sweep(idle-kick)을 유발한다.
    //   이동만 끄면 재현이 안 된다(서버가 처리 패킷마다 활동시각 재스탬프). off(기본)면 아래로 지나쳐
    //   NextRandom 소비 0 - 오늘과 동일 실행(기준선 보존). 소켓은 recv 게시 상태로 열어 둔다(Close 아님).
    if (s_harness.muteIdle)
    {
        ::InterlockedExchange(&m_actionPending, 0);
        return;
    }

    const UINT64 now = ::GetTickCount64();

    // keepalive: 조용한 인게임 구간에도 주기적 CS_PING 으로 서버 idle sweep(무수신 30s -> 좀비 정리) 회피.
    //   서버는 처리한 패킷마다 세션 활동시각을 갱신하는데, CS_PING 은 이동과 달리 과부하 shed 대상이 아니라(critical lane)
    //   포화 상황에도 확실히 도달해 활동시각을 살린다. 행동 1개/tick 슬롯을 쓰므로 due 면 이번 tick 은 ping 만 보내고
    //   반환한다(다음 tick 에 이동/전투 재개 - 10s 마다 한 번뿐이라 이동량 영향은 무시할 수준).
    if (now >= m_nextPingMs)
    {
        m_nextPingMs = now + KEEPALIVE_INTERVAL_MS;
        if (!BuildAndSendPing())
        {
            // armed send 실패(WSASend 실제 에러) = send 키만 advance 되고 패킷 미전송 -> 서버 recv 키와 영구 desync.
            //   recv 실패/kick 과 동일한 teardown(Close)으로 연결을 내려 TickReconnect 가 재접속(재접속이 m_obfArmed=false + 다음 arm 재seed).
            Close();
            ::InterlockedExchange(&m_actionPending, 0);   // 송신 실패 -> 완료 통지 안 옴 -> 여기서 가드 해제
        }
        return;
    }

    // 검증 하니스 행동 분기 (ping 이후, 정상 결정 이전). off 면 통과 -> 오늘과 동일(난수 미소비).
    if (s_harness.stopMove)     { OnActionTickH1Stop(now);    return; }   // H1
    if (s_harness.bogusWhisper) { OnActionTickH7Whisper(now); return; }   // H7
    if (s_harness.rawInject)    { OnActionTickH3Inject(now);  return; }   // H3

    const int peers = static_cast<int>(m_peerBotCount);

    int decision = 0;   // 0=move 1=skill 2=portal 3=chat 4=whisper 5=channel-change 6=pickup
    int sendX = 0, sendY = 0;
    int chosenPortalId = -1;
    int whisperTarget = -1;
    int chosenChannel = -1;
    UINT32 pickupObjectId = 0;

    ::AcquireSRWLockExclusive(&m_stateLock);

    int monX = 0, monY = 0;
    const bool haveMon = FindNearestMonster_NoLock(monX, monY);
    bool monInRange = false;
    if (haveMon)
    {
        const long long dx = static_cast<long long>(monX) - m_x;
        const long long dy = static_cast<long long>(monY) - m_y;
        // 벽 너머 타격 금지 - 서버 LoS 게이트와 대칭. 막히면 monInRange=false -> 접근(steer) 분기로 폴백(직선 접근이라
        //   벽에 막히면 그 앞에서 멈춤 - 봇엔 우회 길찾기 없음. 우회는 서버 몬스터 몫이라 몬스터가 봇 쪽으로 온다).
        monInRange = (dx * dx + dy * dy) <= static_cast<long long>(SKILL_HIT_RANGE) * SKILL_HIT_RANGE
                     && HasLineOfSight(m_mapId, Position{ m_x, m_y }, Position{ monX, monY });
    }

    // 근처 바닥 드랍 - 줍기 사거리 안이면 최우선(전투보다 먼저 loot 회수, 드랍-줍기 순환 e2e 행사).
    int giX = 0, giY = 0;
    UINT32 giId = 0;
    const bool haveItem = FindNearestGroundItem_NoLock(giX, giY, giId);
    bool itemInRange = false;
    if (haveItem)
    {
        const long long dx = static_cast<long long>(giX) - m_x;
        const long long dy = static_cast<long long>(giY) - m_y;
        itemInRange = (dx * dx + dy * dy) <= static_cast<long long>(PICKUP_RANGE) * PICKUP_RANGE;
    }

    int probeWallX = 0, probeWallY = 0;
    if (s_harness.wallProbe && m_botIndex == 0 && FindNearestWallSpot_NoLock(probeWallX, probeWallY))
    {
        // [HW 벽 게이트 프로브] 봇0 은 다른 행동을 전부 스킵하고 가장 가까운 벽 칸으로 직진한다 -
        //   AdvancePosition 의 hw 분기가 벽을 무시하고 전진하므로 벽 안 좌표가 그대로 보고되고,
        //   서버 벽 게이트가 보정(wallgate 로그)하는지 결정적으로 관측한다.
        decision = 0;
        SteerToward_NoLock(probeWallX, probeWallY);
        AdvancePosition_NoLock();
        sendX = m_x; sendY = m_y;
    }
    else if (haveItem && itemInRange && now >= m_nextPickupMs)
    {
        decision = 6;                  // 줍기: 사거리 내 최근접 드랍
        pickupObjectId = giId;
        m_nextPickupMs = now + 300;    // 줍기 페이싱 (연타 방지)
    }
    else if (haveMon && monInRange && now >= m_nextSkillMs)
    {
        decision = 1;                  // 스킬: 사거리 내 몬스터 타격 (제자리)
        m_nextSkillMs = now + 600;     // 서버 쿨다운 0.5s 보다 약간 길게
    }
    else if (haveMon)
    {
        decision = 0;                  // 이동: 몬스터로 접근
        SteerToward_NoLock(monX, monY);
        AdvancePosition_NoLock();
        sendX = m_x; sendY = m_y;
    }
    else
    {
        // 목표 맵에 도착했으면 새 목표를 뽑는다 (전 맵을 고르게 행사하도록 한 방향 전진).
        //   현재 맵은 제외하고 뽑아 항상 다른 방향으로 전진 - 같은 맵을 다시 뽑으면 한 칸 왕복하는 잔진동이 생김.
        while (m_mapId == m_targetMap && MapTableCount() > 1)
        {
            m_targetMap = static_cast<int>(NextBounded(static_cast<UINT32>(MapTableCount())));   // 실제 맵 수(maps.csv)로 추첨 - 없는 맵을 목표로 잡으면 도달 불가 배회
        }

        // 목표 맵 방향 포탈을 고른다. 선형 체인이라 트리거 x로 방향 판별:
        //   오른쪽 끝(x3800) 포탈 = 다음 맵(map++) / 왼쪽 끝(x200) 포탈 = 이전 맵(map--).
        //   목표가 내 맵보다 크면 다음 맵 포탈만, 작으면 이전 맵 포탈만 후보 -> 핑퐁(왔던 포탈로 회귀) 차단.
        //   방향 포탈은 선형 체인서 맵당 1개라 최근접 비교 불요(있으면 그게 유일 후보).
        //   [!] 트리거가 맵 좌우 끝(x200/x3800)에만 있다는 전제 - 중앙(x2000) 근처 포탈이 생기면 방향 판별이 깨짐.
        int trigX = 0, trigY = 0, pid = -1;
        bool havePortal = false, inPortalRadius = false;
        const bool wantNext = (m_targetMap > m_mapId);   // true=다음 맵 방향, false=이전 맵 방향
        for (int i = 0; i < PortalTableCount(); ++i)   // 공유 포탈 표 (portals.csv - 서버와 같은 파일·portalId = 행 순서)
        {
            const PortalDef& portal = PortalTableAt(i);
            if (portal.srcMapId != m_mapId) { continue; }
            const bool portalGoesNext = (portal.trigger.x >= (MapWidthFor(m_mapId) / 2));   // 오른쪽(맵 절반 너머) 트리거 = 다음 맵 (맵마다 폭이 다름 - 상한 상수로 나누면 좁은 맵에서 방향 오판)
            if (portalGoesNext != wantNext) { continue; }   // 목표 방향이 아닌 포탈은 건너뜀
            trigX = portal.trigger.x;
            trigY = portal.trigger.y;
            pid = i;
            const long long dx = static_cast<long long>(trigX) - m_x;
            const long long dy = static_cast<long long>(trigY) - m_y;
            const long long r = portal.triggerRadius;
            inPortalRadius = (dx * dx + dy * dy <= r * r);
            havePortal = true;
            break;   // 방향 포탈은 맵당 1개
        }

        const UINT32 roll = NextBounded(100);
        if (s_harness.b3repro && peers > 1 && m_channelCount > 1)
        {
            // [B3 재현 하니스] PostToChannel TOCTOU 재현: 실peer 귓속말 매 틱 + 채널홉 0.4s 로 서버 창(#ifdef _DEBUG 확대)에 대상 이관을 겹친다.
            if (now >= m_nextChannelChangeMs)
            {
                decision = 5; m_nextChannelChangeMs = now + 400;   // 타이트 채널홉 (기본 8s -> 0.4s)
                chosenChannel = static_cast<int>(NextBounded(static_cast<UINT32>(m_channelCount)));
            }
            else
            {
                decision = 4;   // 매 틱 실peer 귓속말 (bot%05d - ResolveName 이 찾는 실제 대상)
                whisperTarget = static_cast<int>(NextBounded(static_cast<UINT32>(peers)));
                if (whisperTarget == m_botIndex) { whisperTarget = (whisperTarget + 1) % peers; }
            }
        }
        else if (now >= m_nextChatMs && roll < 5)
        {
            decision = 3; m_nextChatMs = now + 2000;          // 드물게 채팅 (제자리)
        }
        else if (peers > 1 && now >= m_nextWhisperMs && roll >= 5 && roll < 10)
        {
            decision = 4; m_nextWhisperMs = now + 3000;       // 드물게 귓속말
            whisperTarget = static_cast<int>(NextBounded(static_cast<UINT32>(peers)));
            if (whisperTarget == m_botIndex) { whisperTarget = (whisperTarget + 1) % peers; }
        }
        else if (m_channelCount > 1 && now >= m_nextChannelChangeMs && roll >= 10 && roll < 11)
        {
            decision = 5; m_nextChannelChangeMs = now + 8000;   // 드물게 채널 변경 (cross-thread 마이그레이션 부하 검증)
            // H2 hotspot 이면 채널0 고정(이미 0이면 서버 SAME 회신) - 랜덤 이탈로 채널0 집중이 침식되지 않게.
            //   off 면 오늘과 동일하게 NextBounded 로 랜덤 채널(같으면 SAME 무해).
            chosenChannel = s_harness.hotspot ? 0 : static_cast<int>(NextBounded(static_cast<UINT32>(m_channelCount)));
        }
        else if (havePortal && inPortalRadius && now >= m_nextPortalMs)
        {
            decision = 2; chosenPortalId = pid; m_nextPortalMs = now + 5000;   // 트리거 반경 내 -> 포탈
        }
        else
        {
            decision = 0;   // 이동
            if (havePortal && (NextBounded(100)) < 30)
            {
                SteerToward_NoLock(trigX, trigY);             // 포탈 트리거로 표류 (e2e 도달)
            }
            else if (s_harness.hotspot)
            {
                SteerToward_NoLock(HOTSPOT_X, HOTSPOT_Y);     // H2: worst-case 맵 중앙 수렴
            }
            else if (--m_stepsUntilTurn <= 0)
            {
                m_dir = static_cast<EMoveDirection>(NextBounded(8));   // random-walk 방향 재선택 (8방)
                m_stepsUntilTurn = 10 + static_cast<int>(NextBounded(20));
            }
            AdvancePosition_NoLock();
            sendX = m_x; sendY = m_y;
        }
    }

    ::ReleaseSRWLockExclusive(&m_stateLock);

    // 송신 (락 밖) - 행동 1개. 실패 시 완료 통지가 안 오므로 여기서 가드 해제.
    bool ok = true;
    switch (decision)
    {
    case 1:  ok = BuildAndSendSkill();              break;
    case 2:  ok = BuildAndSendPortal(chosenPortalId); break;
    case 3:  ok = BuildAndSendChat();               break;
    case 4:  ok = (whisperTarget >= 0) ? BuildAndSendWhisper(whisperTarget) : BuildAndSendMove(sendX, sendY); break;
    case 5:  ok = (chosenChannel >= 0) ? BuildAndSendChannelChange(static_cast<BYTE>(chosenChannel)) : BuildAndSendMove(sendX, sendY); break;
    case 6:  ok = (pickupObjectId != 0) ? BuildAndSendPickup(pickupObjectId) : BuildAndSendMove(sendX, sendY); break;
    default: ok = BuildAndSendMove(sendX, sendY);   break;
    }
    if (!ok)
    {
        // armed send 실패(WSASend 실제 에러) = send 키만 advance 되고 패킷 미전송 -> 서버 recv 키와 영구 desync.
        //   recv 실패/kick 과 동일한 teardown(Close)으로 연결을 내려 TickReconnect 가 재접속(재접속이 m_obfArmed=false + 다음 arm 재seed).
        Close();
        ::InterlockedExchange(&m_actionPending, 0);
    }
}

void DummySession::OnActionSendComplete(int transferred)
{
    (void)transferred;
    ::InterlockedExchange(&m_actionPending, 0);   // 행동 사이클 종료 -> 다음 타이머 틱이 ACTION 게시 가능
}

// H1 (검증 하니스): 먼저 워밍업으로 몇 tick 이동(START)해 서버에 이동 상태(속도)를 심는다 - idle/STOP 상태
//   플레이어는 서버가 dead-reckon 하지 않으므로, START 없이 STOP 만 보내면 표류가 아예 안 생겨 재현이 실패한다.
//   워밍업 후 CS_MOVE{STOP} 1회 -> 이후 주기 프로브 재전송. 프로브 사이 침묵 동안 STOP 이 movement 레인서 shed
//   되면 서버는 마지막 START 로 계속 dead-reckon 하고, 재전송된 프로브의 self-echo 가 표류한 서버 위치를 실어
//   온다(HandlePacket SC_MOVE_BROADCAST 에서 s_h1Divergence 집계). 수정 후엔 STOP 이 critical 레인으로
//   재분류돼 확실히 도달 -> 표류 소멸.
//   전제: 과부하(H2 병행)라야 STOP 이 실제로 shed 된다. 프로브가 100% shed 되면 self-echo 가 아예 안 와 미탐
//   가능하므로, H2 는 shed 하되 굶기지는 않는 부하로 켠다(h1 h2 함께).
void DummySession::OnActionTickH1Stop(UINT64 now)
{
    int px, py;
    bool isWarmup = false;
    bool firstStopSend = false;
    ::AcquireSRWLockExclusive(&m_stateLock);
    if (m_h1MoveTicks < H1_WARMUP_MOVE_TICKS)
    {
        // Phase A (워밍업): 맵 중앙으로 전진(START). 결정론(난수 미소비)이라 기준선 무관. 서버에 이동 상태를 심는다.
        m_h1MoveTicks++;
        SteerToward_NoLock(HOTSPOT_X, HOTSPOT_Y);
        AdvancePosition_NoLock();
        px = m_x;
        py = m_y;
        isWarmup = true;
    }
    else
    {
        // Phase B/C: 워밍업 끝난 위치에서 STOP -> 이후 그 좌표로 프로브 재전송.
        if (m_h1Stopped == 0)
        {
            m_h1Stopped = 1;
            m_h1StopX = m_x;   // 워밍업 종료 위치를 의도 STOP 좌표로 고정
            m_h1StopY = m_y;
            firstStopSend = true;
        }
        px = m_h1StopX;
        py = m_h1StopY;
    }
    ::ReleaseSRWLockExclusive(&m_stateLock);

    // 워밍업 이동은 매 tick, STOP 프로브는 페이싱(첫 STOP 은 즉시).
    if (!isWarmup && !firstStopSend && now < m_h1NextProbeMs)
    {
        ::InterlockedExchange(&m_actionPending, 0);   // 프로브 간 침묵 - 이 tick 은 아무것도 안 보냄 (다음 타이머 틱이 재게시)
        return;
    }
    if (!isWarmup)
    {
        m_h1NextProbeMs = now + H1_PROBE_INTERVAL_MS;
    }

    const bool ok = isWarmup ? BuildAndSendMove(px, py) : BuildAndSendMoveStop(px, py);
    if (!ok)
    {
        Close();
        ::InterlockedExchange(&m_actionPending, 0);
    }
}

// H7 (검증 하니스): 3단계 사이클로 귓속말 실패 2경로를 행사한다. 오늘 서버는 두 경로 모두 발신자에게 통지 0(거짓 성공).
//   0=채팅 -> 1=직후(500ms 내) 귓속말(채팅 rate-limit 트립) -> 2=없는 이름 귓속말(ResolveName 0 offline).
//   B4 가 SC_WHISPER_FAIL 통지를 신설하면, 그 수신 카운터로 "수정 후 실패가 발신자에게 도달"을 관측한다(B1 후 배선).
void DummySession::OnActionTickH7Whisper(UINT64 now)
{
    if (now < m_h7NextMs)
    {
        ::InterlockedExchange(&m_actionPending, 0);   // 사이클 대기 - 이 tick 은 아무것도 안 보냄
        return;
    }
    const int step = m_h7Step;
    m_h7Step = (m_h7Step + 1) % 3;

    bool ok = true;
    wchar_t targetName[WHISPER_NAME_MAX];
    if (step == 0)
    {
        m_h7NextMs = now + 33;   // 곧바로(500ms 내) 귓속말로 이어 rate-limit 유발
        ok = BuildAndSendChat();
    }
    else if (step == 1)
    {
        m_h7NextMs = now + H7_CYCLE_MS;
        ::swprintf_s(targetName, WHISPER_NAME_MAX, L"bot%05d", m_accountIndex);   // 자기 이름 - CanChat 게이트가 resolve 전에 rate-limit 로 drop
        ok = BuildAndSendWhisperName(targetName);
    }
    else
    {
        m_h7NextMs = now + H7_CYCLE_MS;
        ::swprintf_s(targetName, WHISPER_NAME_MAX, L"ghost%05d", m_accountIndex);   // 존재하지 않는 이름 - ResolveName 0 -> 조용히 drop
        ok = BuildAndSendWhisperName(targetName);
    }
    if (!ok)
    {
        Close();
        ::InterlockedExchange(&m_actionPending, 0);
    }
}

// H3 (검증 하니스): admission(배치) 후 raw 절단 프레임을 1회 주입한다. 서버가 PROTOCOL_VIOLATION 으로 끊으면
//   봇 연결이 죽고, churn 이 켜져 있으면 재접속 후 다시 주입한다. RST/FIN 구분은 Wireshark/netstat 로 관측(B0 flip).
void DummySession::OnActionTickH3Inject(UINT64 now)
{
    (void)now;
    if (m_h3Injected != 0)
    {
        ::InterlockedExchange(&m_actionPending, 0);   // 이미 주입함 - 서버가 곧 끊는다. 추가 송신 안 함(재접속이 재주입)
        return;
    }
    m_h3Injected = 1;
    ::InterlockedIncrement64(&s_h3Injected);
    if (!BuildAndSendRawMalformed())
    {
        Close();
        ::InterlockedExchange(&m_actionPending, 0);
    }
}

// 공용 행동 송신: 직렬화된 패킷을 m_actionSendBuf 로 복사 후 WSASend(op=ACTION_SEND). 게시 성공 시 true.
bool DummySession::SendBuilt(KYS::GAMECOMMON::PROTOCOL::CPacket& out)
{
    const int size = out.GetSize();
    if (size <= 0 || size > MAX_PACKET_SIZE)
        return false;

    ::memcpy(m_actionSendBuf, out.GetBuffer(), static_cast<size_t>(size));
    if (m_obfArmed && size >= static_cast<int>(sizeof(PacketHeader)))
        KYS::GAMECOMMON::PROTOCOL::ObfuscateOpcode(m_actionSendBuf, m_sendKey);   // opcode scramble + send 키 advance (송신 직전)
    m_actionSendCtx.m_op = EBotOp::ACTION_SEND;
    ::ZeroMemory(&m_actionSendCtx.m_overlapped, sizeof(OVERLAPPED));
    m_actionSendCtx.m_wsaBuf.buf = reinterpret_cast<char*>(m_actionSendBuf);
    m_actionSendCtx.m_wsaBuf.len = static_cast<ULONG>(size);

    DWORD sent = 0;
    int ret = ::WSASend(m_sock, &m_actionSendCtx.m_wsaBuf, 1, &sent, 0, &m_actionSendCtx.m_overlapped, NULL);
    if (ret == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
        return false;
    return true;
}

// SC_ENTER_WORLD(admission 성공)을 평문으로 받은 직후 호출 - 방향별 키를 심고 무장한다 (서버 SeedObfKeys 와 방향 대칭).
void DummySession::ArmObfuscation()
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    if (m_badCred) { ::InterlockedIncrement64(&s_spuriousArm); }   // H12: 거부돼야 할 봇에 arm 발동 = arm 트리거 회귀 (정상 0)
    InitObfKey(m_sendKey, m_token, ObfDirection::CLIENT_TO_SERVER);   // 내가 보내는 방향 (서버 recv 와 일치)
    InitObfKey(m_recvKey, m_token, ObfDirection::SERVER_TO_CLIENT);   // 내가 받는 방향 (서버 send 와 일치)
    m_obfArmed = true;
}

bool DummySession::BuildAndSendMove(int x, int y)
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_MOVE));
    CS_MOVE req{};
    req.moveState = EMoveState::START;   // 항상 이동 (정지 미모델)
    req.direction = m_dir;
    req.x = x;
    req.y = y;
    req.clientSeq = ++m_seq;
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }
    return SendBuilt(out);
}

// H1 (검증 하니스): CS_MOVE{STOP} 프로브. 서버가 이걸 처리하면 dead-reckon 을 멈춰야 정상.
bool DummySession::BuildAndSendMoveStop(int x, int y)
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_MOVE));
    CS_MOVE req{};
    req.moveState = EMoveState::STOP;   // 정지 보고 (movement 레인서 shed 되면 서버가 계속 외삽 = 버그)
    req.direction = m_dir;
    req.x = x;
    req.y = y;
    req.clientSeq = ++m_seq;
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }
    return SendBuilt(out);
}

bool DummySession::BuildAndSendSkill()
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_SKILL));
    CS_SKILL req{};
    req.skillId = 0;   // 0 = 기본 공격
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }
    if (SendBuilt(out)) { ::InterlockedIncrement64(&s_sentSkill); return true; }
    return false;
}

bool DummySession::BuildAndSendPortal(int portalId)
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_PORTAL));
    CS_PORTAL req{};
    req.portalId = portalId;
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }
    if (SendBuilt(out)) { ::InterlockedIncrement64(&s_sentPortal); return true; }
    return false;
}

bool DummySession::BuildAndSendChat()
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_CHAT));
    CS_CHAT req{};
    ::wcscpy_s(req.message, CHAT_MSG_MAX, L"hi");
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }
    if (SendBuilt(out)) { ::InterlockedIncrement64(&s_sentChat); return true; }
    return false;
}

bool DummySession::BuildAndSendWhisper(int targetBotIndex)
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_WHISPER));
    CS_WHISPER req{};
    ::swprintf_s(req.targetName, WHISPER_NAME_MAX, L"bot%05d", targetBotIndex);   // 대상 캐릭명 = 로그인 id
    ::wcscpy_s(req.message, CHAT_MSG_MAX, L"hi");
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }
    if (SendBuilt(out)) { ::InterlockedIncrement64(&s_sentWhisper); return true; }
    return false;
}

// H7 (검증 하니스): 임의 대상 이름으로 귓속말. 없는 이름(offline) / 자기 이름(rate-limit) 실패 경로 행사용.
bool DummySession::BuildAndSendWhisperName(const wchar_t* targetName)
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_WHISPER));
    CS_WHISPER req{};
    ::wcscpy_s(req.targetName, WHISPER_NAME_MAX, targetName);
    ::wcscpy_s(req.message, CHAT_MSG_MAX, L"hi");
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }
    if (SendBuilt(out)) { ::InterlockedIncrement64(&s_sentWhisper); return true; }
    return false;
}

// H3 (검증 하니스): size=3 절단 프레임(00 03 FF)을 raw 로 직송. 서버 Channel::OnRecv 가 size < sizeof(PacketHeader)(4)
//   를 보고 PROTOCOL_VIOLATION 으로 끊는다(B0 전 FIN / B0 후 RST). 라이브러리 framing 은 size in [2,8192] 통과하고,
//   DescrambleRecv 는 len<4 early-return 이라 키 desync 도 없다. 3바이트 < 헤더 4바이트라 난독화도 무관(SendBuilt 우회).
bool DummySession::BuildAndSendRawMalformed()
{
    m_actionSendBuf[0] = 0x00;   // size 상위 (big-endian)
    m_actionSendBuf[1] = 0x03;   // size 하위 = 3 (프레임 전체 길이)
    m_actionSendBuf[2] = 0xFF;   // 절단된 opcode 바이트 1개 (size<4 라 opcode 해석 전에 걸림)
    m_actionSendCtx.m_op = EBotOp::ACTION_SEND;
    ::ZeroMemory(&m_actionSendCtx.m_overlapped, sizeof(OVERLAPPED));
    m_actionSendCtx.m_wsaBuf.buf = reinterpret_cast<char*>(m_actionSendBuf);
    m_actionSendCtx.m_wsaBuf.len = 3;
    DWORD sent = 0;
    int ret = ::WSASend(m_sock, &m_actionSendCtx.m_wsaBuf, 1, &sent, 0, &m_actionSendCtx.m_overlapped, NULL);
    if (ret == SOCKET_ERROR && ::WSAGetLastError() != WSA_IO_PENDING)
        return false;
    return true;
}

bool DummySession::BuildAndSendChannelChange(BYTE targetChannel)
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_CHANNEL_CHANGE));
    CS_CHANNEL_CHANGE req{};
    req.channelId = targetChannel;
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }
    if (SendBuilt(out)) { ::InterlockedIncrement64(&s_sentChannelChange); return true; }
    return false;
}

bool DummySession::BuildAndSendPickup(UINT32 objectId)
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_ITEM_PICKUP));
    CS_ITEM_PICKUP req{};
    req.objectId = objectId;
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }
    if (SendBuilt(out)) { ::InterlockedIncrement64(&s_sentPickup); return true; }
    return false;
}

bool DummySession::BuildAndSendPing()
{
    using namespace KYS::GAMECOMMON::PROTOCOL;
    CPacket out(MAX_PACKET_SIZE);
    out.Begin(static_cast<USHORT>(PacketType::CS_PING));
    CS_PING req{};
    req.clientTimeMs = static_cast<UINT32>(::GetTickCount64());   // RTT 측정용 - 서버 SC_PONG 이 그대로 되돌림
    req.Serialize(out);
    if (!out.End()) { ::InterlockedIncrement64(&s_serializeFail); return false; }
    return SendBuilt(out);
}

// 봇별 LCG (Numerical Recipes 계수) - 전역 rand() 비-thread-safe 회피.
UINT32 DummySession::NextRandom()
{
    m_rngState = m_rngState * 1664525u + 1013904223u;
    return m_rngState;
}

// [0, bound) 균일 난수 - LCG 하위 비트는 주기가 짧아(bit0 = 주기 2) % 로 작은 수를 뽑으면 편향된다.
//   상위 16비트를 써서 편향을 피한다 (Knuth TAOCP: 작은 수엔 상위 비트 사용). 호출부 bound < 65536.
UINT32 DummySession::NextBounded(UINT32 bound)
{
    return (NextRandom() >> 16) % bound;
}

// --- 몬스터 추적 (m_stateLock 보유 상태에서 호출) ---
void DummySession::TrackMonster_NoLock(UINT32 id, int x, int y)
{
    for (int i = 0; i < m_monsterCount; ++i)
    {
        if (m_monsters[i].id == id) { m_monsters[i].x = x; m_monsters[i].y = y; return; }   // 있으면 위치 갱신
    }
    if (m_monsterCount < MAX_TRACKED_MONSTERS)
    {
        m_monsters[m_monsterCount].id = id;
        m_monsters[m_monsterCount].x = x;
        m_monsters[m_monsterCount].y = y;
        ++m_monsterCount;
    }
    // 가득 차면 새 몬스터 무시 (e2e 표본엔 충분)
}

void DummySession::UntrackMonster_NoLock(UINT32 id)
{
    for (int i = 0; i < m_monsterCount; ++i)
    {
        if (m_monsters[i].id == id)
        {
            m_monsters[i] = m_monsters[m_monsterCount - 1];   // swap-pop
            --m_monsterCount;
            return;
        }
    }
}

bool DummySession::FindNearestMonster_NoLock(int& outX, int& outY) const
{
    int best = -1;
    long long bestD2 = 0;
    for (int i = 0; i < m_monsterCount; ++i)
    {
        const long long dx = static_cast<long long>(m_monsters[i].x) - m_x;
        const long long dy = static_cast<long long>(m_monsters[i].y) - m_y;
        const long long d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < bestD2) { best = i; bestD2 = d2; }
    }
    if (best < 0) { return false; }
    outX = m_monsters[best].x;
    outY = m_monsters[best].y;
    return true;
}

// --- 바닥 드랍 추적 (몬스터 추적과 같은 계약 - m_stateLock 보유 상태에서 호출) ---
void DummySession::TrackGroundItem_NoLock(UINT32 id, int x, int y)
{
    for (int i = 0; i < m_groundItemCount; ++i)
    {
        if (m_groundItems[i].id == id) { m_groundItems[i].x = x; m_groundItems[i].y = y; return; }   // 있으면 위치 갱신
    }
    if (m_groundItemCount < MAX_TRACKED_GROUND_ITEMS)
    {
        m_groundItems[m_groundItemCount].id = id;
        m_groundItems[m_groundItemCount].x = x;
        m_groundItems[m_groundItemCount].y = y;
        ++m_groundItemCount;
    }
    // 가득 차면 새 드랍 무시 (e2e 표본엔 충분)
}

void DummySession::UntrackGroundItem_NoLock(UINT32 id)
{
    for (int i = 0; i < m_groundItemCount; ++i)
    {
        if (m_groundItems[i].id == id)
        {
            m_groundItems[i] = m_groundItems[m_groundItemCount - 1];   // swap-pop
            --m_groundItemCount;
            return;
        }
    }
}

bool DummySession::FindNearestGroundItem_NoLock(int& outX, int& outY, UINT32& outId) const
{
    int best = -1;
    long long bestD2 = 0;
    for (int i = 0; i < m_groundItemCount; ++i)
    {
        const long long dx = static_cast<long long>(m_groundItems[i].x) - m_x;
        const long long dy = static_cast<long long>(m_groundItems[i].y) - m_y;
        const long long d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < bestD2) { best = i; bestD2 = d2; }
    }
    if (best < 0) { return false; }
    outX = m_groundItems[best].x;
    outY = m_groundItems[best].y;
    outId = m_groundItems[best].id;
    return true;
}

// 자기 맵에서 가장 가까운 벽 칸의 중심 좌표를 찾는다 (HW 벽 게이트 프로브의 직진 목표).
//   80x80 전 칸 스캔 = 6400회 비트 조회 - 167ms 행동 tick 1회당이라 비용 무시 가능. 벽 없는 맵이면 false.
bool DummySession::FindNearestWallSpot_NoLock(int& outX, int& outY) const
{
    const int w = MapWidthFor(m_mapId);
    const int h = MapHeightFor(m_mapId);
    long long bestDistSq = -1;
    for (int y = WALK_CELL_SIZE / 2; y < h; y += WALK_CELL_SIZE)
    {
        for (int x = WALK_CELL_SIZE / 2; x < w; x += WALK_CELL_SIZE)
        {
            if (IsWalkable(m_mapId, x, y)) { continue; }
            const long long dx = static_cast<long long>(x) - m_x;
            const long long dy = static_cast<long long>(y) - m_y;
            const long long distSq = dx * dx + dy * dy;
            if (bestDistSq < 0 || distSq < bestDistSq)
            {
                bestDistSq = distSq;
                outX = x;
                outY = y;
            }
        }
    }
    return bestDistSq >= 0;
}

// --- 항법 (m_stateLock 보유 상태) ---
void DummySession::SteerToward_NoLock(int tx, int ty)
{
    const int dx = tx - m_x;
    const int dy = ty - m_y;
    m_dir = DirectionToward(dx, dy);   // 서버 몬스터 조향과 같은 8방 양자화(예전 지배축 4방 -> 대각 포함 8방)
}

void DummySession::AdvancePosition_NoLock()
{
    if (s_harness.wallProbe && m_botIndex == 0)
    {
        // [HW 벽 게이트 프로브] 봇0 만 통행 격자를 무시하고 옛 수식(경계 클램프만)으로 전진해
        //   벽 너머 좌표를 그대로 보고한다 - 서버 벽 게이트(IsPathWalkable 거부)가 보정하고
        //   wallgate 로그가 발화하는지 관측하는 검증 하니스. self-echo 가 서버 권위로 되돌리므로 발산 없음.
        // AdvanceMove 와 "같은 형태"의 수식(단위벡터 * 속도 * dt 를 int 절단)으로 전진하되 벽 게이트만 우회.
        //   float LUT 를 int step 에 곧장 곱하면 C4244 + 절단치가 공유 수식과 갈리므로, 반드시 이 형태를 따른다(D-M3).
        const float dt = BOT_MOVE_INTERVAL_MS / 1000.0f;
        const int d = static_cast<int>(m_dir);
        int nx = m_x + static_cast<int>(MOVE_DIR_DELTA[d][0] * MAX_MOVE_SPEED * dt);
        int ny = m_y + static_cast<int>(MOVE_DIR_DELTA[d][1] * MAX_MOVE_SPEED * dt);
        const int w = MapWidthFor(m_mapId);
        const int h = MapHeightFor(m_mapId);
        if (nx < 0) { nx = 0; } else if (nx > w) { nx = w; }
        if (ny < 0) { ny = 0; } else if (ny > h) { ny = h; }
        m_x = nx;
        m_y = ny;
        return;
    }

    // 서버/GUI 클라와 같은 공유 함수로 한 걸음 - 같은 벽에서 멈춰야 서버 보정(되당김) 없이 tolerance 안에 머문다.
    //   봇 스텝 = 167ms 에 33px (dt = 간격/1000).
    const Position moved = AdvanceMove(m_mapId, Position{ m_x, m_y }, m_dir, MAX_MOVE_SPEED, BOT_MOVE_INTERVAL_MS / 1000.0f);
    if (moved.x == m_x && moved.y == m_y)
    {
        // 벽/맵 끝에 막혀 제자리 - 방향을 다시 뽑고 짧게 걷는다 (직선 조향뿐인 봇이 벽에 눌려 영구 정체하는 것 방지).
        m_dir = static_cast<EMoveDirection>(NextBounded(8));   // 8방 재추첨
        m_stepsUntilTurn = 3 + static_cast<int>(NextBounded(6));
        return;
    }
    m_x = moved.x;
    m_y = moved.y;
}

void DummySession::Close()
{
    // m_sock read+무효화를 원자화 - churn 시 worker/Logout/TickReconnect 가 동시에 Close 호출 시
    //   둘 다 같은 핸들을 닫던 비원자 check-then-act 제거. 옛 소켓값을 잡은 단 하나의 경로만 closesocket
    //   (재사용 핸들이 다른 봇에 배정된 뒤 오close 방지) - 프로덕션 Session::Disconnect 와 동일 idiom.
    const SOCKET sock = reinterpret_cast<SOCKET>(
        ::InterlockedExchangePointer(reinterpret_cast<volatile PVOID*>(&m_sock), reinterpret_cast<PVOID>(INVALID_SOCKET)));
    if (sock != INVALID_SOCKET)
        ::closesocket(sock);
}

// 송신 직렬화 실패 카운터 - 정의를 파일 끝에 두는 이유: 이 파일 앞쪽 줄 번호를 학습 문서(Docs/fundamentals)가 28 자리 봉인하고 있어 위쪽에 줄을 넣으면 전부 밀린다.
volatile LONG64 DummySession::s_serializeFail = 0;
