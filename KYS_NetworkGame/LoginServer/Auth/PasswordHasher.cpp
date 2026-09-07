#include "pch_loginserver.h"
#include "PasswordHasher.h"
#include <bcrypt.h>   // BCryptOpenAlgorithmProvider / BCryptDeriveKeyPBKDF2 / BCryptGenRandom / NTSTATUS
#include <cwchar>     // swprintf_s / wcsncmp / wcschr / wcslen
#include <cstdlib>    // wcstoull

#pragma comment(lib, "bcrypt.lib")

namespace
{
    using KYS::LOGINSERVER::PasswordHasher::STORED_MAX;

    const unsigned ITERATIONS = 600000;   // OWASP 2024 PBKDF2-HMAC-SHA256 권고 반복수 (느림 = 오프라인 크래킹 저항)
    const int      SALT_LEN   = 16;       // salt 바이트 (CSPRNG)
    const int      HASH_LEN   = 32;       // 파생 키 바이트 (SHA-256 출력 길이)

    // wchar_t(UTF-16) -> UTF-8 바이트. 반환 = 바이트 수(널 제외). 비번은 불투명 바이트로 해시 입력.
    int ToUtf8(const wchar_t* w, BYTE* out, int outCap)
    {
        if (w == nullptr) { if (outCap > 0) { out[0] = 0; } return 0; }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, reinterpret_cast<char*>(out), outCap, nullptr, nullptr);
        return (n > 0) ? (n - 1) : 0;   // n 은 널 포함 길이 -> 바이트 길이는 n-1
    }

    // 바이트 배열 -> 소문자 hex 문자열. out 은 len*2+1 wchar 이상.
    void BytesToHex(const BYTE* in, int len, wchar_t* out)
    {
        static const wchar_t* const kDigits = L"0123456789abcdef";
        for (int i = 0; i < len; ++i)
        {
            out[i * 2]     = kDigits[(in[i] >> 4) & 0xF];
            out[i * 2 + 1] = kDigits[in[i] & 0xF];
        }
        out[len * 2] = 0;
    }

    int HexVal(wchar_t c)
    {
        if (c >= L'0' && c <= L'9') { return c - L'0'; }
        if (c >= L'a' && c <= L'f') { return c - L'a' + 10; }
        if (c >= L'A' && c <= L'F') { return c - L'A' + 10; }
        return -1;
    }

    // hex 문자열(hexLen 글자) -> outLen 바이트. 길이/문자 불일치면 false.
    bool HexToBytes(const wchar_t* hex, size_t hexLen, BYTE* out, int outLen)
    {
        if (hexLen != static_cast<size_t>(outLen) * 2) { return false; }
        for (int i = 0; i < outLen; ++i)
        {
            const int hi = HexVal(hex[i * 2]);
            const int lo = HexVal(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) { return false; }
            out[i] = static_cast<BYTE>((hi << 4) | lo);
        }
        return true;
    }

    // PBKDF2(HMAC-SHA256) 핵심. Hash/Verify 가 공유. 성공=true.
    bool Pbkdf2(const BYTE* pw, ULONG pwLen, const BYTE* salt, ULONG saltLen,
                unsigned long long iter, BYTE* out, ULONG outLen)
    {
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        // HMAC 플래그로 SHA-256 을 PRF 로 여는 알고리즘 핸들.
        NTSTATUS s = ::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                   BCRYPT_ALG_HANDLE_HMAC_FLAG);
        if (!BCRYPT_SUCCESS(s)) { return false; }

        // pbPassword/pbSalt 는 PUCHAR(비-const) 시그니처라 const_cast (CNG 가 읽기만 함).
        s = ::BCryptDeriveKeyPBKDF2(hAlg,
                                    const_cast<PUCHAR>(pw), pwLen,
                                    const_cast<PUCHAR>(salt), saltLen,
                                    static_cast<ULONGLONG>(iter),
                                    out, outLen, 0);
        ::BCryptCloseAlgorithmProvider(hAlg, 0);
        return BCRYPT_SUCCESS(s);
    }

    // 상수시간 바이트 비교 - 일치 여부와 무관하게 항상 len 바이트를 본다(early-return 금지=타이밍 누출 차단).
    bool ConstTimeEqual(const BYTE* a, const BYTE* b, int len)
    {
        BYTE diff = 0;
        for (int i = 0; i < len; ++i)
        {
            diff |= static_cast<BYTE>(a[i] ^ b[i]);
        }
        return diff == 0;
    }
}

namespace KYS
{
    namespace LOGINSERVER
    {
        namespace PasswordHasher
        {
            bool Hash(const wchar_t* plain, wchar_t* out, int outCap)
            {
                if (plain == nullptr || out == nullptr || outCap < STORED_MAX) { return false; }

                // (1) 평문 -> UTF-8 바이트 (PASSWORD_MAX 31자 -> 최대 ~93 바이트, 여유 128).
                BYTE pwBytes[128];
                const int pwLen = ToUtf8(plain, pwBytes, sizeof(pwBytes));

                // (2) salt 16B = 시스템 CSPRNG (TokenIssuer 와 같은 CNG 경로).
                BYTE salt[SALT_LEN];
                if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, salt, SALT_LEN, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
                {
                    return false;
                }

                // (3) PBKDF2 파생.
                BYTE hash[HASH_LEN];
                if (!Pbkdf2(pwBytes, static_cast<ULONG>(pwLen), salt, SALT_LEN, ITERATIONS, hash, HASH_LEN))
                {
                    return false;
                }

                // (4) self-describing 문자열 조립: "$pbkdf2$<iter>$<saltHex>$<hashHex>".
                wchar_t saltHex[SALT_LEN * 2 + 1];
                wchar_t hashHex[HASH_LEN * 2 + 1];
                BytesToHex(salt, SALT_LEN, saltHex);
                BytesToHex(hash, HASH_LEN, hashHex);

                const int written = ::swprintf_s(out, static_cast<size_t>(outCap),
                                                 L"$pbkdf2$%u$%s$%s", ITERATIONS, saltHex, hashHex);
                return written > 0;
            }

            bool Verify(const wchar_t* plain, const wchar_t* stored, bool* needsRehash)
            {
                if (needsRehash != nullptr) { *needsRehash = false; }
                if (plain == nullptr || stored == nullptr) { return false; }

                // 봇 로드테스트 스킴 - PBKDF2 우회 즉시 통과(seed_bots.sql 수동 적재로만 생기고, 클라 입력으로는 절대 생성 안 됨).
                if (wcscmp(stored, L"$test$") == 0)
                {
                    return true;
                }

                // PBKDF2 스킴 - 저장된 iter/salt 로 재계산 후 상수시간 비교.
                if (wcsncmp(stored, L"$pbkdf2$", 8) != 0)
                {
                    return false;   // 알 수 없는 형식
                }

                const wchar_t* p = stored + 8;   // "$pbkdf2$" 다음

                // iter (10진, 다음 '$' 까지).
                wchar_t* endp = nullptr;
                const unsigned long long iter = ::wcstoull(p, &endp, 10);
                if (endp == p || *endp != L'$' || iter == 0) { return false; }

                // saltHex ('$' 까지) / hashHex (끝까지).
                const wchar_t* saltStart = endp + 1;
                const wchar_t* dollar = wcschr(saltStart, L'$');
                if (dollar == nullptr) { return false; }
                const size_t saltHexLen = static_cast<size_t>(dollar - saltStart);
                const wchar_t* hashStart = dollar + 1;
                const size_t hashHexLen = wcslen(hashStart);

                BYTE salt[SALT_LEN];
                BYTE storedHash[HASH_LEN];
                if (!HexToBytes(saltStart, saltHexLen, salt, SALT_LEN)) { return false; }
                if (!HexToBytes(hashStart, hashHexLen, storedHash, HASH_LEN)) { return false; }

                // 평문을 같은 salt/iter 로 재계산.
                BYTE pwBytes[128];
                const int pwLen = ToUtf8(plain, pwBytes, sizeof(pwBytes));
                BYTE computed[HASH_LEN];
                if (!Pbkdf2(pwBytes, static_cast<ULONG>(pwLen), salt, SALT_LEN, iter, computed, HASH_LEN))
                {
                    return false;
                }

                return ConstTimeEqual(computed, storedHash, HASH_LEN);
            }
        }
    }
}
