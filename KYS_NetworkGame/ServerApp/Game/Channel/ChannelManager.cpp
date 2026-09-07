#include "pch_serverapp.h"
#include "ChannelManager.h"
#include "Jobs.h"
#include "Monitor/ServerMonitor.h"
#include "Network/LoginLinkThread.h"      // pre-auth 토큰검증 enqueue + leave 통지
#include "SharedService/CrossChannelChat.h"   // OwnedDeliveryJob (PostToChannel 이 SetRoutedChannel stamp)
#include "../GameCommon/Protocol/PacketType.h"     // 패킷 lane 라우팅 (CS_MOVE 판별)
#include "../GameCommon/Protocol/PacketHeader.h"   // 헤더 크기 / type 오프셋
#include "../GameCommon/Protocol/GamePackets.h"    // CS_GAME_AUTH (pre-auth 토큰 파싱)
#include "../GameCommon/Protocol/CPacket.h"        // pre-auth payload 역직렬화
#include "../GameServer/Network/IOCP/IOCPServer.h"
#include "../GameServer/Network/IOCP/Session.h"   // KickByAccount의 Session.Disconnect
#include "../GameServer/Core/Log/Logger.h"   // 유저 lifecycle 파일 로그 (로그인/kick/채널이동)
#include "../GameCommon/MapData/MapTable.h"           // MapTableCount (인구 집계 mapId 범위 가드 - 쓰는 맵 수)
#include <cstdlib>                            // _wgetenv_s / _wtoi (B3 재현 하니스 창확대 - #ifdef _DEBUG 만)

namespace
{
    // network byte order IPv4 를 로그용 4옥텟으로 (수동 포맷 - 발화 지점에서 %u.%u.%u.%u 와 함께 사용)
    struct IpOctets { unsigned int a, b, c, d; };
    IpOctets SplitIp(UINT32 ipNetOrder)
    {
        const BYTE* p = reinterpret_cast<const BYTE*>(&ipNetOrder);
        IpOctets o = { p[0], p[1], p[2], p[3] };
        return o;
    }
}


// pre-auth CS_GAME_AUTH 시도 cap 차단 누적 (flood 방어 관측 - GetPreAuthBlockCount로 표출)
volatile LONG KYS::SERVERAPP::ChannelManager::s_preAuthBlockCount = 0;

KYS::SERVERAPP::ChannelManager::ChannelManager()
    : m_gameSessionPool(static_cast<UINT32>(DEFAULT_SESSION_POOL_CAPACITY))   // 게임세션 풀(7000 > MAX_CCU 6000) - m_playerPool 패턴
    , m_playerPool(static_cast<UINT32>(DEFAULT_PLAYER_POOL_CAPACITY))
    , m_channels(nullptr)
    , m_channelCount(0)
    , m_server(nullptr)
{
    for (int i = 0; i < MAX_CHANNEL_COUNT; ++i) { m_channelPlayerCount[i] = 0; }   // 채널별 인구 카운터 0-init
}

KYS::SERVERAPP::ChannelManager::~ChannelManager()
{
    // 게임세션 메모리는 m_gameSessionPool 소멸자가 정리. m_sessionMap은 포인터만 보유(비소유).
}

// 채널 배열/서버 핸들을 주입한다 (게임세션은 풀에서 로그인 시점에 Allocate).
//   maxSessionCount : 시그니처 호환용 (더 이상 배열 크기로 안 쓰임)
//   channels        : 채널 배열 (ChannelManager는 Channel을 new 하지 않는 소비자/디스패처)
//   channelCount    : 채널 수
//   server          : pre-auth reject 송신 + 로그인 시 gs->SetServer 주입용
bool KYS::SERVERAPP::ChannelManager::Init(int maxSessionCount, IChannel** channels, int channelCount, KYS::GAMESERVER::NETWORK::IOCPServer* server)
{
    (void)maxSessionCount;
    m_channels = channels;                               // 보유
    m_channelCount = channelCount;
    m_server = server;
    return true;
}

// 새 연결 통지 - 미인증 pre-auth 라 채널 미소속. 첫 패킷(CS_GAME_AUTH)에서 비로소 검증을 시작한다.
void KYS::SERVERAPP::ChannelManager::EnqueueConnect(UINT64 sid)
{
    (void)sid;   // pre-auth - 잡아둘 게임 상태 없음. 0번 채널(LOGIN_HANDLER) 폐기로 connect job 라우팅 제거.
    // socket 카운트는 SessionPool 활성 슬롯에서 파생 - 별도 카운터 증가 안 함(음수/drift 제거)
}

// 완성 패킷 통지 - 인증 세션이면 소속 채널에 OnRecvJob, 미인증이면 pre-auth 디스패치(CS_GAME_AUTH 토큰 검증).
void KYS::SERVERAPP::ChannelManager::EnqueuePacket(UINT64 sid, const BYTE* data, int len)
{
    const int chId = ResolveChannel(sid);
    if (chId == INVALID_CHANNEL_ID)
    {
        HandlePreAuthPacket(sid, data, len);   // 미인증 - CS_GAME_AUTH면 검증 enqueue, 그 외 drop (채널 라우팅 안 함)
        return;
    }
    // 인증 세션 - Worker 버퍼를 복사한 OnRecvJob을 먼저 만들고, 그 복사본의 opcode를 recv 키로 복호한다.
    //   복호(+recv 키 advance)를 lane drop보다 앞에 두어, 이후 movement lane에서 shed돼도 키가 이미 굴러 클라 send 키와 동기 유지.
    OnRecvJob* job = new OnRecvJob(m_channels[chId], sid, data, len);
    GameSession* gs = GetSession(sid);
    if (gs != nullptr)
    {
        gs->DescrambleRecv(job->PacketBytes(), len);   // 복사본 opcode 평문화 + recv 키 1회 advance (drop 이전, 수신 순서 1회)
    }

    // 평문 opcode로 lane 라우팅: 이동 중(START) CS_MOVE는 droppable(dead-reckoning이 메움) -> movement lane,
    //   그 외(전투/채팅/포탈/종료)는 critical lane(과부하서도 보존). 과부하 시 movement lane이 먼저 shed.
    const int headerSize = static_cast<int>(sizeof(PacketHeader));
    const bool isMove = (len >= headerSize &&
        static_cast<PacketType>(::ntohs(*reinterpret_cast<const UINT16*>(job->PacketBytes() + sizeof(UINT16)))) == PacketType::CS_MOVE);
    // STOP CS_MOVE 만은 critical lane 로 예외: STOP 이 shed 되면 서버가 마지막 START 속도로 무한 dead-reckon 해
    //   맵끝(그 맵의 폭/높이)으로 표류하고 그게 autosave 에 굳는다. moveState 는 CS_MOVE payload 첫 바이트라 역직렬화 없이 1바이트 무상태 read.
    const bool isStopMove = (isMove && len > headerSize &&
        static_cast<EMoveState>(job->PacketBytes()[headerSize]) == EMoveState::STOP);
    if (isMove && !isStopMove)
    {
        m_channels[chId]->EnqueueMoveJob(job);
    }
    else
    {
        m_channels[chId]->EnqueueJob(job);
    }
}

// 연결 종료 통지 - 인증 세션이면 소속 채널 세션이벤트 큐에 leave job(절대 드롭 금지). 미인증이면 정리할 게임 상태 없음.
void KYS::SERVERAPP::ChannelManager::EnqueueDisconnect(UINT64 sid, EDisconnectReason reason)
{
    {
        KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_preAuthLock);
        m_preAuthAttempts.erase(sid);   // pre-auth 시도 카운터 정리 - 모든 종료(admitted/미admitted)의 보편 훅이라 여기서 지우면 누수 0
    }
    const int chId = ResolveChannel(sid);
    if (chId == INVALID_CHANNEL_ID)
    {
        // pre-auth - 게임세션 미등록. 단 verify는 됐으나 채널 미선택(pending) 상태일 수 있다 -> 링크 스레드에
        //   통지해 pending 정리 + online 해제(ghost 봉인). pending 아니면 ProcessPendingGone이 no-op.
        LoginLinkThread::GetInstance().EnqueuePendingGone(sid);
        return;
    }
    m_channels[chId]->EnqueueSessionEvent(new OnClientLeaveJob(m_channels[chId], sid, reason));

    // socket 카운트는 SessionPool 활성 슬롯에서 파생 - 별도 카운터 감소 안 함(짝없는 socket-- 음수 제거)
}

// sid가 어느 채널 소속인지 찾는다 (미인증/옛 sid면 INVALID_CHANNEL_ID - 채널 미소속).
int KYS::SERVERAPP::ChannelManager::ResolveChannel(UINT64 sid)
{
    // full sid 키 map 조회. 역참조(GetChannelId)까지 read 락 보유 -> erase+Deallocate(write)와 상호배제.
    KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
    std::unordered_map<UINT64, GameSession*>::const_iterator it = m_sessionMap.find(sid);
    if (it == m_sessionMap.end())
    {
        return INVALID_CHANNEL_ID;   // 미인증(pre-auth)/옛 sid - 채널 없음. 호출자가 pre-auth 분기/가드(0번 채널 폐기로 sid를 채널 인덱스로 표현 불가)
    }
    return it->second->GetChannelId();   // map 멤버십 = 인증됨 (insert는 SetChannel 후)
}

// 로그인 성공 - 게임세션을 풀에서 할당해 채널을 세팅하고 map에 등록한다 (같은 sid 중복 로그인 멱등).
//   sid       : 소켓 세션 핸들
//   chId      : 배정 채널
//   accountId : 인증 계정 번호 (0=미사용, admission이 verify 응답으로 전달). 0 아니면 역인덱스(accountId->sid) 등록.
KYS::SERVERAPP::GameSession* KYS::SERVERAPP::ChannelManager::OnLoginSuccess(UINT64 sid, int chId, UINT32 accountId, UINT64 token)
{
    // 생존 확인: 로그인 처리 시점에 소켓이 이미 끊겨 재사용됐으면(generation 불일치) Init 하지 않는다.
    if (!m_server->IsSessionAlive(sid))
    {
        return nullptr;
    }

    GameSession* gs = nullptr;
    {
        // write 락 안에서 중복-체크 -> Allocate -> Init -> insert 를 한 번에 (같은 sid 중복 로그인 안전).
        //   Allocate(ObjectPool SpinLock)가 이 락 안에서 겹치나, OnSessionGone은 락 해제 후 Deallocate라 역순 겹침 없음(데드락 0).
        KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_sessionLock);

        std::unordered_map<UINT64, GameSession*>::const_iterator it = m_sessionMap.find(sid);
        if (it != m_sessionMap.end())
        {
            return nullptr;   // 같은 sid 중복 로그인 -> abort. 기존 gs 반환하면 2차 DB load->AttachPlayer가 1차 Player를 덮어써 누수
        }

        gs = m_gameSessionPool.Allocate();   // 소켓 idx 무관 새 슬롯 -> 잘못된 덮어쓰기 소멸
        if (gs == nullptr)
        {
            return nullptr;   // 풀 고갈 가드(도달 불가: MAX_CCU < 풀 용량, admission이 enforce)
        }
        gs->SetServer(m_server);   // map insert 전 주입 -> 보이는 gs는 항상 m_server 세팅됨
        gs->Init(sid);
        gs->SetChannel(chId);
        gs->SeedObfKeys(token);    // publish 전 obf 키 심기 (seed-before-publish) - worker가 세션을 찾는 순간 recv 키는 이미 seed됨(seed=0 창 봉인). 롤백 시 아래 gs->Reset()가 재zero
        m_sessionMap[sid] = gs;    // full sid 키 (봇A != 봇B)
        if (chId >= 0 && chId < MAX_CHANNEL_COUNT) { ++m_channelPlayerCount[chId]; }   // 채널 인구 +1 (락 내라 map과 일관, cap 판정용)
        if (accountId != 0)
        {
            gs->SetAccountId(accountId);        // leave 통지/축출 키
            m_accountToSid[accountId] = sid;    // accountId -> 현 세션 sid (kick 역인덱스, 신규가 기존 매핑 계승)
        }

        // login 도중 소켓 사망 봉인: pre-check 이후 map insert 전에 소켓이 끊겼을 수 있다(R1은 채널 스레드 직렬화가 없음).
        //   insert 직후 같은 write-lock 안에서 generation 재확인 - 죽었으면 방금 넣은 것을 통째 롤백(고아 0).
        //   IsSessionAlive 는 풀 spinlock 만 잡으므로 m_sessionLock 안 호출에 lock-order 역전 없음(풀 락 안에서 m_sessionLock 을 잡는 경로가 없다).
        if (!m_server->IsSessionAlive(sid))
        {
            m_sessionMap.erase(sid);
            if (chId >= 0 && chId < MAX_CHANNEL_COUNT) { --m_channelPlayerCount[chId]; }
            if (accountId != 0) { m_accountToSid.erase(accountId); }
            gs->Reset();
            m_gameSessionPool.Deallocate(gs);   // Allocate와 같은 락 순서(m_sessionLock->pool SpinLock) - 역전 0
            return nullptr;
        }
    }

    ServerMonitor::GetInstance().OnLogin();  // 모니터링 - 인증 세션수는 GetAuthenticatedCount(map.size)로 파생 (싱글턴 직접, null 가드 불요)

    // 로그인 성공 이력 - 접속 IP 포함 (실무 수렴: 기록하는 표본 4/5 가 IP 동봉. accept 경로가 세션에 심어둔 값을 읽기만)
    {
        UINT32 ip = 0;
        ip = m_server->GetSessionRemoteIp(sid);   // 값으로 읽는다 - 풀 락 안(Reset 과 상호배제). 없으면 0
        const IpOctets o = SplitIp(ip);
        KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
            KYS::GAMESERVER::LOG::LogChannel::USER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"login",
            L"account=%u sid=%llu ch=%d ip=%u.%u.%u.%u", accountId, sid, chId, o.a, o.b, o.c, o.d);
    }
    return gs;
}

// 세션 종료 - map에서 빼고 Player/게임세션을 풀에 반납한다 (중복 leave도 안전).
void KYS::SERVERAPP::ChannelManager::OnSessionGone(UINT64 sid)
{
    GameSession* gs = nullptr;
    UINT32 accountId = 0;   // 인증 세션이면 leave 통지/역인덱스 정리 키 (0=미인증, 축출된 세션)
    {
        KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_sessionLock);
        std::unordered_map<UINT64, GameSession*>::iterator it = m_sessionMap.find(sid);
        if (it != m_sessionMap.end())
        {
            gs = it->second;
            accountId = gs->GetAccountId();   // 축출(KickByAccount)된 세션은 이미 0 -> 통지/erase 둘 다 게이트
            m_sessionMap.erase(it);   // 키=full sid -> 봇A leave가 정확히 gsA만 제거
            const int ch = gs->GetChannelId();   // Reset(락 밖) 전이라 유효
            if (ch >= 0 && ch < MAX_CHANNEL_COUNT) { --m_channelPlayerCount[ch]; }   // 채널 인구 -1 (락 내라 map과 일관)

            // 역인덱스 값일치 erase: 신규 로그인이 같은 accountId를 sid_new로 이미 덮었으면 그 매핑을 보존(이 sid 것만 지움).
            if (accountId != 0)
            {
                std::unordered_map<UINT32, UINT64>::iterator ai = m_accountToSid.find(accountId);
                if (ai != m_accountToSid.end() && ai->second == sid) { m_accountToSid.erase(ai); }
            }

            // 축출된 세션이면 종료 저장(위 OnClientLeave가 이미 enqueue)이 끝났으니 pendingSave 해제 -> 신규 로그인 Load defer 풀림.
            //   Save enqueue가 이 시점(OnSessionGone)보다 먼저라, 단일 DBThread FIFO에서 Save가 이후 Load보다 앞선다(lost-update 봉인).
            const UINT32 kickSaveAcct = gs->GetKickSaveAccountId();
            if (kickSaveAcct != 0)
            {
                m_pendingSaveAccounts.erase(kickSaveAcct);
            }
        }
    }
    if (gs != nullptr)
    {
        Player* p = gs->GetPlayer();   // Reset 전에 잡아둠
        gs->Reset();                   // m_player=nullptr, m_sessionId=0
        if (p != nullptr)
        {
            p->SetGameSession(nullptr);   // 방어적 clear (역포인터 위생 - 로컬 p 사용, gs->GetPlayer 재조회 금지=null deref 회피)
            m_playerPool.Deallocate(p);   // Player 단일 회수점 (RemovePlayer는 grid unlink만 - 거기서 Deallocate 금지)
        }
        m_gameSessionPool.Deallocate(gs);   // 게임세션 풀 반납
    }
    // 로그인 서버 online 표 해제 통지 (인증 세션만). sid 동봉 -> LoginServer는 그 sid가 현 holder일 때만 제거
    //   (같은 계정이 빠르게 재접속해 sid_new로 갈아탄 뒤 늦게 도는 이 leave가 신규 online을 지우는 ghost 봉인). 인라인 소켓 I/O 금지 -> enqueue.
    if (accountId != 0)
    {
        LoginLinkThread::GetInstance().EnqueueSessionGone(accountId, sid);
    }
    // map miss -> no-op (이미 회수 / 중복 leave도 안전 - map 멤버십이 generation gate 대체)
}

// sid의 게임세션 조회 (옛/미인증이면 nullptr).
KYS::SERVERAPP::GameSession* KYS::SERVERAPP::ChannelManager::GetSession(UINT64 sid)
{
    // full sid 키 map 조회. 반환 gs를 락 밖에서 써도 허상 0(풀 블록 영구 상주). 채널 스레드는 자기 sid 를 소유하고,
    //   모니터(main)는 GameSession 의 평문 스칼라(accountId/channelId/midFlush)만 근사 read 한다(Player 등 소유 객체 deref 금지).
    KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
    std::unordered_map<UINT64, GameSession*>::const_iterator it = m_sessionMap.find(sid);
    if (it == m_sessionMap.end())
    {
        return nullptr;   // 옛/미인증 (map 멤버십이 generation gate 대체)
    }
    return it->second;
}

// 인증 세션 수 = 현재 접속자 수(CCU).
int KYS::SERVERAPP::ChannelManager::GetAuthenticatedCount() const
{
    // map은 인증 시점(SetChannel 후) insert -> size = 인증 수 (별도 카운터 없이 집합 크기로 파생, drift 불가).
    KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
    return static_cast<int>(m_sessionMap.size());
}

// sid -> 부착된 Player (없으면 nullptr).
KYS::SERVERAPP::Player* KYS::SERVERAPP::ChannelManager::GetPlayer(UINT64 sid)
{
    GameSession* gs = GetSession(sid);
    return (gs != nullptr) ? gs->GetPlayer() : nullptr;
}

// Player를 풀에서 할당해 세션에 붙인다(순수 attach). spawn(채널 진입 job)은 분리 -
//   DbResultJob 이 캐릭터+인벤을 다 채운 뒤 EnqueueChannelEnter 로 예약한다(원자 로드 - 빈 인벤 Player 가 grid 에 안 올라감).
//   진입 채널(chId)은 여기서 안 쓴다(spawn 예약이 분리돼서) - 세션은 이미 자기 채널을 안다.
//   sid  : 대상 세션
KYS::SERVERAPP::Player* KYS::SERVERAPP::ChannelManager::AttachPlayer(UINT64 sid)
{
    GameSession* gs = GetSession(sid);
    if (gs == nullptr)
    {
        return nullptr;
    }

    Player* p = m_playerPool.Allocate(sid);
    if (p == nullptr)
    {
        return nullptr;
    }                 // 풀 고갈 가드

    gs->SetPlayer(p);
    p->SetGameSession(gs);   // 양방향 링크 - 그리드 진입(OnChannelEnter) 전 set -> broadcast 핫패스 deref 안전
    return p;
}

// 채널 진입(spawn) job 예약 - AttachPlayer 에서 분리. DbResultJob 이 캐릭터+인벤을 다 채운 뒤 호출해
//   빈 인벤 Player 가 grid 에 편입(spawn)되지 않도록 한다(원자 로드). chId 는 admission 에서 검증된 값.
void KYS::SERVERAPP::ChannelManager::EnqueueChannelEnter(UINT64 sid, int chId)
{
    m_channels[chId]->EnqueueSessionEvent(new OnChannelEnterJob(m_channels[chId], sid));   // spawn 누락=보이지 않는 좀비 - 절대 드롭 금지
}

// idle 후보 sid만 read 락으로 스냅샷한다 (실제 회수는 락 밖 Channel이 수행).
//   chId      : 이 채널 소속만 대상
//   nowMs     : 현재 시각
//   timeoutMs : 무활동 임계
//   out       : 수집될 idle sid 목록
void KYS::SERVERAPP::ChannelManager::CollectIdleSessions(int chId, UINT64 nowMs, UINT64 timeoutMs, std::vector<UINT64>& out) const
{
    KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
    const UINT64 wallNow = ::GetTickCount64();   // player-less loading deadline 비교용 글로벌 시계(admission GetTickCount64와 동일). idle nowMs 도 이제 GetTickCount64 라 두 경로가 같은 시계 - 별도 read 는 lock 시점 최신값 확보용
    for (std::unordered_map<UINT64, GameSession*>::const_iterator it = m_sessionMap.begin(); it != m_sessionMap.end(); ++it)
    {
        GameSession* gs = it->second;
        if (gs->GetChannelId() != chId)
        {
            continue;   // 다른 채널 소속 - 그 채널 스레드가 처리(중복 회수 방지)
        }
        if (gs->GetPlayer() == nullptr)
        {
            // player-less: 정상 admission->AttachPlayer 윈도우(transient)는 보호하되, LOADING_DEADLINE_MS 넘게 고착한 좀비(DB Load 실패/풀고갈)는 회수.
            //   wall-clock 기준이라 CS_PING heartbeat 가 못 갱신(activity 무관) - Fix A 가 못 덮는 미래 우회 경로의 2선 방어(rAthena/HeavenMS 식).
            const UINT64 admittedAt = gs->GetAdmittedAtMs();
            if (admittedAt != 0 && wallNow - admittedAt > LOADING_DEADLINE_MS)
            {
                out.push_back(it->first);
            }
            continue;
        }
        const UINT64 last = gs->GetLastActivityMs();
        if (last == 0)
        {
            continue;   // 활동 미초기화(진입 직전 윈도우) - 오회수 방지
        }
        if (nowMs - last < timeoutMs)
        {
            continue;   // 활동 중 - 회수 대상 아님
        }
        out.push_back(it->first);   // sid 스냅샷 - 순회 중 erase 안 함(락 밖 Channel이 회수)
    }
}

// 채널별/(채널,맵)별 플레이어 수를 read 락으로 1회 순회 집계한다 (모니터링).
//   channelCount  : 채널 수
//   perChannel    : 채널별 카운트 버퍼 (호출자가 0으로 초기화)
//   perChannelMap : (채널*MAX_MAP_COUNT + mapId)별 카운트 버퍼 - 할당(모니터/exporter)과 인덱싱(여기)이 같은 상한 상수를 써야 한다
void KYS::SERVERAPP::ChannelManager::CollectPopulation(int channelCount, int* perChannel, int* perChannelMap) const
{
    // read 락이 erase/release(write 락)를 배제하므로 세션/Player 포인터는 순회 중 유효. GetMapId는 근사 read(int).
    KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
    for (std::unordered_map<UINT64, GameSession*>::const_iterator it = m_sessionMap.begin(); it != m_sessionMap.end(); ++it)
    {
        GameSession* gs = it->second;
        Player* p = gs->GetPlayer();
        if (p == nullptr)
        {
            continue;   // 인증됐으나 Player 미부착(로그인 진행 중) - 플레이어로 안 셈
        }
        const int ch = gs->GetChannelId();
        if (ch < 0 || ch >= channelCount)
        {
            continue;   // 범위 가드
        }
        ++perChannel[ch];
        const int mapId = p->GetMapId();
        if (mapId >= 0 && mapId < MapTableCount())   // 쓰는 맵 수까지만 (미사용 슬롯 통계 오염 방지)
        {
            ++perChannelMap[ch * MAX_MAP_COUNT + mapId];   // 보폭(stride)은 버퍼 할당과 같은 상한 상수 - 다르면 힙 밖 write
        }
    }
}

// 진단(모니터) - cap 카운터 + 파생 인구 + player-less + CCU 를 한 read 락에서 함께 뽑는다.
//   한 락 스냅샷이라야 sum(cap)==CCU (drift 검사) 와 derived+playerLess==CCU (로딩 세션 정합) 가 유의미하다.
//   cap 카운터 = admission 시 ++, 종료 시 --, 이관 시 net-0 스왑 -> 항상 sum==CCU 여야 한다(어긋나면 drift 버그).
void KYS::SERVERAPP::ChannelManager::CollectPopulationDiagnostics(int channelCount, int* perChannelCap,
                                                                  int* outDerivedTotal, int* outPlayerLess, long* outCcu) const
{
    KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
    for (int ch = 0; ch < channelCount && ch < MAX_CHANNEL_COUNT; ++ch)
    {
        perChannelCap[ch] = m_channelPlayerCount[ch];   // cap 카운터 스냅샷
    }
    int derived = 0;      // Player 부착(인게임 진입) 세션
    int playerLess = 0;   // 인증됐으나 로드 중(AttachPlayer 전) 세션
    for (std::unordered_map<UINT64, GameSession*>::const_iterator it = m_sessionMap.begin(); it != m_sessionMap.end(); ++it)
    {
        if (it->second->GetPlayer() != nullptr) { ++derived; }
        else { ++playerLess; }
    }
    *outDerivedTotal = derived;
    *outPlayerLess = playerLess;
    *outCcu = static_cast<long>(m_sessionMap.size());
}

int KYS::SERVERAPP::ChannelManager::GetGameSessionPoolUsed() const
{
    return static_cast<int>(m_gameSessionPool.GetUsedCount());   // 검증 관측(근사 - 무락 read)
}

int KYS::SERVERAPP::ChannelManager::GetPlayerPoolUsed() const
{
    return static_cast<int>(m_playerPool.GetUsedCount());   // 검증 관측(근사 - 무락 read)
}

// chId 범위 가드 후 채널 포인터 반환 (범위 밖이면 nullptr).
KYS::SERVERAPP::IChannel* KYS::SERVERAPP::ChannelManager::GetChannel(int chId)
{
    // 상한/하한 가드 - chId 는 게임 코드가 넘기는 값이라 방어.
    if (chId < 0 || chId >= m_channelCount)
    {
        return nullptr;
    }
    return m_channels[chId];   // OnLoginSuccess 의 m_channels[chId] 와 같은 배열 (다형 IChannel*)
}

// CCU가 상한(MAX_CCU)에 도달했는지 (집합 크기로 파생, 별도 카운터 없음).
bool KYS::SERVERAPP::ChannelManager::IsCcuFull() const
{
    return GetAuthenticatedCount() >= MAX_CCU;
}

// pre-auth sid에 거부 패킷을 보내고 끊는다 (게임세션이 없어 raw 소켓 Session을 직접 사용).
//   sid    : 거부할 연결
//   data   : 보낼 거부 패킷
//   size   : 길이
//   reason : 종료 사유
void KYS::SERVERAPP::ChannelManager::RejectConnection(UINT64 sid, const BYTE* data, int size,
                                                      EDisconnectReason reason)
{
    // 거부 통지는 best-effort(KEEP) - 넘쳐도 여기서 안 끊는다: 아래 사유(LOGIN_REJECTED/SERVER_FULL)로 FIN 종료해야 하는 규약(Defines.h IsAbnormal).
    //   두 호출 사이에 슬롯이 회수·재사용돼도 DisconnectTo 는 세대 불일치로 no-op - 새 접속자를 끊지 않는다.
    m_server->SendTo(sid, data, size, ESendDropPolicy::KEEP_CONNECTION);
    m_server->DisconnectTo(sid, reason);   // 소켓 종료 (접속자 미증가 = 하드 보장)
}

// pre-auth sid 강제 종료 - Send 없이 Disconnect만. kick/timeout은 거부 패킷이 불필요하다(클라는 recv 에러로 끊김 감지).
//   admission kick(KickByAccount의 DisconnectTo)과 동일 패턴으로 통일.
void KYS::SERVERAPP::ChannelManager::DisconnectPending(UINT64 sid, EDisconnectReason reason)
{
    m_server->DisconnectTo(sid, reason);   // 없는/옛 핸들이면 no-op
}

// 채널 선택(B) - chId가 유효하고 cap 미만인지 (선택 수신 시 admission 직전 재검사, read 락).
bool KYS::SERVERAPP::ChannelManager::IsChannelJoinable(int chId) const
{
    if (chId < 0 || chId >= m_channelCount || chId >= MAX_CHANNEL_COUNT)
        return false;
    KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
    return m_channelPlayerCount[chId] < MAX_PLAYERS_PER_CHANNEL;
}

// 인게임 채널 이동 - count 스왑 + SetChannel + 대상 채널 enter job post 를 한 write 락에 (계획 0.1 step 4).
//   호출자(Channel::OnChannelChange, 출발 채널 스레드)가 이미 A 그리드 제거 + 배치 quiesce 를 마친 뒤 호출한다.
bool KYS::SERVERAPP::ChannelManager::MigrateChannel(UINT64 sid, int fromCh, int toCh)
{
    if (fromCh < 0 || fromCh >= MAX_CHANNEL_COUNT) { return false; }
    if (toCh < 0 || toCh >= m_channelCount || toCh >= MAX_CHANNEL_COUNT) { return false; }

    {
        KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_sessionLock);   // SRW 비재귀 - GetSession(read 락) 대신 map 직접 조회
        std::unordered_map<UINT64, GameSession*>::iterator it = m_sessionMap.find(sid);
        if (it == m_sessionMap.end())
        {
            return false;   // 이관 사이 끊김 등 - 세션 없음
        }
        GameSession* gs = it->second;

        // 소유권 재검증 - fromCh가 실제 현 소유 채널인지 write 락 안에서 확인. 중복 이관/stale fromCh를 거부해
        //   count 이중 감소(drift)와 이중 enter job post(이중 grid insert/UAF)를 봉인한다. 정상 경로(OnChannelChange 소유권 가드 통과)에선 항상 일치.
        if (gs->GetChannelId() != fromCh)
        {
            return false;
        }

        // 인구 스왑 (net-0) + 라우팅 전환. worker ResolveChannel(read 락)은 이 write 락에 막혀 중간 상태를 못 본다.
        --m_channelPlayerCount[fromCh];
        ++m_channelPlayerCount[toCh];
        gs->SetChannel(toCh);

        // 대상 채널 session-event mailbox 로 enter job post (락 안에서). 락 해제 시점엔 enter job 이 이미 큐에 있어,
        //   worker 가 대상 채널로 라우팅하는 첫 패킷보다 대상 tick 의 phase 0(session-event)이 grid insert 를 먼저 처리한다.
        //   락 순서: m_sessionLock(write) -> 대상 mailbox SpinLock(leaf). 드레인은 dequeue 후 Execute 라 역순 없음 = 데드락 0.
        m_channels[toCh]->EnqueueSessionEvent(new OnChannelChangeEnterJob(m_channels[toCh], sid));
    }

    // 채널 이동 이력 - 반드시 write 락 "밖"에서 (로그의 큐 락/커널 신호 대기가 전역 라우팅 락 보유시간에 얹히지 않게.
    //   login/kick 로그와 같은 배치 원칙. in-place handoff 라 leave/login 로그가 안 남는 유일한 소속 변경이라 여기서 남긴다.)
    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
        KYS::GAMESERVER::LOG::LogChannel::USER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"channel",
        L"채널 이동 sid=%llu ch %d -> %d", sid, fromCh, toCh);
    return true;
}

// 이관 중 disconnect가 옛 채널로 라우팅된 leave를 현 소유 채널로 다시 보낸다 (Channel::OnClientLeave가 소유자 아님을 감지 시 호출).
//   소유 채널이 자기 그리드 제거 + OnSessionGone(free)를 수행해야 도착 채널 그리드에 dangling Player*가 안 남는다.
//   세션은 아직 map에 있어(free는 소유 채널이) 재라우팅이 유실되지 않는다. 연쇄 이관이면 소유자를 따라 수렴(유저 페이스라 유한).
void KYS::SERVERAPP::ChannelManager::RerouteLeave(UINT64 sid, EDisconnectReason reason)
{
    int ch = INVALID_CHANNEL_ID;
    {
        KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
        std::unordered_map<UINT64, GameSession*>::const_iterator it = m_sessionMap.find(sid);
        if (it == m_sessionMap.end())
        {
            return;   // 이미 정리됨 - 재라우팅 불필요
        }
        ch = it->second->GetChannelId();
    }
    if (ch >= 0 && ch < m_channelCount)
    {
        m_channels[ch]->EnqueueSessionEvent(new OnClientLeaveJob(m_channels[ch], sid, reason));
    }
}

// 채널 선택(B) - 채널별 현재 인구 스냅샷 (SC_CHANNEL_LIST 빌드용, read 락).
void KYS::SERVERAPP::ChannelManager::SnapshotChannelCounts(int* outCounts, int n) const
{
    KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
    for (int i = 0; i < n && i < MAX_CHANNEL_COUNT; ++i)
        outCounts[i] = m_channelPlayerCount[i];
}

// 채널별 현재 인구를 SC_CHANNEL_LIST 완성 패킷으로 조립한다 (wire 포맷 단일 소스).
//   로그인 push(LoginLinkThread::SendChannelList)와 인게임 재조회(Channel::OnChannelListRequest)가 공용 -
//   entries 매핑/maxPlayers 가 두 경로에서 갈라지지 않게 여기 한 곳에 둔다. 송신은 호출자(pre-auth=SendRaw / 인게임=gs->Send).
bool KYS::SERVERAPP::ChannelManager::BuildChannelListPacket(KYS::GAMECOMMON::PROTOCOL::CPacket& out) const
{
    const int n = GetChannelCount();
    int counts[MAX_CHANNEL_COUNT];
    for (int i = 0; i < MAX_CHANNEL_COUNT; ++i) { counts[i] = 0; }
    SnapshotChannelCounts(counts, n);

    SC_CHANNEL_LIST list;
    list.channelCount = static_cast<BYTE>(n);
    for (int i = 0; i < n && i < MAX_CHANNEL_COUNT; ++i)
    {
        list.entries[i].channelId   = static_cast<BYTE>(i);
        list.entries[i].playerCount = counts[i];
        list.entries[i].maxPlayers  = MAX_PLAYERS_PER_CHANNEL;
    }
    out.Begin(static_cast<USHORT>(PacketType::SC_CHANNEL_LIST));
    list.Serialize(out);
    return out.End();
}

// admission된 (accountId, sid) 쌍을 출력 벡터에 append (online 스냅샷용, read 락). 짧게 유지 - 30Hz login/leave write 와 경합.
void KYS::SERVERAPP::ChannelManager::AppendOnlineAccounts(std::vector<UINT32>& accountIds, std::vector<UINT64>& sids) const
{
    KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
    for (std::unordered_map<UINT32, UINT64>::const_iterator it = m_accountToSid.begin(); it != m_accountToSid.end(); ++it)
    {
        accountIds.push_back(it->first);
        sids.push_back(it->second);
    }
}

// 채널 선택(B) - pre-auth sid에 패킷만 보낸다 (RejectConnection과 달리 Disconnect 안 함, SC_CHANNEL_LIST 송신).
void KYS::SERVERAPP::ChannelManager::SendRaw(UINT64 sid, const BYTE* data, int size)
{
    m_server->SendTo(sid, data, size, ESendDropPolicy::KEEP_CONNECTION);   // Fire-and-Forget (best-effort) - pre-auth 응답은 넘쳐도 안 끊는다(클라 재요청으로 복원)
}

// sid의 채널 mailbox에 job을 넣는다 (cross-channel 라우팅 진입점).
void KYS::SERVERAPP::ChannelManager::PostToChannel(UINT64 sid, OwnedDeliveryJob* job)
{
    // ResolveChannel(sid): sid의 게임세션을 map에서 찾아 그 채널 id를 반환한다(슬롯 인덱스가 아님).
    //   미인증/옛 sid면 INVALID_CHANNEL_ID 반환. EnqueueConnect 등과 같은 경로.
    //   [!] 절대 m_channels[sid>>32]로 단축하지 말 것 - sid>>32는 세션 슬롯 인덱스라 채널 수와 무관(범위 밖).
    const int chId = ResolveChannel(sid);
    if (chId == INVALID_CHANNEL_ID)
    {
        delete job;   // 대상 오프라인(미인증/옛 sid) - 배달 불가, job 회수 (m_channels[-1] OOB 방지)
        return;
    }
    job->SetRoutedChannel(chId);   // 이 Job 이 라우팅되는 채널 - Execute 에서 대상 현 소유와 비교(그 사이 이관됐으면 drop)
#ifdef _DEBUG
    // [B3 재현 하니스] TOCTOU 창 인위 확대: resolve~enqueue 사이에 대상이 채널 이관되도록 지연을 준다.
    //   환경변수 KYS_B3_REPRO_WIDEN_MS(ms)로만 활성 - 미설정이면 0(무영향). Release 미포함.
    //   PostToChannel 은 SST 단일 스레드(WhisperRouteJob::Execute)에서만 호출 - static 초기화 경쟁 없음.
    {
        static int s_reproWidenMs = -1;
        if (s_reproWidenMs < 0)
        {
            wchar_t buf[16]; size_t n = 0;
            s_reproWidenMs = (::_wgetenv_s(&n, buf, 16, L"KYS_B3_REPRO_WIDEN_MS") == 0 && n > 0) ? ::_wtoi(buf) : 0;
        }
        if (s_reproWidenMs > 0) { ::Sleep(static_cast<DWORD>(s_reproWidenMs)); }
    }
#endif
    m_channels[chId]->EnqueueJob(job);   // OnRecvJob enqueue와 같은 경로 (IChannel::EnqueueJob)
}

// 중복 로그인 축출 - accountId 역인덱스로 기존 세션을 찾아 소켓을 강제 종료한다 (LoginLinkThread가 IS_KICK 수신 시 호출).
//   실제 map/풀 정리는 소켓 종료 -> 라이브러리 EnqueueDisconnect -> OnSessionGone에서. 여기선 게이트 닫기 + 소켓 종료만.
bool KYS::SERVERAPP::ChannelManager::KickByAccount(UINT32 accountId)
{
    if (accountId == 0) { return false; }

    // (1) admission된 세션 축출 (m_accountToSid 역인덱스). 없으면 pending일 수 있으니 early return 금지 - (2)로 진행.
    UINT64 oldSid = 0;
    bool   found  = false;
    {
        KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_sessionLock);
        std::unordered_map<UINT32, UINT64>::iterator ai = m_accountToSid.find(accountId);
        if (ai != m_accountToSid.end())
        {
            found  = true;
            oldSid = ai->second;
            std::unordered_map<UINT64, GameSession*>::iterator si = m_sessionMap.find(oldSid);
            if (si != m_sessionMap.end())
            {
                si->second->SetKickSaveAccountId(accountId);   // 종료 저장 후 pendingSave clear용 accountId 보존 (m_accountId는 아래서 0이 됨)
                si->second->SetAccountId(0);   // 이 세션의 leave 통지/역인덱스 게이트를 닫음 (자기 leave가 신규 online을 안 지움)
                m_pendingSaveAccounts.insert(accountId);   // 신규 로그인 Load를 이 세션의 종료 저장이 enqueue될 때까지 defer (lost-update 봉인). OnSessionGone가 clear.
            }
            m_accountToSid.erase(ai);   // 역인덱스 즉시 제거 (신규가 곧 sid_new로 다시 채움)
        }
    }
    if (found)
    {
        m_server->DisconnectTo(oldSid, EDisconnectReason::NET_RESET);   // RST 강제 종료 (축출 = 강제 끊김). 없는/옛 핸들이면 no-op
        // 축출 발행 이력 - 같은 sid 의 leave(NET_RESET) 행과 대조해 "reset = 중복 로그인 축출" 을 판별하는 근거
        KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
            KYS::GAMESERVER::LOG::LogChannel::USER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"kick",
            L"중복 로그인 축출 account=%u sid=%llu", accountId, oldSid);
    }

    // (2) 채널 선택 대기(pending) 중인 기존 세션 축출 - admission 전이라 m_accountToSid에 없으므로 항상 시도해야 한다
    //   (admitted가 없어도 pending이 있을 수 있음 - 위에서 early return하면 이 경로를 놓쳐 pending kick이 누락됐던 버그).
    //   KickByAccount도 IS_KICK 처리=링크 스레드라 pending 맵 직접 접근 안전.
    LoginLinkThread::GetInstance().KickPending(accountId);   // pending 축출은 실행하되(중복 로그인 정리) 반환값엔 넣지 않는다 - pending은 admission 전이라 CCU 슬롯을 미소비, 축출해도 빈 슬롯이 생기지 않는다
    return found;   // net-zero(CCU 게이트 면제)는 admitted victim 축출로 실 슬롯이 빈 경우만. pending-only kick은 슬롯 회수 0이라 면제하면 CCU cap 초과 admission이 된다
}

// 이 연결의 CS_GAME_AUTH 시도를 +1 하고 cap 이하면 true - 초과면 verify(인터서버 왕복)를 enqueue하지 않아 증폭을 막는다.
//   LoginHandler::AllowLoginAttempt 미러. cap 도달 후엔 증가 없이 거부(포화 - UINT32 wrap 재허용 차단). 계정 잠금 없음.
bool KYS::SERVERAPP::ChannelManager::AllowPreAuthAttempt(UINT64 sid)
{
    KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_preAuthLock);
    UINT32& count = m_preAuthAttempts[sid];   // 없으면 0으로 생성
    if (count >= MAX_PREAUTH_ATTEMPTS_PER_CONN)
    {
        ::InterlockedIncrement(&s_preAuthBlockCount);   // silent-cap 규약: drop 은 조용하되 카운터로 관측(이벤트 로그는 폭주 방지 생략)
        return false;   // 이미 상한 - 증가 없이 거부(포화, wrap 방지)
    }
    ++count;
    return true;
}

// pre-auth 시도 cap 차단 누적 (모니터 표출용 read).
long KYS::SERVERAPP::ChannelManager::GetPreAuthBlockCount() const
{
    return s_preAuthBlockCount;
}

// 축출된 세션의 종료 저장이 enqueue 끝났는지 (집합에 없으면 끝남 또는 애초에 축출 아님). 링크 스레드가 신규 Load defer 해제 판정에 read.
bool KYS::SERVERAPP::ChannelManager::IsSaveReady(UINT32 accountId) const
{
    KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_sessionLock);
    return m_pendingSaveAccounts.find(accountId) == m_pendingSaveAccounts.end();
}

// 미인증 세션 첫 패킷 처리 (worker 스레드) - CS_GAME_AUTH면 토큰을 꺼내 검증을 enqueue, 그 외 opcode는 조용히 버린다.
//   data : 헤더 4B + payload / len : 전체 바이트. blocking I/O 금지(enqueue만).
void KYS::SERVERAPP::ChannelManager::HandlePreAuthPacket(UINT64 sid, const BYTE* data, int len)
{
    const int headerSize = static_cast<int>(sizeof(PacketHeader));
    if (len < headerSize)
    {
        return;   // type 필드(offset 2)도 못 읽음 - drop
    }
    const PacketType opcode =
        static_cast<PacketType>(::ntohs(*reinterpret_cast<const UINT16*>(data + sizeof(UINT16))));

    // 미인증(pre-auth) 세션이 허용하는 핸드셰이크 opcode: CS_GAME_AUTH(토큰 검증) / CS_CHANNEL_SELECT(채널 선택) /
    //   CS_CHARACTER_SELECT(입장=admission) / CS_CHARACTER_CREATE / CS_CHARACTER_DELETE. 그 외는 drop(끊지 않음).
    //   admission 후엔 같은 opcode 가 Channel::OnRecv 로 라우팅되며 거기선 stale 중복으로 무시한다(대칭).
    if (opcode == PacketType::CS_GAME_AUTH)
    {
        if (len < headerSize + static_cast<int>(sizeof(UINT64)))
            return;   // token(8B) 미만 = malformed -> drop
        if (!AllowPreAuthAttempt(sid))
            return;   // 이 연결의 CS_GAME_AUTH 시도 상한 초과 - verify enqueue 스킵(인터서버 증폭 차단). 정상 pending은 안 죽임(연결이 스스로 대역폭 낭비)
        KYS::GAMECOMMON::PROTOCOL::CPacket pkt(len - headerSize);
        pkt.Write(data + headerSize, len - headerSize);
        CS_GAME_AUTH body;
        body.Deserialize(pkt);
        LoginLinkThread::GetInstance().EnqueueVerify(sid, body.token);   // 비차단 SpinLock 큐 push
    }
    else if (opcode == PacketType::CS_CHANNEL_SELECT)
    {
        if (len < headerSize + 1)
            return;   // channelId(1B) 미만 = malformed -> drop
        KYS::GAMECOMMON::PROTOCOL::CPacket pkt(len - headerSize);
        pkt.Write(data + headerSize, len - headerSize);
        CS_CHANNEL_SELECT body;
        body.Deserialize(pkt);   // channelId(BYTE) 추출
        LoginLinkThread::GetInstance().EnqueueChannelSelect(sid, body.channelId);   // 선택 -> 링크 스레드 (캐릭터 목록 load)
    }
    else if (opcode == PacketType::CS_CHARACTER_SELECT)
    {
        if (len < headerSize + static_cast<int>(sizeof(UINT32)))
            return;   // charId(4B) 미만 = malformed -> drop
        KYS::GAMECOMMON::PROTOCOL::CPacket pkt(len - headerSize);
        pkt.Write(data + headerSize, len - headerSize);
        CS_CHARACTER_SELECT body;
        body.Deserialize(pkt);   // charId(UINT32) 추출
        LoginLinkThread::GetInstance().EnqueueCharacterSelect(sid, body.charId);   // 선택 -> 링크 스레드 admission
    }
    else if (opcode == PacketType::CS_CHARACTER_CREATE)
    {
        if (len < headerSize + 3)
            return;   // 이름 길이 2B + slotId 1B = 최소 3B. 1B 로 두면 slotId 가 없는 payload 가 파싱까지 가서
                      //   읽기 가드가 채우는 0 이 slotId 가 되는데, 슬롯 0 은 유효값(0..2)이라 거부되지 않는다.
        KYS::GAMECOMMON::PROTOCOL::CPacket pkt(len - headerSize);
        pkt.Write(data + headerSize, len - headerSize);
        CS_CHARACTER_CREATE body;
        body.Deserialize(pkt);   // name(가변) + slotId(BYTE) 추출
        LoginLinkThread::GetInstance().EnqueueCharacterCreate(sid, body.name, body.slotId);
    }
    else if (opcode == PacketType::CS_CHARACTER_DELETE)
    {
        if (len < headerSize + static_cast<int>(sizeof(UINT32)))
            return;   // charId(4B) 미만 = malformed -> drop
        KYS::GAMECOMMON::PROTOCOL::CPacket pkt(len - headerSize);
        pkt.Write(data + headerSize, len - headerSize);
        CS_CHARACTER_DELETE body;
        body.Deserialize(pkt);   // charId(UINT32) 추출
        LoginLinkThread::GetInstance().EnqueueCharacterDelete(sid, body.charId);
    }
    // 그 외 opcode: drop (admission 전 게임패킷 등, 끊지 않음)
}
