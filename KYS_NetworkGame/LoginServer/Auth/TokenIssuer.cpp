#include "pch_loginserver.h"
#include "TokenIssuer.h"
#include <bcrypt.h>   // BCryptGenRandom / NTSTATUS / BCRYPT_SUCCESS

#pragma comment(lib, "bcrypt.lib")

namespace KYS
{
    namespace LOGINSERVER
    {
        // 8바이트를 시스템 CSPRNG 로 채워 UINT64 토큰을 만든다. 0 은 무효 표식이라 제외한다.
        UINT64 TokenIssuer::Make()
        {
            UINT64 token = 0;

            // BCRYPT_USE_SYSTEM_PREFERRED_RNG: 알고리즘 핸들 없이 시스템 기본 CSPRNG 사용.
            //   0 이 뽑히는 건 천문학적으로 드물지만, 무효 표식과 겹치므로 0 이면 다시 뽑는다.
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                const NTSTATUS status = ::BCryptGenRandom(
                    nullptr,
                    reinterpret_cast<PUCHAR>(&token),
                    static_cast<ULONG>(sizeof(token)),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG);

                if (!BCRYPT_SUCCESS(status))
                {
                    return 0;   // CSPRNG 실패 - 호출측이 발급 실패로 처리
                }
                if (token != 0)
                {
                    return token;
                }
            }

            return 0;   // 연속으로 0 (사실상 불가) - 실패로 처리
        }
    }
}
