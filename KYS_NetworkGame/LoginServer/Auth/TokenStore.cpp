#include "pch_loginserver.h"
#include "TokenStore.h"
#include <cwchar>   // wcsncpy_s

namespace KYS
{
    namespace LOGINSERVER
    {
        using KYS::GAMESERVER::THREAD::SRWWriteGuard;

        // 토큰 적재. 만료 시각 = 지금(GetTickCount64) + ttlMs.
        void TokenStore::Put(UINT64 token, UINT32 accountId, const wchar_t* loginName, UINT64 ttlMs)
        {
            SRWWriteGuard guard(m_lock);

            TokenEntry entry;
            entry.accountId = accountId;
            entry.expiryMs  = ::GetTickCount64() + ttlMs;

            // 고정 배열로 안전 복사(널 종단 보장). null 이면 빈 문자열.
            entry.loginName[0] = L'\0';
            if (loginName != nullptr)
            {
                wcsncpy_s(entry.loginName, WHISPER_NAME_MAX, loginName, _TRUNCATE);
            }

            m_tokens[token] = entry;   // 같은 토큰 키면 덮음
        }

        // 토큰 1회 소비. 미만료면 out 채우고 erase 후 true(일회용).
        bool TokenStore::Consume(UINT64 token, TokenInfo& out)
        {
            SRWWriteGuard guard(m_lock);

            const std::unordered_map<UINT64, TokenEntry>::iterator it = m_tokens.find(token);
            if (it == m_tokens.end())
            {
                return false;   // 토큰 없음 (위조 또는 이미 소비)
            }

            const UINT64 nowMs = ::GetTickCount64();
            if (nowMs >= it->second.expiryMs)
            {
                m_tokens.erase(it);   // 만료 토큰은 꺼내면서 같이 청소
                return false;
            }

            out.accountId = it->second.accountId;
            wcsncpy_s(out.loginName, WHISPER_NAME_MAX, it->second.loginName, _TRUNCATE);

            m_tokens.erase(it);   // 일회용 - 소비 즉시 제거
            return true;
        }

        // online[accountId]를 sid로 갈아끼운다. 이미 접속 중이었으면 true(호출자가 IS_KICK으로 기존 세션 축출 후 신규 수용).
        bool TokenStore::PutOnline(UINT32 accountId, UINT64 sid)
        {
            SRWWriteGuard guard(m_lock);
            std::unordered_map<UINT32, OnlineEntry>::iterator it = m_online.find(accountId);
            const bool wasOnline = (it != m_online.end());
            OnlineEntry e; e.sid = sid; e.lastSyncMs = GetTickCount64();   // 신규가 현 holder (insert 또는 overwrite) + 갱신시각 박음(grace sweep 기준)
            m_online[accountId] = e;
            return wasOnline;            // true => 기존 세션 kick 필요
        }

        // 현 holder(online[accountId]==sid)일 때만 제거. 다른 sid면 신규가 갈아탄 것이라 보존(stale leave ghost 봉인).
        void TokenStore::RemoveOnline(UINT32 accountId, UINT64 sid)
        {
            SRWWriteGuard guard(m_lock);
            std::unordered_map<UINT32, OnlineEntry>::iterator it = m_online.find(accountId);
            if (it != m_online.end() && it->second.sid == sid)
            {
                m_online.erase(it);
            }
        }

        // 게임 권위 스냅샷(IS_ONLINE_SYNC)으로 online 엔트리 갱신(upsert). 미존재면 생성(게임은 online인데 로그인이 모르는 under-count 보정),
        //   sid 다르면 게임 권위로 정정. lastSyncMs 를 now 로 박아 grace sweep 에서 살아남게 한다.
        void TokenStore::RefreshOnline(UINT32 accountId, UINT64 sid, UINT64 nowMs)
        {
            SRWWriteGuard guard(m_lock);
            OnlineEntry e; e.sid = sid; e.lastSyncMs = nowMs;
            m_online[accountId] = e;
        }

        // grace 넘게 미갱신된 online 엔트리(ghost) 제거. 스냅샷에 안 담긴 엔트리(RES 유실, LEAVE 드롭, 링크끊김, shutdown)는
        //   더 이상 refresh 를 못 받아 lastSyncMs 가 늙으므로 여기서 통합 정리된다(erase 가 다음 반복자 반환).
        void TokenStore::SweepOnline(UINT64 graceMs)
        {
            SRWWriteGuard guard(m_lock);
            const UINT64 nowMs = ::GetTickCount64();   // 락 안에서 읽음 - 이 락이 RefreshOnline/PutOnline 을 직렬화하므로 nowMs >= 모든 기존 lastSyncMs (stale-window 제거)
            std::unordered_map<UINT32, OnlineEntry>::iterator it = m_online.begin();
            while (it != m_online.end())
            {
                // 언더플로 가드(belt-and-suspenders): lastSyncMs > nowMs(시계 역전 시) 면 막 갱신된 산 엔트리라 erase 안 함.
                if (nowMs > it->second.lastSyncMs && (nowMs - it->second.lastSyncMs) > graceMs)
                {
                    it = m_online.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // 현재 미소비 토큰 수 (모니터 - read 락 스냅샷).
        int TokenStore::GetTokenCount()
        {
            KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_lock);
            return static_cast<int>(m_tokens.size());
        }

        // 현재 접속 중 계정 수 (모니터, kick 진단 - read 락 스냅샷).
        int TokenStore::GetOnlineCount()
        {
            KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_lock);
            return static_cast<int>(m_online.size());
        }

        // 만료 토큰 일괄 제거. erase 가 다음 유효 반복자를 돌려주므로 그걸로 진행.
        void TokenStore::SweepExpired(UINT64 nowMs)
        {
            SRWWriteGuard guard(m_lock);

            std::unordered_map<UINT64, TokenEntry>::iterator it = m_tokens.begin();
            while (it != m_tokens.end())
            {
                if (nowMs >= it->second.expiryMs)
                {
                    it = m_tokens.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
}
