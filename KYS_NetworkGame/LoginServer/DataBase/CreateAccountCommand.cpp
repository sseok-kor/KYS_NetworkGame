#include "pch_loginserver.h"
#include "CreateAccountCommand.h"
#include "Auth/PasswordHasher.h"                     // Hash (PBKDF2)
#include "Auth/AccountManager.h"                     // AddAccount (성공 시 메모리맵 등록)
#include "../GameServer/Network/IOCP/IOCPServer.h"   // SendTo
#include "../GameCommon/Protocol/CPacket.h"
#include "../GameCommon/Protocol/PacketType.h"
#include "../GameCommon/Protocol/GamePackets.h"
#include "../GameServer/Core/Log/Logger.h"   // 계정 생성 이력 파일 로그 (user 채널)
#include <cstring>   // memset / strlen
#include <cwchar>    // wcscpy_s

namespace
{
    // MySQL 중복 키 에러 코드(ER_DUP_ENTRY). 헤더 의존(mysqld_error.h)을 줄이려 로컬 상수로 둠.
    const unsigned MYSQL_ER_DUP_ENTRY = 1062;

    // wchar_t(UTF-16) -> UTF-8 char. 반환 = 바이트 수(널 제외). 바인딩 length 로 그대로 쓴다.
    int ToUtf8(const wchar_t* w, char* out, int outCap)
    {
        if (w == nullptr) { out[0] = '\0'; return 0; }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outCap, nullptr, nullptr);
        return (n > 0) ? (n - 1) : 0;   // n 은 널 포함 -> 바이트 길이는 n-1
    }
}

namespace KYS
{
    namespace LOGINSERVER
    {
        CreateAccountCommand::CreateAccountCommand(UINT64 sid, const wchar_t* id, const wchar_t* pw,
                                                   KYS::GAMESERVER::NETWORK::IOCPServer* server)
            : m_sid(sid)
            , m_server(server)
        {
            // 고정 배열 복사 - 큐에서 대기하는 동안 호출측 버퍼 수명과 무관하게 안전.
            m_id[0] = 0;
            m_pw[0] = 0;
            if (id != nullptr) { wcscpy_s(m_id, LoginReq::ID_MAX, id); }
            if (pw != nullptr) { wcscpy_s(m_pw, PASSWORD_MAX, pw); }
        }

        void CreateAccountCommand::Execute()
        {
            ECreateAccountResult result = ECreateAccountResult::DB_ERROR;
            wchar_t storedHash[PasswordHasher::STORED_MAX] = { 0 };
            UINT32  newAccountId = 0;

            // (1) PBKDF2 해시 (DB 스레드에서 - 워커 블로킹 0). 실패면 DB_ERROR 로 떨어짐.
            if (m_conn != nullptr && PasswordHasher::Hash(m_pw, storedHash, PasswordHasher::STORED_MAX))
            {
                // (2) prepared INSERT (account_id = AUTO_INCREMENT). 인자 바인딩이라 인젝션 안전.
                char loginUtf8[64];
                char hashUtf8[256];   // storedHash(<=112 ASCII) UTF-8 - 여유
                unsigned long loginLen = static_cast<unsigned long>(ToUtf8(m_id, loginUtf8, sizeof(loginUtf8)));
                unsigned long hashLen  = static_cast<unsigned long>(ToUtf8(storedHash, hashUtf8, sizeof(hashUtf8)));

                MYSQL_STMT* stmt = mysql_stmt_init(m_conn);
                if (stmt != nullptr)
                {
                    const char* sql = "INSERT INTO accounts(login_name, pw_hash) VALUES(?,?)";
                    if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(strlen(sql))) == 0)
                    {
                        MYSQL_BIND b[2];
                        memset(b, 0, sizeof(b));
                        b[0].buffer_type   = MYSQL_TYPE_STRING;   // login_name
                        b[0].buffer        = loginUtf8;
                        b[0].buffer_length = sizeof(loginUtf8);
                        b[0].length        = &loginLen;
                        b[1].buffer_type   = MYSQL_TYPE_STRING;   // pw_hash
                        b[1].buffer        = hashUtf8;
                        b[1].buffer_length = sizeof(hashUtf8);
                        b[1].length        = &hashLen;

                        if (mysql_stmt_bind_param(stmt, b) == 0)
                        {
                            if (mysql_stmt_execute(stmt) == 0)
                            {
                                newAccountId = static_cast<UINT32>(mysql_stmt_insert_id(stmt));   // AUTO_INCREMENT 값
                                result = ECreateAccountResult::OK;
                            }
                            else
                            {
                                // 중복 login_name(UNIQUE 충돌) = DUP_ID, 그 외 = DB_ERROR.
                                const unsigned e = mysql_stmt_errno(stmt);
                                result = (e == MYSQL_ER_DUP_ENTRY)
                                             ? ECreateAccountResult::DUP_ID
                                             : ECreateAccountResult::DB_ERROR;
                            }
                        }
                    }
                    mysql_stmt_close(stmt);
                }
            }

            // (3) 성공 시 메모리맵 등록 - 즉시 로그인 가능.
            if (result == ECreateAccountResult::OK)
            {
                AccountManager::GetInstance().AddAccount(m_id, newAccountId, storedHash);
            }

            // 계정 생성 이력 - 결과가 판명되는 여기(DB 스레드)서 기록 (캐릭터 생성/삭제 로그와 대칭 - 계정만 침묵하는 비대칭 해소).
            if (result == ECreateAccountResult::OK)
            {
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::USER, KYS::GAMESERVER::LOG::LogLevel::LL_INFO, L"account",
                    L"계정 생성 id=%s accountId=%u", m_id, newAccountId);
            }
            else
            {
                KYS::GAMESERVER::LOG::Logger::GetInstance().Log(
                    KYS::GAMESERVER::LOG::LogChannel::USER, KYS::GAMESERVER::LOG::LogLevel::LL_WARN, L"account",
                    L"계정 생성 실패 id=%s result=%d", m_id, static_cast<int>(result));
            }

            // (4) 결과 회신 (DB 스레드에서 SendTo - 세대 검증 + lease 가 풀 안에서 send-teardown race 를 봉인).
            SC_REGISTER_RESULT res;
            res.result = static_cast<BYTE>(result);

            KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
            out.Begin(static_cast<USHORT>(PacketType::SC_REGISTER_RESULT));
            res.Serialize(out);
            if (!out.End()) { KYS::GAMESERVER::LOG::Logger::GetInstance().Log(KYS::GAMESERVER::LOG::LogChannel::SERVER, KYS::GAMESERVER::LOG::LogLevel::LL_ERROR, L"packet", L"직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)", out.GetType(), out.GetSize()); return; }

            if (m_server != nullptr)
            {
                m_server->SendTo(m_sid, out.GetBuffer(), out.GetSize(), ESendDropPolicy::KEEP_CONNECTION);   // 없는/옛 핸들이면 no-op
            }
        }
    }
}
