#include "pch_gameserver.h"
#include "SerializationBuffer.h"

// capacity 바이트 버퍼를 잡고 쓰기/읽기 위치를 0으로 시작한다.
//   capacity : 확보할 버퍼 크기(바이트)
KYS::GAMESERVER::PROTOCOL::SerializationBuffer::SerializationBuffer(int capacity)
    : m_writePos(0)
    , m_readPos(0)
    , m_capacity(capacity)
    , m_failed(false)
    , m_readFailed(false)
{
    _ASSERTE(capacity > 0);
    m_buffer = new BYTE[capacity];
}

// 버퍼 메모리를 해제한다.
KYS::GAMESERVER::PROTOCOL::SerializationBuffer::~SerializationBuffer()
{
    delete[] m_buffer;
    m_buffer = nullptr;
}

// BYTE 1개를 그대로 쓰고 쓰기 위치를 1바이트 전진한다.
//   value : 쓸 바이트
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Write(BYTE value)
{
    // 첫 실패 뒤에는 아무것도 쓰지 않는다 - 버퍼를 첫 실패 직전 상태로 얼려 두어야 끝에서 한 번 보는 IsGood 이 뜻을 갖는다.
    if (m_failed) { return; }
    // 용량 초과는 런타임으로 막는다 (_ASSERTE 는 Release 에서 사라져 힙 침범이 되므로 읽기 경로와 같은 형태로 대체).
    if (m_writePos + static_cast<int>(sizeof(BYTE)) > m_capacity) { m_failed = true; return; }
    m_buffer[m_writePos] = value;
    m_writePos += sizeof(BYTE);
}

// USHORT 2바이트를 네트워크 바이트순서로 바꿔 쓴다.
//   value : 쓸 값(호스트 바이트순서)
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Write(USHORT value)
{
    // 첫 실패 뒤에는 아무것도 쓰지 않는다 - 버퍼를 첫 실패 직전 상태로 얼려 두어야 끝에서 한 번 보는 IsGood 이 뜻을 갖는다.
    if (m_failed) { return; }
    // 용량 초과는 런타임으로 막는다 (_ASSERTE 는 Release 에서 사라져 힙 침범이 되므로 읽기 경로와 같은 형태로 대체).
    if (m_writePos + static_cast<int>(sizeof(USHORT)) > m_capacity) { m_failed = true; return; }
    USHORT valueBE = htons(value);
    memcpy(m_buffer + m_writePos, &valueBE, sizeof(USHORT));
    m_writePos += sizeof(USHORT);
}

// UINT 4바이트를 네트워크 바이트순서로 바꿔 쓴다.
//   value : 쓸 값(호스트 바이트순서)
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Write(UINT value)
{
    // 첫 실패 뒤에는 아무것도 쓰지 않는다 - 버퍼를 첫 실패 직전 상태로 얼려 두어야 끝에서 한 번 보는 IsGood 이 뜻을 갖는다.
    if (m_failed) { return; }
    // 용량 초과는 런타임으로 막는다 (_ASSERTE 는 Release 에서 사라져 힙 침범이 되므로 읽기 경로와 같은 형태로 대체).
    if (m_writePos + static_cast<int>(sizeof(UINT)) > m_capacity) { m_failed = true; return; }
    UINT valueBE = htonl(value);
    memcpy(m_buffer + m_writePos, &valueBE, sizeof(UINT));
    m_writePos += sizeof(UINT);
}

// int 4바이트를 네트워크 바이트순서로 바꿔 쓴다 (UINT로 캐스트해 변환).
//   value : 쓸 값
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Write(int value)
{
    // 첫 실패 뒤에는 아무것도 쓰지 않는다 - 버퍼를 첫 실패 직전 상태로 얼려 두어야 끝에서 한 번 보는 IsGood 이 뜻을 갖는다.
    if (m_failed) { return; }
    // 용량 초과는 런타임으로 막는다 (_ASSERTE 는 Release 에서 사라져 힙 침범이 되므로 읽기 경로와 같은 형태로 대체).
    if (m_writePos + static_cast<int>(sizeof(int)) > m_capacity) { m_failed = true; return; }
    UINT valueBE = htonl((UINT)value);
    memcpy(m_buffer + m_writePos, &valueBE, sizeof(int));
    m_writePos += sizeof(int);
}

// 임의 크기 데이터를 변환 없이 그대로 복사해 쓴다.
//   data : 복사할 원본
//   size : 복사할 바이트 수
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Write(const void* data, int size)
{
    _ASSERTE(data != nullptr);   // 인자 sanity - 프로그래머 실수용(네트워크 데이터가 만들 수 없는 상태)
    _ASSERTE(size > 0);

    if (m_failed) { return; }
    if (m_writePos + size > m_capacity) { m_failed = true; return; }
    memcpy(m_buffer + m_writePos, data, size);
    m_writePos += size;
}

// BYTE 1개를 읽고 읽기 위치를 1바이트 전진한다.
//   out : 읽은 바이트를 받을 곳
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Read(BYTE& out)
{
    // 신뢰 경계 방어: 남은 바이트보다 더 읽으려 하면(절단/변조 패킷) 0을 주고 멈춘다.
    //   (_ASSERTE 는 Release 에서 사라져 OOB read/abort 가 되므로 런타임 가드로 대체.)
    if (m_readPos + static_cast<int>(sizeof(BYTE)) > m_writePos) { out = 0; m_readFailed = true; return; }
    out = m_buffer[m_readPos];
    m_readPos += sizeof(BYTE);
}

// 2바이트를 읽어 호스트 바이트순서로 되돌린다.
//   out : 읽은 값을 받을 곳
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Read(USHORT& out)
{
    if (m_readPos + static_cast<int>(sizeof(USHORT)) > m_writePos) { out = 0; m_readFailed = true; return; }   // 절단/변조 방어
    USHORT raw;
    memcpy(&raw, m_buffer + m_readPos, sizeof(USHORT));
    out = ntohs(raw);
    m_readPos += sizeof(USHORT);
}

// 4바이트를 읽어 호스트 바이트순서로 되돌린다.
//   out : 읽은 값을 받을 곳
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Read(UINT& out)
{
    if (m_readPos + static_cast<int>(sizeof(UINT)) > m_writePos) { out = 0; m_readFailed = true; return; }   // 절단/변조 방어
    UINT raw;
    memcpy(&raw, m_buffer + m_readPos, sizeof(UINT));
    out = ntohl(raw);
    m_readPos += sizeof(UINT);
}

// 4바이트를 읽어 호스트 바이트순서로 되돌린다.
//   out : 읽은 값을 받을 곳
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Read(INT& out)
{
    if (m_readPos + static_cast<int>(sizeof(INT)) > m_writePos) { out = 0; m_readFailed = true; return; }   // 절단/변조 방어
    UINT raw;
    memcpy(&raw, m_buffer + m_readPos, sizeof(INT));
    out = (INT)ntohl(raw);
    m_readPos += sizeof(INT);
}

// 임의 크기 데이터를 변환 없이 그대로 복사해 읽는다.
//   dest : 읽은 데이터를 받을 곳
//   size : 읽을 바이트 수
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Read(void* dest, int size)
{
    if (dest == nullptr || size <= 0) { return; }
    // 신뢰 경계 방어: 남은 바이트보다 크면 0으로 채우고 멈춘다 (절단/변조 패킷).
    if (m_readPos + size > m_writePos) { memset(dest, 0, static_cast<size_t>(size)); m_readFailed = true; return; }
    memcpy(dest, m_buffer + m_readPos, size);
    m_readPos += size;
}

// 문자열을 [길이 2B][문자 2B씩]로 쓴다 (버퍼 밖 읽기 방어 포함).
//   str    : 쓸 wchar_t 문자열
//   maxLen : str 배열 크기(널 종료 자리 포함) - ReadString 과 같은 뜻
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::WriteString(const wchar_t* str, int maxLen)
{
    _ASSERTE(str != nullptr);
    _ASSERTE(maxLen > 0);

    // 널 종료가 없어도 배열 밖으로 나가지 않게 maxLen-1 까지만 센다 (wcslen 은 배열 크기를 몰라 널을 찾을 때까지 읽는다).
    //   maxLen 의 뜻은 ReadString 과 같다 - 배열 크기(널 자리 포함). 그래서 최대 maxLen-1 글자를 쓴다.
    int charCount = 0;
    while (charCount < maxLen - 1 && str[charCount] != L'\0')
    {
        ++charCount;
    }

    const USHORT length = static_cast<USHORT>(charCount);
    Write(length);
    for (USHORT i = 0; i < length; ++i)
    {
        Write(static_cast<USHORT>(str[i]));
    }
}

// [길이][문자들] 형식 문자열을 dest로 읽는다 (버퍼 넘침/스트림 어긋남 방어 포함).
//   dest   : 읽은 문자열을 받을 배열
//   maxLen : dest 배열 크기(널 종료 자리 포함)
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::ReadString(wchar_t* dest, int maxLen)
{
    _ASSERTE(dest != nullptr);
    _ASSERTE(maxLen > 0);

    USHORT length;
    Read(length);   // 절단 패킷이면 Read 가드가 length=0 -> 빈 문자열 (abort 안 함)
    // 있지도 않은 데이터 읽기 막기: 선언된 length가 버퍼에 남은 글자 수보다 크면 남은 만큼으로 줄인다.
    //   (적대적 큰 length 도 아래 remainChars/copyLen clamp 가 흡수 - _ASSERTE 제거로 Debug abort 방지.)
    int remainChars = (m_writePos - m_readPos) / static_cast<int>(sizeof(USHORT));
    if (remainChars < 0) remainChars = 0;   // 심층 방어: Read 가드 덕에 지금은 음수가 안 나오지만, 그 가드가 무너지면 음수 -> USHORT 로 바꿀 때 큰 값으로 뒤집히므로 0으로 눌러 막음
    if (static_cast<int>(length) > remainChars)
    {
        // 정직한 피어는 이 값을 만들 수 없다. 보내는 쪽이 선언한 글자 수가 L 이면 그 뒤 필드 바이트 T 를 더해
        //   remainChars = floor((2L + T) / 2) = L + floor(T/2) >= L 이라 항상 통과한다. 넘었다면 선언이 거짓이다.
        length = static_cast<USHORT>(remainChars);
        m_readFailed = true;
    }

    // 받는 배열 넘침 막기: dest에는 maxLen-1 글자까지만 담는다 (+1은 널 자리). 스택 넘침의 진짜 방어.
    int copyLen = (static_cast<int>(length) <= maxLen - 1) ? static_cast<int>(length) : (maxLen - 1);
    for (int i = 0; i < copyLen; ++i)
    {
        USHORT ch;
        Read(ch);
        dest[i] = static_cast<wchar_t>(ch);
    }
    dest[copyLen] = L'\0';

    // 읽기 위치 맞추기: dest에 안 담은 나머지 글자도 읽기 위치만 넘겨 다음 필드 파싱이 어긋나지 않게 한다.
    for (int i = copyLen; i < static_cast<int>(length); ++i)
    {
        USHORT skip;
        Read(skip);
    }
}

// 이미 쓴 영역의 pos 위치에 USHORT 2바이트를 덮어쓴다 (쓰기 위치는 그대로). 패킷 맨 앞 길이 자리를 나중에 채우는 용도.
//   pos : 덮어쓸 위치(바이트, writePos 이내)
//   value : 덮어쓸 값(호스트 바이트순서)
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Put(int pos, USHORT value)
{
    // 이미 쓴 영역(writePos 이내)만 덮어쓸 수 있다. 아직 안 쓴 영역 금지.
    if (m_failed) { return; }
    if (pos < 0 || pos + static_cast<int>(sizeof(USHORT)) > m_writePos) { m_failed = true; return; }

    // 보내는 데이터는 네트워크 바이트순서라 여기서 변환한다.
    USHORT be = htons(value);

    // pos 위치에 2바이트만 덮어쓴다 (쓰기 위치는 안 변함)
    memcpy(m_buffer + pos, &be, sizeof(USHORT));
}

// operator<< - Write 호출 후 자기 자신 반환 (a << b << c 처럼 이어 쓰기)
KYS::GAMESERVER::PROTOCOL::SerializationBuffer&
KYS::GAMESERVER::PROTOCOL::SerializationBuffer::operator<<(BYTE value)
{
    Write(value);
    return *this;
}

KYS::GAMESERVER::PROTOCOL::SerializationBuffer&
KYS::GAMESERVER::PROTOCOL::SerializationBuffer::operator<<(USHORT value)
{
    Write(value);
    return *this;
}

KYS::GAMESERVER::PROTOCOL::SerializationBuffer&
KYS::GAMESERVER::PROTOCOL::SerializationBuffer::operator<<(UINT value)
{
    Write(value);
    return *this;
}

KYS::GAMESERVER::PROTOCOL::SerializationBuffer&
KYS::GAMESERVER::PROTOCOL::SerializationBuffer::operator<<(int value)
{
    Write(value);
    return *this;
}

// operator>> - Read 호출 후 자기 자신 반환 (이어 읽기)
KYS::GAMESERVER::PROTOCOL::SerializationBuffer&
KYS::GAMESERVER::PROTOCOL::SerializationBuffer::operator>>(BYTE& out)
{
    Read(out);
    return *this;
}

KYS::GAMESERVER::PROTOCOL::SerializationBuffer&
KYS::GAMESERVER::PROTOCOL::SerializationBuffer::operator>>(USHORT& out)
{
    Read(out);
    return *this;
}

KYS::GAMESERVER::PROTOCOL::SerializationBuffer&
KYS::GAMESERVER::PROTOCOL::SerializationBuffer::operator>>(UINT& out)
{
    Read(out);
    return *this;
}

KYS::GAMESERVER::PROTOCOL::SerializationBuffer&
KYS::GAMESERVER::PROTOCOL::SerializationBuffer::operator>>(int& out)
{
    Read(out);
    return *this;
}

// 쓰기/읽기 위치를 0으로 되돌려 버퍼를 재사용한다 (할당된 메모리는 유지).
void KYS::GAMESERVER::PROTOCOL::SerializationBuffer::Reset()
{
    m_writePos = 0;
    m_readPos = 0;
    m_failed = false;   // 재사용 시 이전 실패가 남으면 그 버퍼는 영원히 못 쓰게 된다
    m_readFailed = false;   // 풀 재사용 버퍼가 첫 절단 패킷 하나로 영구 불능이 되지 않도록 함께 내린다
    OnReset();  // 자식 클래스가 추가 초기화할 hook
    // m_buffer, m_capacity는 그대로 - ObjectPool 재사용
}

// 지금까지 읽은 위치(바이트)를 반환한다.
int KYS::GAMESERVER::PROTOCOL::SerializationBuffer::GetReadOffset() const
{
    return m_readPos;
}

// 내부 버퍼 시작 주소를 반환한다 (WSASend로 그대로 보낼 때 사용).
BYTE* KYS::GAMESERVER::PROTOCOL::SerializationBuffer::GetBuffer()
{
    return m_buffer;  // WSASend 직접 송신용 (송신 외 수정 금지)
}

// 지금까지 쓴 바이트 수를 반환한다.
int KYS::GAMESERVER::PROTOCOL::SerializationBuffer::GetSize() const
{
    return m_writePos;
}

// 지금까지의 쓰기가 모두 성공했는지 반환한다.
//   한 번이라도 버퍼를 넘겼으면 false 이고, 그 뒤로는 계속 false 다(Reset 전까지).
//   호출자는 매 쓰기를 검사할 필요 없이 다 쓴 뒤 한 번만 보면 된다 - 첫 실패 이후 버퍼가 얼어 있기 때문.
bool KYS::GAMESERVER::PROTOCOL::SerializationBuffer::IsGood() const
{
    return !m_failed;
}

// 읽기가 한 번이라도 버퍼 끝을 넘었으면 false (첫 실패 이후 계속 false).
//   쓰기 실패(IsGood)와 갈라 둔 이유: 쓰기 실패는 우리 버퍼가 작았다는 뜻이라 로그를 남기고 코드를 고칠 일이고,
//   읽기 실패는 상대가 잘린/위조된 패킷을 보냈다는 뜻이라 그 패킷을 버릴 일이다. 원인도 처분도 다르다.
bool KYS::GAMESERVER::PROTOCOL::SerializationBuffer::IsReadGood() const
{
    return !m_readFailed;
}
