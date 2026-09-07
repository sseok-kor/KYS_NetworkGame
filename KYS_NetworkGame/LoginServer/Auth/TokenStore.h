#pragma once
#include "Types/Defines.h"                              // UINT32 / UINT64
#include "../GameCommon/GameDefines.h"                  // WHISPER_NAME_MAX, LOGIN_TOKEN_TTL_MS (호출자가 Put 에 넘기는 기본 TTL)
#include "../GameServer/Core/Thread/SRWLockWrapper.h"   // 동기화 (read=공유 조회 / write=적재, 소비)
#include <unordered_map>

namespace KYS
{
    namespace LOGINSERVER
    {
        // 발급된 일회용 로그인 토큰 보관소.
        //   인증 통과 계정에 토큰을 발급해 여기 적재(Put)하고, 게임 서버의 검증 질의에서 한 번만 꺼내(Consume) 소멸시킨다.
        //   클라 대면 로그인 스레드 + 인터서버 스레드가 함께 접근하므로 SRWLock 으로 보호한다.
        //   - m_tokens : 단명(TTL) 토큰 표. 만료되면 SweepExpired 가 청소.
        //   - m_online : 접속 중 계정 집합(중복 로그인 차단). 인터서버 검증 시점에 PutOnline/RemoveOnline 로 채우고 비운다.
        class TokenStore
        {
        public:
            static TokenStore& GetInstance() { static TokenStore instance; return instance; }   // Meyers static

            TokenStore(const TokenStore&) = delete;            // 복사 2줄 (단일 소유, 이동은 자동 미선언)
            TokenStore& operator=(const TokenStore&) = delete;

            // Consume 가 돌려주는 계정 식별 (게임 서버가 캐릭터 로드/세션 인증에 사용 - 인터서버 토큰 검증 응답 IS_TOKEN_VERIFY_RES 운반값).
            struct TokenInfo
            {
                UINT32  accountId;                       // 토큰 주인 계정 번호
                wchar_t loginName[WHISPER_NAME_MAX];     // 계정 로그인 이름 (인터서버 응답에 동봉하는 표시용 - 캐릭터 로드 키는 charId+accountId 다)
            };

            // 토큰 적재. now + ttlMs 를 만료 시각으로 박는다. 같은 토큰 키가 또 오면 마지막으로 덮음(충돌 회피는 호출자 책임).
            //   ttlMs 는 보통 LOGIN_TOKEN_TTL_MS 를 넘긴다.
            void Put(UINT64 token, UINT32 accountId, const wchar_t* loginName, UINT64 ttlMs);

            // 토큰 1회 소비. 존재 + 미만료면 out 채우고 erase 후 true(일회용). 없거나 만료면 false.
            bool Consume(UINT64 token, TokenInfo& out);

            // 중복 로그인 추적 online 표 (accountId -> 현 세션 sid). kick 정책: 신규는 항상 수용하되 이미 접속 중이면 기존을 축출.
            //   PutOnline: online[accountId]=sid 로 갈아끼우고, 이미 있었으면 true(=호출자가 IS_KICK 으로 기존 세션 축출).
            bool PutOnline(UINT32 accountId, UINT64 sid);
            //   RemoveOnline: online[accountId]==sid 일 때만 제거(현 holder 의 leave 만 반영 - 같은 계정 재접속의 stale leave ghost 봉인).
            void RemoveOnline(UINT32 accountId, UINT64 sid);
            //   RefreshOnline: 게임 권위 스냅샷(IS_ONLINE_SYNC)으로 online[accountId]={sid,now} upsert(미존재면 생성=under-count 보정, sid 다르면 게임 권위로 정정, lastSyncMs 갱신).
            void RefreshOnline(UINT32 accountId, UINT64 sid, UINT64 nowMs);
            //   SweepOnline: lastSyncMs 가 graceMs 넘게 미갱신된 online 엔트리(ghost) 제거 - 스냅샷에 안 담긴 ghost(RES 유실, LEAVE 드롭, 링크끊김, shutdown)의 통합 backstop.
            //   now 는 락 안에서 읽는다(인자로 받지 않음) - 동시 RefreshOnline 이 락 밖 stale now 보다 늦은 stamp 를 찍어 산 엔트리를 언더플로로 오삭제하는 것 방지.
            void SweepOnline(UINT64 graceMs);

            // 만료된 토큰 일괄 제거 (주기 호출). nowMs >= expiryMs 인 항목 삭제.
            void SweepExpired(UINT64 nowMs);

            int GetTokenCount();    // 현재 미소비 토큰 수 (모니터)
            int GetOnlineCount();   // 현재 접속 중 계정 수 (모니터, kick 진단 - ServerApp CCU와 대조)

        private:
            TokenStore() = default;      // 싱글턴 - GetInstance 로만 생성
            ~TokenStore() = default;

            struct TokenEntry
            {
                UINT32  accountId;
                wchar_t loginName[WHISPER_NAME_MAX];
                UINT64  expiryMs;                        // 이 시각(GetTickCount64 기준 ms)을 지나면 만료
            };

            struct OnlineEntry
            {
                UINT64 sid;          // 현 holder 게임 세션 sid (값일치 해제, kick 역인덱스 키)
                UINT64 lastSyncMs;   // 마지막 갱신 시각(PutOnline commit 또는 IS_ONLINE_SYNC refresh). 이 시각 + grace 넘게 미갱신이면 ghost 로 sweep.
            };

            std::unordered_map<UINT64, TokenEntry>  m_tokens;    // token -> 발급 내역
            std::unordered_map<UINT32, OnlineEntry> m_online;    // accountId -> 현 세션 (중복 로그인 추적, kick, grace sweep 수렴)
            KYS::GAMESERVER::THREAD::SRWLockWrapper m_lock;     // m_tokens / m_online 보호
        };
    }
}
