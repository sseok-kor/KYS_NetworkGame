#pragma once

namespace KYS
{
    namespace LOGINSERVER
    {
        // 비밀번호 해시 seam. 평문 -> self-describing 해시 문자열(알고리즘 종류가 문자열 prefix 에 박힘) 생성/검증.
        //   현재 알고리즘 = PBKDF2(HMAC-SHA256, 600K 반복, salt 16B) via Windows CNG(BCryptDeriveKeyPBKDF2).
        //   봇 로드테스트용 "$test$" 스킴 공존(PBKDF2 우회 즉시 통과 - seed_bots.sql 수동 적재로만 생기고, 클라 입력으로는 생성 불가).
        //   self-describing 포맷 + prefix dispatch 라 미래 Argon2id 등을 같은 자리에 추가 가능(알고리즘 민첩성).
        //
        //   왜 느린 해시인가: DB 가 통째로 유출되면 공격자가 자기 GPU 에서 무제한 속도로 비번을 추측한다
        //   (서버 rate-limit/잠금/로그가 전부 우회됨). 유일한 방어가 해시를 느리게(반복) 만드는 것.
        //   그래서 의도적으로 느리다(건당 ~200-490ms) - 호출 스레드는 함수마다 다르다.
        //   Hash 는 계정 생성 경로라 LoginDbThread(단일 직렬)에서 돌려 워커를 막지 않는다(부팅 더미 해시 1회는 예외).
        //   Verify 는 로그인 경로라 큐를 거치지 않고 IOCP 워커에서 바로 돈다 - 연결당 시도 상한이 증폭을 막는다.
        namespace PasswordHasher
        {
            static const int STORED_MAX = 128;   // 저장 해시 문자열 최대 wchar (accounts.pw_hash VARCHAR(128) 정합)

            // 평문 -> "$pbkdf2$<iter>$<saltHex>$<hashHex>". out 은 STORED_MAX 이상. 성공=true / CNG 실패=false.
            //   매 호출 새 salt(BCryptGenRandom 16B) - 같은 비번도 매번 다른 해시(rainbow table 무력화).
            bool Hash(const wchar_t* plain, wchar_t* out, int outCap);

            // 평문 vs 저장 해시. prefix dispatch:
            //   "$pbkdf2$" = 저장된 salt/iter 로 재계산 후 상수시간 비교(early-return 금지 - 타이밍 누출 차단)
            //   "$test$"   = 즉시 통과(봇 로드테스트 전용)
            //   그 외 prefix = false(알 수 없는 형식)
            //   needsRehash: 약한/구식 해시면 true(로그인 시 재해시 업그레이드 hook). 현재 PBKDF2 1개라 항상 false.
            //   성공(일치)=true / 불일치=false.
            bool Verify(const wchar_t* plain, const wchar_t* stored, bool* needsRehash);
        }
    }
}
