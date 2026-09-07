#pragma once
#include "LoginLinkClient.h"
#include "../../GameServer/Core/Thread/SpinLock.h"   // 요청 큐 보호 (게임 스레드 producer <-> 링크 스레드 consumer)
#include <Windows.h>                                  // HANDLE
#include <queue>
#include <unordered_map>

namespace KYS
{
    namespace SERVERAPP
    {
        // 인터서버 링크 전용 스레드. 게임 스레드(30Hz tick)를 절대 막지 않으려고 토큰 검증/접속종료 통지를
        //   이 스레드로 오프로드한다(DBThread 모델 미러 - blocking 소켓 + m_connectDone fail-fast 신호 + producer/consumer 큐).
        //   ThreadLoop 은: 로그인 서버 연결 + 링크 등록(IS_REGISTER) -> 요청 큐를 비우며 검증은 serial 1왕복(send REQ -> recv RES),
        //   접속종료는 fire-and-forget. 링크가 끊기면 지수 백오프로 재연결한다(로그인 서버 재시작이 게임 서버를 죽이지 않음).
        class LoginLinkThread
        {
        public:
            static LoginLinkThread& GetInstance() { static LoginLinkThread instance; return instance; }   // Meyers static (DBThread 미러)

            LoginLinkThread(const LoginLinkThread&) = delete;            // 복사 2줄 (스레드/소켓/큐 단일 소유, 이동 자동 미선언)
            LoginLinkThread& operator=(const LoginLinkThread&) = delete;

            // 링크 스레드 기동. 연결 대상(로그인 서버 인터서버 주소) + 자기 게임 listen 주소(등록에 실음)를 받는다.
            //   첫 연결 시도 결과를 m_connectDone 으로 신호하고 그 값을 반환한다(링크 다운이라도 boot 는 비치명 - 백그라운드 재연결).
            bool Start(UINT32 loginServerIp, USHORT loginServerPort,
                       USHORT serverId, UINT32 listenIp, USHORT listenPort, UINT32 authSecret);
            void Stop();   // m_running=false + stop 이벤트 신호 + join + 핸들 정리

            // producer (게임 스레드) - 요청을 큐에 넣고 즉시 반환(인라인 소켓 I/O 금지).
            void EnqueueVerify(UINT64 sid, UINT64 token);            // 토큰 검증 요청
            void EnqueueSessionGone(UINT32 accountId, UINT64 sid);  // 접속 종료 통지 (sid 동봉 - 현 holder 검증)
            void EnqueueChannelSelect(UINT64 sid, BYTE channelId);  // 채널 선택 수신 (worker -> 링크 스레드, 캐릭터 목록 load)
            void EnqueueCharacterSelect(UINT64 sid, UINT32 charId);                  // 캐릭터 선택 -> admission + 선택 캐릭터 Load
            void EnqueueCharacterCreate(UINT64 sid, const wchar_t* name, BYTE slotId);  // 캐릭터 생성
            void EnqueueCharacterDelete(UINT64 sid, UINT32 charId);                  // 캐릭터 삭제
            void EnqueuePendingGone(UINT64 sid);                    // pre-auth 끊김 (pending 정리 + online 해제)
            bool KickPending(UINT32 accountId);                     // 중복 로그인 - 채널 선택 대기(pending) 중인 기존 세션 축출. 반환=실제 pending 축출 여부 (KickByAccount가 같은 링크 스레드에서 호출)

            bool IsLinkUp() const;   // 링크 연결 여부 힌트 (관측용)

            void ThreadLoop();   // 스레드 본체 (file-local 진입 함수가 호출 - DBThread 정합)

            // e2e 관측 카운터 (self-test 가 읽음). 워커/링크 스레드 갱신 -> Interlocked.
            static volatile LONG s_verifyOk;             // 검증 OK 응답 수
            static volatile LONG s_verifyNotFound;       // 토큰 없음/만료 응답 수
            static volatile LONG s_verifyAlreadyOnline;  // 중복 로그인 거부 응답 수
            static volatile LONG s_verifyError;          // 송수신 실패(링크 끊김/타임아웃) 수
            static volatile LONG s_linkQueueDrop;        // 큐 cap 초과로 유계 drop 된 클라 구동 요청 수 (flood 방어 관측 - silent-cap 규약)

        private:
            LoginLinkThread();
            ~LoginLinkThread();

            // 한 요청의 종류와 인자(검증=sid/token, 종료통지=accountId, 캐릭터=charId/name/slotId).
            enum class ELinkReqKind { VERIFY, LEAVE, CHANNEL_SELECT, PENDING_GONE,
                                      CHARACTER_SELECT, CHARACTER_CREATE, CHARACTER_DELETE };
            struct LinkRequest
            {
                ELinkReqKind kind;
                UINT64       sid;        // 공통: 게임 세션 sid(상관 키)
                UINT64       token;      // VERIFY: 검증할 토큰
                UINT32       accountId;  // LEAVE: 해제할 계정 번호
                BYTE         channelId;  // CHANNEL_SELECT: 유저가 고른 채널
                UINT32       charId;     // CHARACTER_SELECT/DELETE: 대상 캐릭터 PK
                wchar_t      name[WHISPER_NAME_MAX];   // CHARACTER_CREATE: 새 캐릭터 이름 (값 복사)
                BYTE         slotId;     // CHARACTER_CREATE: 슬롯 번호
            };
            // 채널 선택 대기(verify OK ~ 캐릭터 선택 사이) 상태 - admission에 필요한 정보 보관 (링크 스레드 전용).
            struct PendingSelect
            {
                UINT32  accountId;
                bool    isReplacingKick;
                UINT64  enqueuedMs;   // pending 마지막 활동 시각(GetTickCount64) - reaper 무활동 TTL 판정용. 등록 시 최초 스탬프, 채널/캐릭터 선택 등 활동마다 재스탬프
                UINT64  lastChannelSelectMs;   // 직전 채널선택 시각(GetTickCount64) - 종류별 독립 쿨다운(Player::TryAct 미러). 0=최초(첫 행동 통과). 종류 분리라 채널선택->캐릭터생성 정상 시퀀스를 오throttle 안 함
                UINT64  lastCharCreateMs;      // 직전 캐릭터 생성 시각 - 생성 도배 쿨다운
                UINT64  lastCharDeleteMs;      // 직전 캐릭터 삭제 시각 - 삭제 도배 쿨다운
                bool    awaitingSave;       // 축출된 기존 세션의 종료 저장 대기 중(Load defer) - true면 RetryDeferredSelects가 save-ready 시 admission 완료
                BYTE    selectedChannelId;  // 클라가 고른 채널 (캐릭터 선택 시 이 채널로 admission)
                UINT32  selectedCharId;     // F17 defer 중 보존하는 선택 캐릭터 PK (save-ready 시 이 char_id로 Load 재발행)
                UINT64  token;              // opcode 난독화 키 재료 (verify -> admission 운반, SeedObfKeys로 소비). 절대 로그 금지.
            };

            bool ConnectAndRegister();                          // 연결 + IS_REGISTER 핸드셰이크. 성공 true
            bool ProcessVerify(UINT64 sid, UINT64 token);       // 검증 1왕복(send REQ -> recv RES -> Deliver). 링크 정상 true
            void DeliverVerifyResult(const IS_TOKEN_VERIFY_RES& res, bool isReplacingKick, UINT64 token);   // 검증 결과 처리. OK면 pending 등록(token=키 재료 운반) + SC_CHANNEL_LIST 송신(admission은 선택 후)
            void ProcessChannelSelect(UINT64 sid, BYTE channelId);   // 채널 선택 -> 채널 보존 + 캐릭터 목록 load (admission 아님)
            void ProcessCharacterSelect(UINT64 sid, UINT32 charId);  // 캐릭터 선택 -> F17 게이트 + CCU/cap 검증 + admission + 선택 캐릭터 Load
            void ProcessCharacterCreate(UINT64 sid, const wchar_t* name, BYTE slotId);   // 캐릭터 생성 명령 발행
            void ProcessCharacterDelete(UINT64 sid, UINT32 charId);  // 캐릭터 삭제 명령 발행
            void ProcessPendingGone(UINT64 sid);                     // pending 정리 + online 해제(EnqueueSessionGone)
            void SendChannelList(UINT64 sid);                        // 채널별 인구 SC_CHANNEL_LIST 송신
            void ReapPendingSelect();                                // TTL 초과한 pending(멈춘/악의 좀비) 정리 - ThreadLoop가 주기 호출
            void RetryDeferredSelects();                             // 종료 저장 대기(awaitingSave) pending을 save-ready 시 admission 완료 - ThreadLoop가 주기 호출 (lost-update 봉인)
            void MaybeSendOnlineSnapshot();                          // 주기적 권위 online 집합(admitted+pending) 스냅샷 push - online ghost 수렴 (ThreadLoop가 주기 호출, 내부 간격 제한)
            void RejectLogin(UINT64 sid, BYTE loginResult, EDisconnectReason reason);   // pre-auth 소켓에 SC_LOGIN_RESULT 거부 + Disconnect
            void RejectOrphanConnection(UINT64 sid);   // sid가 admitted도 pending도 아니면(정상 후속 없음) 즉시 FAIL+Disconnect - 링크/검증 실패 시 pre-auth reaper(35s) hang 소멸
            bool PopRequest(LinkRequest& out);                  // 큐에서 1개 꺼냄(없으면 false)
            bool TryPushRequest(const LinkRequest& req);        // 클라 구동 요청 push - 큐 cap 초과면 유계 drop(flood 방어). cleanup(LEAVE/PENDING_GONE)은 게이트 우회(항상 적재)

            LoginLinkClient m_client;   // raw 소켓 전송 계층 (이 스레드만 만짐 - 자체 락 없음)

            std::queue<LinkRequest>          m_requests;   // 요청 큐 (producer 게임 스레드 <-> consumer 링크 스레드)
            KYS::GAMESERVER::THREAD::SpinLock m_queueLock; // m_requests 보호
            std::unordered_map<UINT64, PendingSelect> m_pendingSelect;   // 채널 선택 대기 (링크 스레드 단독 소유, 락 0)
            UINT64 m_lastReapMs;   // 마지막 pending reaper 점검 시각 (간격 제한)
            UINT64 m_lastSyncMs;   // 마지막 online 스냅샷 송신 시각 (간격 제한, ONLINE_SYNC_INTERVAL_MS)

            HANDLE m_thread;        // _beginthreadex 핸들 (Stop 에서 join/close)
            HANDLE m_connectDone;   // 첫 연결 결과 신호 (Start 가 대기)
            HANDLE m_stopEvent;     // 종료/백오프 깨우기 (재연결 대기를 즉시 끊음)

            volatile bool m_connectOk;   // 첫 연결 시도 성공 여부 (Start 반환값)
            volatile bool m_running;     // ThreadLoop 종료 플래그
            volatile LONG m_linkUp;      // 링크 연결 상태 (1=연결, 0=끊김) - IsLinkUp 관측

            // 연결 대상 + 자기 listen 주소 (Start 가 받아 ThreadLoop 가 사용)
            UINT32 m_loginServerIp;
            USHORT m_loginServerPort;
            USHORT m_serverId;
            UINT32 m_listenIp;
            USHORT m_listenPort;
            UINT32 m_authSecret;   // 인터서버 공유 비밀 (conf/Server_Config.ini 의 [interserver] secret 에서 Start로 주입) - IS_REGISTER에 실어 보냄
        };
    }
}
