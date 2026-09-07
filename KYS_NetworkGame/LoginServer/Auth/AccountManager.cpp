#include "pch_loginserver.h"
#include "AccountManager.h"
#include "../GameCommon/Protocol/GamePackets.h"   // LoginReq::ID_MAX (전역 struct)
#include <cstdio>                         // _wfopen_s / fclose
#include <cwchar>                         // fgetws / swscanf_s / wcscpy_s
#include <cstdlib>                         // strtoul (LoadFromDb account_id 파싱)

namespace
{
    // 계정 한 줄 최대 wchar: id 16 + 콤마 + 해시 문자열(<=128) + 여유.
    const int ACCOUNT_LINE_MAX = 160;
}

namespace KYS
{
    namespace LOGINSERVER
    {
        AccountManager::AccountManager()
        {
            // 없는 id 검증용 더미 해시 준비(부팅 1회, 단일 스레드). 임의 비번을 해시한 self-describing 문자열.
            //   Verify 가 없는 id 에도 이걸로 PBKDF2 1회를 돌려 응답 시간을 맞춘다(타이밍 공격 방어).
            if (!PasswordHasher::Hash(L"timing-equalizer-dummy", m_dummyHash, PasswordHasher::STORED_MAX))
            {
                // CNG 실패 폴백 - 형식만 맞는 더미(Verify 가 여전히 PBKDF2 재계산을 타도록 $pbkdf2$ prefix 유지).
                wcscpy_s(m_dummyHash, PasswordHasher::STORED_MAX,
                         L"$pbkdf2$600000$00000000000000000000000000000000$"
                         L"0000000000000000000000000000000000000000000000000000000000000000");
            }
        }

        // accounts.csv 를 통째로 읽어 메모리맵을 채운다 (부팅 1회, 레거시 경로 - 현재 호출자 없음, MySQL 적재로 대체).
        //   path : 계정 파일 경로. 형식 = "login_name,storedHash" (storedHash = "$pbkdf2$.." 문자열). 반환 = 적재 계정 수.
        int AccountManager::LoadFromFile(const wchar_t* path)
        {
            KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_lock);   // 적재 = write (부팅이라 경합 없으나 일관)
            m_accounts.clear();
            if (path == nullptr)
            {
                return 0;
            }

            FILE* fp = nullptr;
            if (_wfopen_s(&fp, path, L"rt, ccs=UTF-8") != 0 || fp == nullptr)
            {
                return 0;   // 파일 없음 = 계정 0
            }

            wchar_t line[ACCOUNT_LINE_MAX];
            int loaded = 0;
            while (fgetws(line, ACCOUNT_LINE_MAX, fp) != nullptr)
            {
                wchar_t id[LoginReq::ID_MAX] = { 0 };
                wchar_t storedHash[PasswordHasher::STORED_MAX] = { 0 };

                // id(콤마 전) + 해시(콤마 후, 줄 끝 개행 제외). width 로 오버런 차단.
                const int matched = swscanf_s(line, L"%15[^,],%127[^\r\n]",
                    id, static_cast<unsigned>(LoginReq::ID_MAX),
                    storedHash, static_cast<unsigned>(PasswordHasher::STORED_MAX));
                if (matched != 2)
                {
                    continue;   // 깨진/빈 줄 건너뜀
                }

                AccountRecord rec;
                rec.accountId = static_cast<UINT32>(loaded + 1);   // 레거시 경로 - 적재 순번(DB 가 아니라 영속성 없음)
                wcscpy_s(rec.storedHash, PasswordHasher::STORED_MAX, storedHash);
                m_accounts[id] = rec;
                ++loaded;
            }

            fclose(fp);
            return loaded;
        }

        // MySQL accounts 테이블에서 계정을 메모리맵으로 적재 (부팅 1회). accounts.csv 적재를 대체.
        //   반환 = 적재 계정 수. 연결 실패 = -1 (호출측 fail-fast). 0 = 계정 없음(시드 확인).
        int AccountManager::LoadFromDb(const DbConfig& config)
        {
            KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_lock);   // 적재 = write (부팅이라 경합 없으나 일관)
            m_accounts.clear();

            MYSQL* conn = mysql_init(nullptr);
            if (conn == nullptr) { return -1; }

            mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
            if (mysql_real_connect(conn, config.host, config.user, config.password,
                                   config.dbname, static_cast<unsigned int>(config.port),
                                   nullptr, 0) == nullptr)
            {
                mysql_close(conn);
                return -1;
            }

            // account_id(AUTO_INCREMENT 영속) + login_name + pw_hash(self-describing 해시 문자열).
            if (mysql_query(conn, "SELECT account_id, login_name, pw_hash FROM accounts") != 0)
            {
                mysql_close(conn);
                return -1;
            }

            MYSQL_RES* res = mysql_store_result(conn);
            if (res == nullptr)
            {
                if (mysql_errno(conn) != 0)
                {
                    mysql_close(conn);
                    return -1;
                }
                mysql_close(conn);
                return 0;
            }

            int loaded = 0;
            MYSQL_ROW row;   // = char**
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                if (row[0] == nullptr || row[1] == nullptr || row[2] == nullptr)
                {
                    continue;   // 방어 - NULL 컬럼 건너뜀
                }

                // login_name(UTF-8) -> wchar_t. 16자(VARCHAR(16))는 버퍼(15+널) 부족으로 n<=0 -> 그 줄 건너뜀(over-read 방지).
                wchar_t id[LoginReq::ID_MAX] = { 0 };
                const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                    row[1], -1, id, LoginReq::ID_MAX);
                if (n <= 0)
                {
                    continue;
                }

                // pw_hash(UTF-8 ASCII 해시 문자열) -> wchar_t. 128 초과면 변환 실패 -> 건너뜀.
                wchar_t storedHash[PasswordHasher::STORED_MAX] = { 0 };
                const int h = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                    row[2], -1, storedHash, PasswordHasher::STORED_MAX);
                if (h <= 0)
                {
                    continue;
                }

                AccountRecord rec;
                rec.accountId = static_cast<UINT32>(strtoul(row[0], nullptr, 10));   // account_id (AUTO_INCREMENT)
                wcscpy_s(rec.storedHash, PasswordHasher::STORED_MAX, storedHash);
                m_accounts[id] = rec;
                ++loaded;
            }

            mysql_free_result(res);
            mysql_close(conn);
            return loaded;
        }

        // 로그인 검증: 평문 비번을 저장 해시와 대조. 성공 = accountId(1+) / 실패 = 0.
        //   PBKDF2 는 느리므로 락은 맵 조회 + 해시 복사까지만 잡고(마이크로초), 해시 재계산은 락 밖에서(워커 블로킹 최소, writer 기아 방지).
        UINT32 AccountManager::Verify(const wchar_t* id, const wchar_t* pw) const
        {
            if (id == nullptr || pw == nullptr)
            {
                return 0;
            }

            UINT32  accountId = 0;
            wchar_t stored[PasswordHasher::STORED_MAX] = { 0 };
            bool    found = false;
            {
                KYS::GAMESERVER::THREAD::SRWReadGuard guard(m_lock);   // 조회 = read 공유
                const std::unordered_map<std::wstring, AccountRecord>::const_iterator it = m_accounts.find(id);
                if (it != m_accounts.end())
                {
                    accountId = it->second.accountId;
                    wcscpy_s(stored, PasswordHasher::STORED_MAX, it->second.storedHash);
                    found = true;
                }
            }

            bool needsRehash = false;
            if (!found)
            {
                // 없는 id 도 더미 해시로 PBKDF2 1회 - 응답 시간을 맞춰 "id 존재 여부"를 타이밍으로 못 읽게 함.
                //   m_dummyHash 는 부팅 때 1회 세팅 후 불변이라 락 없이 읽어도 안전.
                (void)PasswordHasher::Verify(pw, m_dummyHash, &needsRehash);
                return 0;
            }

            if (PasswordHasher::Verify(pw, stored, &needsRehash))
            {
                return accountId;
            }
            return 0;
        }

        // 런타임 계정 추가(INSERT 성공 후). 메모리맵에 등록 = write 단독. 같은 id 면 마지막 값으로 덮음(중복은 DB UNIQUE 가 먼저 거름).
        void AccountManager::AddAccount(const wchar_t* id, UINT32 accountId, const wchar_t* storedHash)
        {
            if (id == nullptr || storedHash == nullptr)
            {
                return;
            }
            KYS::GAMESERVER::THREAD::SRWWriteGuard guard(m_lock);
            AccountRecord rec;
            rec.accountId = accountId;
            wcscpy_s(rec.storedHash, PasswordHasher::STORED_MAX, storedHash);
            m_accounts[id] = rec;
        }
    }
}
