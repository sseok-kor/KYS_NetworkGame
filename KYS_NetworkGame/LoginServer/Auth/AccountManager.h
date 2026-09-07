#pragma once
#include "Types/Defines.h"                            // UINT32 ($(SolutionDir)GameServer 검색경로 - pch_loginserver.h 와 같은 방식)
#include "../../GameServer/Core/Config/DbConfig.h"    // DbConfig (LoadFromDb 인자)
#include "PasswordHasher.h"                           // STORED_MAX (저장 해시 문자열 길이)
#include "../GameServer/Core/Thread/SRWLockWrapper.h" // 런타임 write(계정 생성) 보호 - 부팅 후 더는 불변이 아니라 read/write 락 도입
#include <unordered_map>
#include <string>

namespace KYS
{
    namespace LOGINSERVER
    {
        // 계정 인증 전담(로그인 서버 분리 후 accounts MySQL 전용 소유). 부팅 때 accounts 를 메모리맵에 올리고, 로그인 시 id/비번을 대조한다.
        //   계정(인증)은 로그인 서버, 캐릭터(게임 상태)는 게임 서버가 따로 담당(characters 는 게임 서버 소유).
        //   인게임 계정 생성(CS_REGISTER)으로 런타임 INSERT 가 생기면서 부팅 후 불변이 아니게 됐다 -> SRWLock 으로 보호
        //   (조회=read 공유, 계정 추가=write 단독). 로그인/생성은 콜드 패스라 락 경합은 무시 가능.
        //   Verify 는 평문 비번을 받아 저장 해시(PBKDF2 self-describing 문자열)와 대조 - 성공 시 계정 번호(accountId)를 돌려준다.
        //   accountId 는 accounts 테이블의 AUTO_INCREMENT 영속 키(토큰 발급/온라인 표/축출 라우팅 + 캐릭터 소유 FK).
        class AccountManager
        {
        public:
            static AccountManager& GetInstance() { static AccountManager instance; return instance; }   // Meyers static

            AccountManager(const AccountManager&) = delete;            // 복사 2줄 (단일 소유, 이동은 자동 미선언)
            AccountManager& operator=(const AccountManager&) = delete;

            int    LoadFromFile(const wchar_t* path);                  // (레거시, MySQL 적재로 대체) accounts.csv 적재. 반환 = 계정 수.
            int    LoadFromDb(const DbConfig& config);                 // 부팅 1회: MySQL accounts 테이블 적재. 반환 = 계정 수(연결 실패 = -1, fail-fast).

            // 로그인 검증: 성공 = accountId(1 이상) / 실패(없는 id 또는 비번 불일치) = 0.
            //   평문을 PasswordHasher 가 저장 해시와 대조. 없는 id 도 더미 해싱 1회로 응답 시간을 맞춰 타이밍 공격을 막는다.
            UINT32 Verify(const wchar_t* id, const wchar_t* pw) const;

            // 런타임 계정 추가(CreateAccountCommand 가 INSERT 성공 후 호출). 메모리맵에 등록해 즉시 로그인 가능하게 한다.
            //   accountId = INSERT 가 받은 AUTO_INCREMENT 값, storedHash = PasswordHasher 가 만든 self-describing 문자열.
            void   AddAccount(const wchar_t* id, UINT32 accountId, const wchar_t* storedHash);

        private:
            AccountManager();    // 싱글턴 - GetInstance 로만 생성 (더미 해시 준비)
            ~AccountManager() = default;

            // 계정 한 건의 적재 내역.
            struct AccountRecord
            {
                UINT32  accountId;                                   // 계정 번호 (accounts.account_id AUTO_INCREMENT, 영속)
                wchar_t storedHash[PasswordHasher::STORED_MAX];      // 저장 해시 문자열 ("$pbkdf2$.." 실계정 / "$test$" 봇)
            };

            std::unordered_map<std::wstring, AccountRecord> m_accounts;   // login_name -> {accountId, storedHash}
            wchar_t                m_dummyHash[PasswordHasher::STORED_MAX]; // 없는 id 검증용 더미 해시(타이밍 공격 방어 - 항상 PBKDF2 1회)
            mutable KYS::GAMESERVER::THREAD::SRWLockWrapper m_lock;        // read=Verify 조회 / write=AddAccount 등록 (Verify const 라 mutable)
        };
    }
}
