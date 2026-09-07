#pragma once
#include "Types/Defines.h"

namespace KYS
{
    namespace GAMESERVER
    {
        namespace PROTOCOL
        {
            // 값들을 바이트열로 직렬화/역직렬화하는 버퍼 (정수는 네트워크 바이트순서로 변환). CPacket의 부모.
            class SerializationBuffer
            {
            public:
                // 생성 / 소멸
                SerializationBuffer(int capacity);                          // capacity 바이트 버퍼 확보
                virtual ~SerializationBuffer();

                SerializationBuffer(const SerializationBuffer&) = delete;
                SerializationBuffer& operator=(const SerializationBuffer&) = delete;

                // 쓰기 (정수는 네트워크 바이트순서로 변환해 씀)
                void Write(BYTE value);
                void Write(USHORT value);
                void Write(UINT value);
                void Write(int value);
                void Write(const void* data, int size);                     // 변환 없이 그대로 복사

                // 읽기 (정수는 호스트 바이트순서로 복원)
                void Read(BYTE& out);
                void Read(USHORT& out);
                void Read(UINT& out);
                void Read(int& out);
                void Read(void* dest, int size);                            // 변환 없이 그대로 복사

                // 문자열 ([길이][문자들] 형식)
                void WriteString(const wchar_t* str, int maxLen);
                void ReadString(wchar_t* dest, int maxLen);

                // 이미 쓴 자리 덮어쓰기 (패킷 맨 앞 길이 자리 채우는 용도)
                void Put(int pos, USHORT value);

                // 체이닝 연산자 (Write/Read 래퍼, 이어 쓰기/읽기)
                SerializationBuffer& operator<<(BYTE value);
                SerializationBuffer& operator<<(USHORT value);
                SerializationBuffer& operator<<(UINT value);
                SerializationBuffer& operator<<(int value);

                SerializationBuffer& operator>>(BYTE& out);
                SerializationBuffer& operator>>(USHORT& out);
                SerializationBuffer& operator>>(UINT& out);
                SerializationBuffer& operator>>(int& out);

                // 버퍼 재사용 (위치만 0으로, 메모리 유지)
                void Reset();

                // 조회
                int   GetReadOffset() const;                                // 지금까지 읽은 위치(바이트)
                BYTE* GetBuffer();                                          // 내부 버퍼 시작 주소 (WSASend 송신용)
                int   GetSize() const;                                      // 지금까지 쓴 바이트 수
                bool  IsGood() const;                                       // 쓰기가 한 번이라도 버퍼를 넘겼으면 false (첫 실패 이후 계속 false)
                bool  IsReadGood() const;                                   // 읽기가 한 번이라도 버퍼 끝을 넘겼으면 false (절단/변조 패킷 판정용)

            private:
                BYTE* m_buffer;       // 직렬화 데이터가 쌓이는 버퍼 (생성자 new, 소멸자 delete)
                int   m_writePos;     // 다음에 쓸 위치(바이트), GetSize가 이 값을 반환
                int   m_readPos;      // 다음에 읽을 위치(바이트), GetReadOffset이 이 값을 반환
                int   m_capacity;     // 버퍼 전체 크기(바이트), 오버플로 검사 기준
                bool  m_failed;       // 쓰기가 버퍼를 넘친 적이 있는가. 한 번 서면 Reset 전까지 안 내려가고, 그 뒤 쓰기는 전부 무시된다
                bool  m_readFailed;   // 읽기가 버퍼 끝을 넘은 적이 있는가. 쓰기 실패와 뜻이 다르다 - 쓰기 실패는 우리 버퍼가 작았다는
                                      //   뜻(우리 버그)이고, 읽기 실패는 상대가 잘린/위조된 패킷을 보냈다는 뜻(상대 입력)이다.
                                      //   그래서 창구도 IsGood 과 따로 둔다. 이 플래그는 쓰기를 얼리지 않는다(End 의 길이 backfill 이
                                      //   막히면 길이 0 프레임이 나가 상대가 연결을 끊는다).

                virtual void OnReset() {}   // Reset 때 자식이 추가 초기화할 hook (기본 빈 몸체)
            };
        }
    }
}
