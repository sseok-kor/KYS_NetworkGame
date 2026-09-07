#include "pch_serverapp.h"
#include "LoginLinkThread.h"
#include "../Game/Channel/ChannelManager.h" // KickByAccount (IS_KICK demux) + admission
#include "../DataBase/DBCommands.h"         // LoadCharacterCommand (admission -> DB 캐릭터 로드)
#include "../../GameCommon/GameDefines.h"   // 게임 공용 상수 (WHISPER_NAME_MAX / ONLINE_SYNC_INTERVAL_MS 등)
#include "../../GameCommon/Protocol/PacketType.h"    // SC_LOGIN_RESULT (reject)
#include "../../GameCommon/Protocol/CPacket.h"       // reject 패킷 직렬화
#include "../GameServer/Core/Log/Logger.h"  // 인터서버 링크 상태 파일 로그 (디버거 전용 ODS 에서 승격)
#include <process.h>   // _beginthreadex (std::thread 금지)
#include <vector>      // online 스냅샷 임시 집합

namespace KYS
{
    namespace SERVERAPP
    {
        using KYS::GAMESERVER::THREAD::SpinLockGuard;

        // recv/send 타임아웃 ms (로그인 서버 지연 시 링크 실패로 판정). 재연결 지수 백오프 범위.
        static const int    LINK_RECV_TIMEOUT_MS   = 3000;
        static const DWORD  LINK_INITIAL_BACKOFF_MS = 500;
        static const DWORD  LINK_MAX_BACKOFF_MS     = 5000;
        static const UINT64 PENDING_SELECT_TIMEOUT_MS = 600000;  // 채널 선택 대기 상한 - 멈춘/좀비 pending 정리 TTL(10분)
        static const UINT64 PENDING_REAP_INTERVAL_MS  = 1000;    // pending TTL 점검 간격

        // e2e 관측 카운터 정의 (선언=LoginLinkThread.h).
        volatile LONG LoginLinkThread::s_verifyOk = 0;
        volatile LONG LoginLinkThread::s_verifyNotFound = 0;
        volatile LONG LoginLinkThread::s_verifyAlreadyOnline = 0;
        volatile LONG LoginLinkThread::s_verifyError = 0;
        volatile LONG LoginLinkThread::s_linkQueueDrop = 0;

        // 이 파일 안의 스레드 진입 함수 - this 의 ThreadLoop 를 바로 호출(Start 를 부르면 무한 재귀). DBThread 정합.
        static unsigned __stdcall LinkThreadEntry(void* arg)
        {
            static_cast<LoginLinkThread*>(arg)->ThreadLoop();
            return 0;
        }

        LoginLinkThread::LoginLinkThread()
            : m_client()
            , m_requests()
            , m_queueLock()
            , m_pendingSelect()
            , m_lastReapMs(0)
            , m_lastSyncMs(0)
            , m_thread(NULL)
            , m_connectDone(NULL)
            , m_stopEvent(NULL)
            , m_connectOk(false)
            , m_running(false)
            , m_linkUp(0)
            , m_loginServerIp(0)
            , m_loginServerPort(0)
            , m_serverId(0)
            , m_listenIp(0)
            , m_listenPort(0)
        {
            // 첫 연결 결과 신호 + 종료/백오프 깨우기용 수동 리셋 이벤트.
            m_connectDone = ::CreateEvent(NULL, TRUE, FALSE, NULL);
            m_stopEvent   = ::CreateEvent(NULL, TRUE, FALSE, NULL);
        }

        LoginLinkThread::~LoginLinkThread()
        {
            // main 이 Stop(join+CloseHandle) 후 파괴. 이벤트 핸들 정리.
            if (m_connectDone != NULL) { ::CloseHandle(m_connectDone); m_connectDone = NULL; }
            if (m_stopEvent   != NULL) { ::CloseHandle(m_stopEvent);   m_stopEvent = NULL; }
        }

        bool LoginLinkThread::Start(UINT32 loginServerIp, USHORT loginServerPort,
                                    USHORT serverId, UINT32 listenIp, USHORT listenPort, UINT32 authSecret)
        {
            m_loginServerIp   = loginServerIp;
            m_loginServerPort = loginServerPort;
            m_serverId        = serverId;
            m_listenIp        = listenIp;
            m_listenPort      = listenPort;
            m_authSecret      = authSecret;

            m_running = true;   // 스레드가 읽기 전에 true
            m_thread = reinterpret_cast<HANDLE>(
                _beginthreadex(NULL, 0, &LinkThreadEntry, this, 0, NULL));
            if (m_thread == NULL)
            {
                return false;   // 스레드 생성 실패
            }

            // 첫 연결 시도 결과를 기다린다. 링크 다운이어도 boot 는 비치명(백그라운드 재연결) - 호출자가 false 를 경고로만 처리.
            ::WaitForSingleObject(m_connectDone, INFINITE);
            return m_connectOk;
        }

        void LoginLinkThread::Stop()
        {
            m_running = false;                        // 다음 루프 반복에 관측 (volatile)
            if (m_stopEvent != NULL) { ::SetEvent(m_stopEvent); }   // 백오프/idle 대기 즉시 깨움
            if (m_thread != NULL)
            {
                // 활성 검증 1왕복 중이면 recv 타임아웃(LINK_RECV_TIMEOUT_MS)만큼 지연될 수 있음 - 그 후 m_running 관측.
                ::WaitForSingleObject(m_thread, INFINITE);
                ::CloseHandle(m_thread);
                m_thread = NULL;
            }
        }

        // 클라 구동 요청을 큐에 넣는다 - 큐가 cap 이면 유계 drop(flood 방어). true=적재, false=drop.
        //   cleanup 계열(EnqueueSessionGone/EnqueuePendingGone)은 이 게이트를 안 거친다 - 정리는 반드시 적재돼야 online/pending 누수가 없다.
        bool LoginLinkThread::TryPushRequest(const LinkRequest& req)
        {
            SpinLockGuard guard(m_queueLock);
            if (m_requests.size() >= MAX_LINK_QUEUE_DEPTH)
            {
                ::InterlockedIncrement(&s_linkQueueDrop);   // silent-cap 규약: 조용히 drop 하되 카운터로 관측
                return false;
            }
            m_requests.push(req);
            return true;
        }

        void LoginLinkThread::EnqueueVerify(UINT64 sid, UINT64 token)
        {
            LinkRequest req;
            req.kind      = ELinkReqKind::VERIFY;
            req.sid       = sid;
            req.token     = token;
            req.accountId = 0;

            TryPushRequest(req);   // 큐 cap 초과 시 유계 drop (flood 방어)
        }

        void LoginLinkThread::EnqueueSessionGone(UINT32 accountId, UINT64 sid)
        {
            LinkRequest req;
            req.kind      = ELinkReqKind::LEAVE;
            req.sid       = sid;        // 현 holder 검증용 (로그인 서버가 online[accountId]==sid 일 때만 해제)
            req.token     = 0;
            req.accountId = accountId;

            SpinLockGuard guard(m_queueLock);
            m_requests.push(req);
        }

        void LoginLinkThread::EnqueueChannelSelect(UINT64 sid, BYTE channelId)
        {
            LinkRequest req;
            req.kind      = ELinkReqKind::CHANNEL_SELECT;
            req.sid       = sid;
            req.token     = 0;
            req.accountId = 0;
            req.channelId = channelId;

            TryPushRequest(req);   // 큐 cap 초과 시 유계 drop (flood 방어)
        }

        void LoginLinkThread::EnqueueCharacterSelect(UINT64 sid, UINT32 charId)
        {
            LinkRequest req;
            req.kind      = ELinkReqKind::CHARACTER_SELECT;
            req.sid       = sid;
            req.token     = 0;
            req.accountId = 0;
            req.channelId = 0;
            req.charId    = charId;
            req.slotId    = 0;

            TryPushRequest(req);   // 큐 cap 초과 시 유계 drop (flood 방어)
        }

        void LoginLinkThread::EnqueueCharacterCreate(UINT64 sid, const wchar_t* name, BYTE slotId)
        {
            LinkRequest req;
            req.kind      = ELinkReqKind::CHARACTER_CREATE;
            req.sid       = sid;
            req.token     = 0;
            req.accountId = 0;
            req.channelId = 0;
            req.charId    = 0;
            // 이름 값 복사 - 과길이 입력에도 안전하게 절단(wcscpy_s 비절단 크래시 회피, 기능 A 교훈).
            if (name == nullptr) { req.name[0] = L'\0'; }
            else { ::wcsncpy_s(req.name, WHISPER_NAME_MAX, name, _TRUNCATE); }
            req.slotId    = slotId;

            TryPushRequest(req);   // 큐 cap 초과 시 유계 drop (flood 방어)
        }

        void LoginLinkThread::EnqueueCharacterDelete(UINT64 sid, UINT32 charId)
        {
            LinkRequest req;
            req.kind      = ELinkReqKind::CHARACTER_DELETE;
            req.sid       = sid;
            req.token     = 0;
            req.accountId = 0;
            req.channelId = 0;
            req.charId    = charId;
            req.slotId    = 0;

            TryPushRequest(req);   // 큐 cap 초과 시 유계 drop (flood 방어)
        }

        void LoginLinkThread::EnqueuePendingGone(UINT64 sid)
        {
            LinkRequest req;
            req.kind      = ELinkReqKind::PENDING_GONE;
            req.sid       = sid;
            req.token     = 0;
            req.accountId = 0;
            req.channelId = 0;

            SpinLockGuard guard(m_queueLock);
            m_requests.push(req);
        }

        bool LoginLinkThread::IsLinkUp() const
        {
            return m_linkUp != 0;   // volatile LONG 단일 읽기는 원자적
        }

        bool LoginLinkThread::PopRequest(LinkRequest& out)
        {
            SpinLockGuard guard(m_queueLock);
            if (m_requests.empty())
            {
                return false;
            }
            out = m_requests.front();
            m_requests.pop();
            return true;
        }

        // 연결 + IS_REGISTER 핸드셰이크(공유 비밀 + 자기 listen 주소). 성공 true.
        bool LoginLinkThread::ConnectAndRegister()
        {
            if (!m_client.Connect(m_loginServerIp, m_loginServerPort, LINK_RECV_TIMEOUT_MS))
            {
                return false;
            }
            if (!m_client.SendRegister(m_serverId, m_authSecret, m_listenIp, m_listenPort))
            {
                m_client.Close();
                return false;
            }
            BYTE ackResult = 0;
            if (!m_client.RecvRegisterAck(ackResult))
            {
                m_client.Close();
                return false;
            }
            if (ackResult != 1)
            {
                m_client.Close();   // 공유 비밀 불일치 등 등록 거부
                return false;
            }
            return true;
        }

        // 검증 1왕복(send REQ -> recv RES). 링크가 정상이면 true, 송수신/상관 실패면 false(재연결).
        //   중복 로그인이면 IS_KICK(기존 축출)이 RES 직전에 동봉돼 오므로, RES를 받을 때까지 demux 루프를 돈다.
        bool LoginLinkThread::ProcessVerify(UINT64 sid, UINT64 token)
        {
            if (!m_client.SendVerify(sid, token))
            {
                RejectOrphanConnection(sid);   // 링크 송신 실패 - orphan이면 즉시 거부(pre-auth 35s hang 소멸)
                return false;
            }
            bool sawKick = false;   // 이 왕복에 IS_KICK 동봉됨 = 중복 로그인 축출 후 신규(net-zero CCU - admission CCU 게이트 면제)
            for (;;)
            {
                IS_TOKEN_VERIFY_RES res;
                UINT32 kickAccountId = 0;
                bool   isKick = false;
                if (!m_client.RecvVerifyOrKick(res, kickAccountId, isKick))
                {
                    RejectOrphanConnection(sid);   // 링크 수신 실패/teardown - orphan이면 즉시 거부(pre-auth 35s hang 소멸)
                    return false;   // recv 실패/teardown
                }
                if (isKick)
                {
                    // admitted victim 축출 시에만 replacement(net-zero) - 그때만 실 CCU 슬롯이 빈다. pending-only kick(슬롯 미소비)과
                    //   ghost-heal(없는 세션 kick)은 sawKick=false -> ProcessCharacterSelect CCU-full 게이트 정상 작동(cap 초과 admission 차단).
                    if (ChannelManager::GetInstance().KickByAccount(kickAccountId)) { sawKick = true; }
                    continue;   // 같은 왕복의 RES를 마저 받는다 (TCP in-order)
                }
                if (res.sid != sid)
                {
                    // serial 모델이라 응답 sid 는 요청 sid 와 같아야 한다. 어긋나면 스트림 어긋남 - 링크 재수립.
                    RejectOrphanConnection(sid);   // 이 sid의 검증 RES는 오지 않음 - orphan이면 즉시 거부(pre-auth 35s hang 소멸)
                    return false;
                }
                DeliverVerifyResult(res, sawKick, token);   // token = 키 재료 (PendingSelect로 admission까지 운반)
                return true;
            }
        }

        // 검증 결과 처리 - 이 LoginLinkThread 스레드에서 직접 처리한다(채널0 mailbox/AuthResultJob 없음).
        //   OK면 PendingSelect 등록 + 채널 리스트 송신. 실제 admission(OnLoginSuccess + 게임세션 생성)과 DB 캐릭터 로드는
        //   캐릭터 선택 시점의 ProcessCharacterSelect 가 수행(=> DbResultJob이 AttachPlayer + SC_ENTER_WORLD).
        //   admission(IsCcuFull/OnLoginSuccess)은 m_sessionLock으로 게임채널 leave와 직렬화돼 스레드 안전.
        void LoginLinkThread::DeliverVerifyResult(const IS_TOKEN_VERIFY_RES& res, bool isReplacingKick, UINT64 token)
        {
            // 관측 카운터 (유지)
            const EVerifyResult result = static_cast<EVerifyResult>(res.result);
            switch (result)
            {
            case EVerifyResult::OK:             ::InterlockedIncrement(&s_verifyOk);            break;
            case EVerifyResult::ALREADY_ONLINE: ::InterlockedIncrement(&s_verifyAlreadyOnline); break;
            case EVerifyResult::NOT_FOUND:
            case EVerifyResult::EXPIRED:
            default:                            ::InterlockedIncrement(&s_verifyNotFound);      break;
            }

            if (result != EVerifyResult::OK)
            {
                // 검증 실패(토큰 없음/만료). Consume 실패라 LoginServer가 online 표에 안 올림 -> leave 통지 불요.
                //   단 같은 sid가 CS_GAME_AUTH를 2번 보내 2번째가 NOT_FOUND(토큰 일회용)로 온 경우,
                //   1번째로 이미 admit된 세션을 파괴하면 안 됨 -> map에 있으면 skip.
                //   추가: 채널 선택 대기(pending)면 정상 진행 중이므로 끊지 않는다(같은 연결의 중복 CS_GAME_AUTH가 pending을 파괴하던 버그 봉인).
                RejectOrphanConnection(res.sid);   // admitted도 pending도 아니면 즉시 FAIL+Disconnect (위 3경로와 공용 가드)
                return;
            }

            // === 채널 선택 - admission 보류, 채널 리스트 송신 ===
            //   verify OK = LoginServer가 online[accountId]=sid 커밋. 실제 admission(OnLoginSuccess + 게임세션 생성)은 채널/캐릭터
            //   선택을 거쳐 ProcessCharacterSelect에서 수행한다(ProcessChannelSelect는 선택 채널 보존 + 캐릭터 목록 push만).
            //   그 사이 끊기면 ProcessPendingGone이 online을 푼다(ghost 봉인).
            //   CCU/cap 게이트도 선택 시점으로 이동 - 선택 전엔 게임세션/인구 미반영이라 정합.
            PendingSelect ps;
            ps.accountId = res.accountId;
            ps.isReplacingKick = isReplacingKick;
            ps.enqueuedMs = ::GetTickCount64();   // reaper 무활동 TTL 최초 스탬프 (이후 채널/캐릭터 선택 활동마다 재스탬프)
            ps.lastChannelSelectMs = 0;          // 종류별 행동 쿨다운 기준 (0=최초 행동은 항상 통과 - now-0 는 항상 큼)
            ps.lastCharCreateMs    = 0;
            ps.lastCharDeleteMs    = 0;
            ps.awaitingSave = false;              // 캐릭터 선택 시점에 IsSaveReady로 판정
            ps.selectedChannelId = CHANNEL_ID_UNSELECTED;   // 채널 미선택 sentinel - CS_CHANNEL_SELECT 없이 CS_CHARACTER_SELECT 직송 시 IsChannelJoinable 거부(ch0 무단 우회 차단). ProcessChannelSelect가 실제 채널로 덮음
            ps.selectedCharId = 0;
            ps.token = token;   // 키 재료 보관 (캐릭터 선택 admission 시 SeedObfKeys로 소비)
            m_pendingSelect[res.sid] = ps;
            SendChannelList(res.sid);
        }

        // pre-auth 소켓에 거부 응답을 보내고 끊는다 (게임세션 미등록이라 ChannelManager의 raw 소켓 reject 사용).
        void LoginLinkThread::RejectLogin(UINT64 sid, BYTE loginResult, EDisconnectReason reason)
        {
            SC_LOGIN_RESULT res;
            res.result = loginResult;
            res.chId   = -1;   // 배정 채널 없음 (거부)
            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_LOGIN_RESULT));
            res.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            ChannelManager::GetInstance().RejectConnection(sid, out.GetBuffer(), out.GetSize(), reason);
        }

        // 이 sid가 admitted 세션도 pending도 아니면(= 이 연결에 정상 후속 상태가 없음) 즉시 거부+종료한다.
        //   링크 송수신 실패나 검증 NOT_FOUND 시 호출 - 안 하면 클라가 응답 없이 pre-auth reaper(게임 35s)까지 매달린다.
        //   admitted/pending이면 정상 진행 중이므로 건드리지 않는다(같은 연결의 후발 실패가 성립된 세션을 파괴하지 않게).
        void LoginLinkThread::RejectOrphanConnection(UINT64 sid)
        {
            ChannelManager& cm = ChannelManager::GetInstance();
            if (cm.GetSession(sid) == nullptr && m_pendingSelect.find(sid) == m_pendingSelect.end())
            {
                RejectLogin(sid, static_cast<BYTE>(ELoginResult::FAIL), EDisconnectReason::LOGIN_REJECTED);
            }
        }

        // 채널별 인구 SC_CHANNEL_LIST 송신 (verify OK 직후 / cap 거부 후 재선택용). 채널=스레드라 게임서버가 인구 직접 보유.
        //   패킷 조립은 ChannelManager::BuildChannelListPacket 이 단일 소스 (인게임 재조회 Channel::OnChannelListRequest 와 공용).
        void LoginLinkThread::SendChannelList(UINT64 sid)
        {
            ChannelManager& cm = ChannelManager::GetInstance();
            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            if (!cm.BuildChannelListPacket(out)) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }
            cm.SendRaw(sid, out.GetBuffer(), out.GetSize());   // pre-auth sid - lease Send (disconnect 없이)
        }

        // pending 행동 쿨다운 - 이 종류의 직전 시각(lastMs)과 now 간격이 min 미만이면 false(throttle), 통과 시 lastMs=now.
        //   종류별 독립 타임스탬프(Player::TryAct 미러)라 채널선택->캐릭터생성 같은 정상 교차-종류 시퀀스를 오throttle 하지 않는다.
        static bool AllowPendingAction(UINT64& lastMs, UINT64 now)
        {
            if (now - lastMs < PREAUTH_ACTION_MIN_INTERVAL_MS) { return false; }
            lastMs = now;
            return true;
        }

        // CS_CHANNEL_SELECT 수신 - 고른 채널을 보존하고 그 계정의 캐릭터 목록을 DB에서 읽어 push 한다(admission 아님).
        //   admission(OnLoginSuccess + GameSession 생성)은 캐릭터 선택 시점(ProcessCharacterSelect)으로 이동했다(B9).
        //   따라서 F17 게이트/CCU 게이트도 여기 두지 않는다 - 게임세션을 안 만드니 인구/저장과 무관.
        void LoginLinkThread::ProcessChannelSelect(UINT64 sid, BYTE channelId)
        {
            std::unordered_map<UINT64, PendingSelect>::iterator it = m_pendingSelect.find(sid);
            if (it == m_pendingSelect.end())
            {
                return;   // pending 없음 (verify 안 된 sid / 이미 처리) - 무시
            }
            const UINT64 now = ::GetTickCount64();
            if (!AllowPendingAction(it->second.lastChannelSelectMs, now))
            {
                return;   // 채널선택 도배 쿨다운(throttle). enqueuedMs 미갱신이라 침묵 취급 -> 결국 reaper 회수
            }
            it->second.enqueuedMs = now;         // reaper 무활동 TTL 리셋 (정상 활동 확인)
            it->second.selectedChannelId = channelId;   // 캐릭터 선택 시 이 채널로 admission
            // 캐릭터 목록 로드 -> SC_CHARACTER_LIST 로 클라에 push (LoadCharListCommand 가 raw 소켓 송신).
            DBThread::GetInstance().Enqueue(new LoadCharListCommand(sid, it->second.accountId));
        }

        // CS_CHARACTER_SELECT 수신 - F17 게이트 + CCU/채널 cap 검증 + admission(OnLoginSuccess) + 선택 캐릭터 Load.
        //   admission+Load 가 여기로 옮겨졌으므로 lost-update 봉인 게이트도 여기 있어야 한다(채널 선택엔 admission 없음).
        void LoginLinkThread::ProcessCharacterSelect(UINT64 sid, UINT32 charId)
        {
            std::unordered_map<UINT64, PendingSelect>::iterator it = m_pendingSelect.find(sid);
            if (it == m_pendingSelect.end())
            {
                return;   // pending 없음 (verify/채널선택 안 됨 / 이미 처리) - 무시
            }
            const PendingSelect ps = it->second;   // 값 복사 (아래서 erase 가능)
            ChannelManager& cm = ChannelManager::GetInstance();
            const int chId = static_cast<int>(ps.selectedChannelId);

            // F17: 중복 로그인 교체(kick)면 축출된 기존 세션의 종료 저장이 DB 큐에 들어갈 때까지 Load를 보류한다.
            //   단일 DBThread FIFO라 Save가 Load보다 먼저 enqueue되면 순서 보장 -> stale-load/lost-update 봉인.
            //   보류 시 선택 charId를 보존(RetryDeferredSelects가 save-ready 시 그 charId로 Load 재발행).
            if (ps.isReplacingKick && !cm.IsSaveReady(ps.accountId))
            {
                it->second.awaitingSave = true;
                it->second.selectedCharId = charId;
                return;
            }

            // CCU 게이트 (kick 후 신규는 1:1 교체라 net-zero -> 면제).
            if (!ps.isReplacingKick && cm.IsCcuFull())
            {
                m_pendingSelect.erase(it);
                EnqueueSessionGone(ps.accountId, sid);   // online 해제 (ghost 봉인)
                RejectLogin(sid, static_cast<BYTE>(ELoginResult::SERVER_FULL), EDisconnectReason::SERVER_FULL);
                return;
            }
            // chId 유효성 + per-channel cap 재검사. 무효/풀이면 종료(재접속 후 재선택) - CCU-full 경로와 통일.
            //   pending-유지 재리스트는 클라 화면 우선순위(캐릭터>채널)로 다른 채널 전환이 막혀 사실상 재선택 불가였고,
            //   submit 가드(pending 중 버튼 비활성)와 겹치면 soft-lock -> 거부+Disconnect 로 통일해 재접속이 arm/가드/화면을 리셋.
            if (!cm.IsChannelJoinable(chId))
            {
                m_pendingSelect.erase(it);
                EnqueueSessionGone(ps.accountId, sid);   // online 해제 (ghost 봉인)
                RejectLogin(sid, static_cast<BYTE>(ELoginResult::SERVER_FULL), EDisconnectReason::SERVER_FULL);
                return;
            }
            // admission - 선택 chId로 게임세션 등록 (post-insert liveness 재확인 내장. 끊김/풀고갈이면 nullptr).
            GameSession* gs = cm.OnLoginSuccess(sid, chId, ps.accountId, ps.token);   // token 동봉 - OnLoginSuccess가 publish 전 SeedObfKeys (seed-before-publish 원자)
            if (gs == nullptr)
            {
                m_pendingSelect.erase(it);
                EnqueueSessionGone(ps.accountId, sid);   // admission 무산 -> online 해제 (ghost 봉인)
                return;
            }
            // 선택 캐릭터 로드 (소유 검증 = Load WHERE account_id) - 캐릭터+인벤을 한 명령에서 로드(원자 로드).
            //   DbResultJob 하나가 AttachPlayer + 캐릭터/인벤 채움 + SC_ENTER_WORLD/SC_INVENTORY + spawn 을 원자 처리(부분 로드 창 봉인).
            DBThread::GetInstance().Enqueue(new LoadCharacterCommand(sid, chId, charId, ps.accountId, cm.GetChannel(chId)));
            m_pendingSelect.erase(it);
        }

        // CS_CHARACTER_CREATE 수신 - 인증된 pending의 계정으로 캐릭터 생성 명령 발행(결과는 raw 소켓 회신).
        void LoginLinkThread::ProcessCharacterCreate(UINT64 sid, const wchar_t* name, BYTE slotId)
        {
            std::unordered_map<UINT64, PendingSelect>::iterator it = m_pendingSelect.find(sid);
            if (it == m_pendingSelect.end())
            {
                return;   // 인증 안 된 sid (pending 없음) - 무시 (계정 위조 차단: accountId는 pending에서만)
            }
            const UINT64 now = ::GetTickCount64();
            if (!AllowPendingAction(it->second.lastCharCreateMs, now))
            {
                return;   // 캐릭터 생성 도배 쿨다운(DB 위임 스팸 억제). 종류별 독립이라 직전 채널선택엔 안 걸림
            }
            it->second.enqueuedMs = now;         // reaper 무활동 TTL 리셋
            DBThread::GetInstance().Enqueue(new CreateCharCommand(sid, it->second.accountId, name, slotId));
        }

        // CS_CHARACTER_DELETE 수신 - 인증된 pending의 계정으로 캐릭터 삭제 명령 발행(소유 검증은 DeleteCharacter WHERE account_id).
        void LoginLinkThread::ProcessCharacterDelete(UINT64 sid, UINT32 charId)
        {
            std::unordered_map<UINT64, PendingSelect>::iterator it = m_pendingSelect.find(sid);
            if (it == m_pendingSelect.end())
            {
                return;   // 인증 안 된 sid - 무시
            }
            const UINT64 now = ::GetTickCount64();
            if (!AllowPendingAction(it->second.lastCharDeleteMs, now))
            {
                return;   // 캐릭터 삭제 도배 쿨다운. 종류별 독립이라 직전 채널선택/생성엔 안 걸림
            }
            it->second.enqueuedMs = now;         // reaper 무활동 TTL 리셋
            DBThread::GetInstance().Enqueue(new DeleteCharCommand(sid, it->second.accountId, charId));
        }

        // pre-auth 끊김 - 채널 선택 대기(pending) 중이었으면 online을 풀고 정리한다 (verify 시 LoginServer가 online 커밋함).
        void LoginLinkThread::ProcessPendingGone(UINT64 sid)
        {
            std::unordered_map<UINT64, PendingSelect>::iterator it = m_pendingSelect.find(sid);
            if (it == m_pendingSelect.end())
            {
                return;   // pending 아님 (verify 전 끊김 / 이미 admission 됨) - no-op
            }
            const UINT32 accountId = it->second.accountId;
            m_pendingSelect.erase(it);
            EnqueueSessionGone(accountId, sid);   // online 해제 (ghost 봉인)
        }

        // 중복 로그인 축출 - 채널 선택 대기(pending) 중인 같은 계정 세션을 끊는다 (KickByAccount가 같은 링크 스레드에서 호출).
        //   admission된 세션은 KickByAccount의 m_accountToSid 경로가 처리. pending은 admission 전이라 거기 안 잡혀 여기서 별도 처리.
        bool LoginLinkThread::KickPending(UINT32 accountId)
        {
            bool kicked = false;
            for (std::unordered_map<UINT64, PendingSelect>::iterator it = m_pendingSelect.begin(); it != m_pendingSelect.end(); )
            {
                if (it->second.accountId == accountId)
                {
                    const UINT64 sid = it->first;
                    it = m_pendingSelect.erase(it);   // 먼저 erase (ProcessPendingGone 재진입 무해화)
                    ChannelManager::GetInstance().DisconnectPending(sid, EDisconnectReason::NET_RESET);   // 축출 = 소켓 종료 (Send 없이 - WSASend/closesocket race 회피)
                    EnqueueSessionGone(accountId, sid);   // online 해제 (신규가 곧 덮음, sid 동봉 현 holder 검증)
                    kicked = true;
                }
                else
                {
                    ++it;
                }
            }
            return kicked;   // 실제 pending 축출 여부 (KickByAccount 의 실-victim 판정에 합류)
        }

        // 권위 online 집합(admission된 m_accountToSid + 채널 선택 대기 m_pendingSelect)을 주기적으로 로그인 서버에 push 한다.
        //   둘 다 넣어야 pending(채널 선택 화면) 플레이어가 false-expire 안 됨. 로그인 서버는 받은 계정의 lastSyncMs 를 갱신하고,
        //   여기 안 담긴 ghost(RES 유실, LEAVE 드롭, 링크끊김, shutdown)는 grace 후 SweepOnline 이 정리한다(수렴 backstop).
        void LoginLinkThread::MaybeSendOnlineSnapshot()
        {
            const UINT64 now = ::GetTickCount64();
            if (now - m_lastSyncMs < ONLINE_SYNC_INTERVAL_MS) { return; }   // 간격 제한
            m_lastSyncMs = now;

            std::vector<UINT32> accountIds;
            std::vector<UINT64> sids;
            for (std::unordered_map<UINT64, PendingSelect>::iterator it = m_pendingSelect.begin(); it != m_pendingSelect.end(); ++it)
            {
                accountIds.push_back(it->second.accountId);   // pending (이 스레드 단독 소유, 락 0)
                sids.push_back(it->first);
            }
            ChannelManager::GetInstance().AppendOnlineAccounts(accountIds, sids);   // admitted (m_accountToSid, read 락 내)

            // MAX_ENTRIES 단위로 청크해 송신 (RECV_BUFFER_SIZE 한 프레임 한도). 빈 집합이면 송신 0(sweep 이 stale 정리).
            const size_t total = accountIds.size();
            size_t i = 0;
            while (i < total)
            {
                IS_ONLINE_SYNC chunk{};   // 앞 n칸만 채우므로 나머지가 스택 잔재를 wire 로 흘리지 않게 0으로
                USHORT n = 0;
                while (i < total && n < IS_ONLINE_SYNC::MAX_ENTRIES)
                {
                    chunk.accountIds[n] = accountIds[i];
                    chunk.sids[n]       = sids[i];
                    ++n; ++i;
                }
                chunk.count = n;
                m_client.SendOnlineSnapshot(chunk);   // fire-and-forget (실패해도 다음 주기가 backstop)
            }
        }

        // TTL 초과한 pending 정리 - 채널 선택을 무한정 안 하는(멈춘/악의) 좀비가 online, 소켓을 점유하는 것 방지.
        //   정상 끊김은 ProcessPendingGone, 재로그인은 KickPending이 처리하므로 여기 걸리는 건 'TCP 살아있는데 선택 안 함'뿐.
        void LoginLinkThread::ReapPendingSelect()
        {
            const UINT64 now = ::GetTickCount64();
            if (now - m_lastReapMs < PENDING_REAP_INTERVAL_MS) { return; }   // 간격 제한 (전수 스캔 빈도 억제)
            m_lastReapMs = now;
            for (std::unordered_map<UINT64, PendingSelect>::iterator it = m_pendingSelect.begin(); it != m_pendingSelect.end(); )
            {
                if (now - it->second.enqueuedMs >= PENDING_SELECT_TIMEOUT_MS)
                {
                    const UINT64 sid = it->first;
                    const UINT32 accountId = it->second.accountId;
                    it = m_pendingSelect.erase(it);
                    ChannelManager::GetInstance().DisconnectPending(sid, EDisconnectReason::IDLE_TIMEOUT);   // 선택 시간초과 = 종료 (Send 없이 - race 회피)
                    EnqueueSessionGone(accountId, sid);   // online 해제
                }
                else
                {
                    ++it;
                }
            }
        }

        // 종료 저장 대기(awaitingSave) pending을 save-ready 시 admission 완료한다 (ThreadLoop가 매 반복 호출).
        //   축출된 기존 세션의 종료 저장이 enqueue되면(IsSaveReady) 보류했던 신규 로그인의 채널 진입 + Load를 진행한다.
        //   m_pendingSelect는 링크 스레드 단독 소유라 락 0. erase-while-iterate 안전 패턴(ReapPendingSelect 미러).
        void LoginLinkThread::RetryDeferredSelects()
        {
            ChannelManager& cm = ChannelManager::GetInstance();
            for (std::unordered_map<UINT64, PendingSelect>::iterator it = m_pendingSelect.begin(); it != m_pendingSelect.end(); )
            {
                if (!it->second.awaitingSave || !cm.IsSaveReady(it->second.accountId))
                {
                    ++it;
                    continue;   // 보류 아님 / 아직 저장 안 끝남
                }
                const UINT64 sid = it->first;
                const PendingSelect ps = it->second;   // 값 복사
                const int chId = static_cast<int>(ps.selectedChannelId);

                // 채널 cap 재검 (defer 동안 변동 가능). 풀이면 종료(재접속 후 재선택) - ProcessCharacterSelect 채널 만석 경로와 통일.
                if (!cm.IsChannelJoinable(chId))
                {
                    EnqueueSessionGone(ps.accountId, sid);   // online 해제 (ghost 봉인)
                    RejectLogin(sid, static_cast<BYTE>(ELoginResult::SERVER_FULL), EDisconnectReason::SERVER_FULL);
                    it = m_pendingSelect.erase(it);
                    continue;
                }
                // admission (post-insert liveness 재확인 내장)
                GameSession* gs = cm.OnLoginSuccess(sid, chId, ps.accountId, ps.token);   // token 동봉 - OnLoginSuccess가 publish 전 SeedObfKeys (seed-before-publish 원자)
                it = m_pendingSelect.erase(it);   // 완료 - pending 제거 (admit 성패 무관)
                if (gs == nullptr)
                {
                    EnqueueSessionGone(ps.accountId, sid);   // admission 무산 -> online 해제 (ghost 봉인)
                    continue;
                }
                DBThread::GetInstance().Enqueue(new LoadCharacterCommand(sid, chId, ps.selectedCharId, ps.accountId, cm.GetChannel(chId)));   // Save 뒤에 enqueue (FIFO 순서) - 보존한 charId로 재발행 (캐릭터+인벤 한 명령, 원자 로드)
            }
        }

        void LoginLinkThread::ThreadLoop()
        {
            // 이 스레드 전용 Winsock 초기화(refcount 안전 - IOCPServer 의 WSAStartup 과 독립). WSACleanup 와 짝.
            WSADATA wsa;
            if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            {
                m_connectOk = false;
                ::SetEvent(m_connectDone);   // Start 가 무한 대기하지 않게
                return;
            }

            bool  firstSignaled = false;
            DWORD backoffMs = LINK_INITIAL_BACKOFF_MS;

            while (m_running)
            {
                if (!m_client.IsConnected())
                {
                    const bool linkUp = ConnectAndRegister();
                    if (!firstSignaled)
                    {
                        m_connectOk = linkUp;        // 첫 시도 결과를 Start 에 신호
                        ::SetEvent(m_connectDone);
                        firstSignaled = true;
                    }
                    if (!linkUp)
                    {
                        ::InterlockedExchange(&m_linkUp, 0);
                        // 백오프 후 재시도 (stop 이벤트로 즉시 중단 가능).
                        if (::WaitForSingleObject(m_stopEvent, backoffMs) == WAIT_OBJECT_0) { break; }
                        backoffMs = (backoffMs * 2 <= LINK_MAX_BACKOFF_MS) ? (backoffMs * 2) : LINK_MAX_BACKOFF_MS;
                        continue;
                    }
                    ::InterlockedExchange(&m_linkUp, 1);
                    backoffMs = LINK_INITIAL_BACKOFF_MS;   // 연결 성공 - 백오프 리셋
                    KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                        KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"link",
                        L"인터서버 링크 연결됨");
                }

                ReapPendingSelect();      // 링크 정상 구간마다 pending TTL 점검 (내부 간격 제한)
                RetryDeferredSelects();   // 종료 저장 대기 pending을 save-ready 시 admission 완료 (lost-update 봉인)
                MaybeSendOnlineSnapshot();   // 주기적 권위 online 스냅샷 push (online ghost 수렴, 내부 간격 제한)

                LinkRequest req;
                if (PopRequest(req))
                {
                    bool linkOk = true;
                    if (req.kind == ELinkReqKind::VERIFY)
                    {
                        linkOk = ProcessVerify(req.sid, req.token);
                        if (!linkOk) { ::InterlockedIncrement(&s_verifyError); }
                    }
                    else if (req.kind == ELinkReqKind::LEAVE)   // fire-and-forget(응답 없음)
                    {
                        linkOk = m_client.SendSessionGone(req.accountId, req.sid);
                    }
                    else if (req.kind == ELinkReqKind::CHANNEL_SELECT)   // 로컬 (링크 송신 없음) - 채널 보존 + 캐릭터 목록 load
                    {
                        ProcessChannelSelect(req.sid, req.channelId);
                    }
                    else if (req.kind == ELinkReqKind::CHARACTER_SELECT)   // 로컬 admission + 선택 캐릭터 Load
                    {
                        ProcessCharacterSelect(req.sid, req.charId);
                    }
                    else if (req.kind == ELinkReqKind::CHARACTER_CREATE)   // 로컬 - 캐릭터 생성 명령 발행
                    {
                        ProcessCharacterCreate(req.sid, req.name, req.slotId);
                    }
                    else if (req.kind == ELinkReqKind::CHARACTER_DELETE)   // 로컬 - 캐릭터 삭제 명령 발행
                    {
                        ProcessCharacterDelete(req.sid, req.charId);
                    }
                    else   // PENDING_GONE - pending 정리 + online 해제(로컬, EnqueueSessionGone이 LEAVE로 큐잉)
                    {
                        ProcessPendingGone(req.sid);
                    }
                    if (!linkOk)
                    {
                        m_client.Close();                       // 링크 끊김 - 다음 반복에서 재연결
                        ::InterlockedExchange(&m_linkUp, 0);
                        KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                            KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_WARN, L"link",
                            L"인터서버 링크 끊김 - 재연결 예정");
                    }
                }
                else
                {
                    // 큐 빔 - 짧게 대기(stop 이벤트로 즉시 깸). 바쁜 대기 회피(DBThread Sleep(1) 정합).
                    if (::WaitForSingleObject(m_stopEvent, 1) == WAIT_OBJECT_0) { break; }
                }
            }

            // 종료 drain - main 이 gameServer.Stop()(세션 teardown -> LEAVE 생성)을 loginLink.Stop() 앞에 두므로,
            //   루프를 빠져나오기 직전 링크가 살아있을 때 큐에 남은 종료 통지를 마저 보낸다(정상 종료 시 전 인구 online 즉시 해제, grace sweep 없이도).
            if (m_client.IsConnected())
            {
                LinkRequest dreq;
                while (PopRequest(dreq))
                {
                    if (dreq.kind == ELinkReqKind::LEAVE) { m_client.SendSessionGone(dreq.accountId, dreq.sid); }
                    else if (dreq.kind == ELinkReqKind::PENDING_GONE) { ProcessPendingGone(dreq.sid); }   // online 해제 -> LEAVE 재큐잉 -> 다음 PopRequest서 비움
                    // VERIFY / CHANNEL_SELECT 는 종료 시점에 무의미 - 버림
                }
            }

            m_client.Close();
            ::InterlockedExchange(&m_linkUp, 0);
            if (!firstSignaled)   // m_running 이 첫 시도 전에 false 가 되어도 Start 가 안 멈추게.
            {
                m_connectOk = false;
                ::SetEvent(m_connectDone);
            }
            ::WSACleanup();
        }
    }
}
