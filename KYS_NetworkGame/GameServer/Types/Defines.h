#pragma once

// ===== 기본 타입 (부호없는 정수 별칭) =====

using BYTE = unsigned char;          // 1바이트 (바이트 스트림 단위)
using UINT16 = unsigned short;       // 2바이트
using UINT32 = unsigned int;         // 4바이트
using UINT64 = unsigned long long;   // 8바이트 (sid 등 큰 값)

using USHORT = unsigned short;       // 2바이트 (패킷 종류/길이용)
using UINT = unsigned int;           // 4바이트

// ===== 메모리 풀 =====

static const size_t DEFAULT_SESSION_POOL_CAPACITY = 7000;   // Session 풀이 미리 잡아두는 슬롯 수 (MAX_CCU 6000보다 크게, 소켓+게임세션 풀 공용)


// ===== 네트워크 =====

static const int MAX_ACCEPT_CONTEXT = 64;       // 미리 걸어두는 accept 대기 개수 (몰려드는 접속을 받아둠)

static const int LENGTH_PREFIX_SIZE = 2;        // 패킷 맨 앞 길이 자리 바이트 수 (전체 길이를 2바이트로 적음)

static const int RECV_BUFFER_SIZE = 8192;       // 세션 한 개 수신 버퍼 바이트 = 수신측이 받아 주는 최대 프레임 (이보다 큰 길이는 Session 프레이밍이 끊는다)

static const int SEND_BUFFER_SIZE = 16384;      // 세션 한 개 송신 버퍼 바이트 (배치 8192의 2배 - 완료 통지가 늦어도 다음 배치 받을 헤드룸 1배치분)
static const int MAX_PACKET_SIZE = RECV_BUFFER_SIZE;   // 패킷 한 개 최대 바이트 = 수신 버퍼에 통째로 담겨야 하는 크기. 송신 CPacket 용량도 이 값이라 End()==true 가 곧 "받는 쪽이 받는다" (옛 65536 은 USHORT 길이 필드 천장 65535 조차 넘는 거짓 값 - 소비처는 descramble 스크래치 1곳뿐이었다)

static const int SEND_WATERMARK = 12288;        // 송신 큐 워터마크 - SEND_BUFFER_SIZE(16384)의 75%. Enqueue 후 이 선을 처음 넘긴 횟수를 관측(백프레셔 조기신호). raw 숫자, 측정 후 조정

// 연결 끊는 이유 (정상 종료인지 비정상 reset인지 -> 소켓 닫는 방식이 갈림)
enum class EDisconnectReason : int
{
	SEND_TIMEOUT,       // 송신이 너무 오래 안 끝남, 비정상 -> RST로 강제 끊기
	RECV_FAIL,          // 수신 실패, 비정상 -> RST
	PROTOCOL_VIOLATION, // 패킷 규약 위반(잘못된 길이/형식) -> 강제 끊기
	CLIENT_FIN,         // 클라가 정상으로 연결 닫음 -> graceful 종료
	SERVER_SHUTDOWN,    // 서버 종료 -> graceful 종료
	IDLE_TIMEOUT,       // 오래 아무것도 안 보냄 -> 끊기
	NET_RESET,          // 비정상 reset (recv 완료가 실패로 옴)
	SEND_FAIL,          // WSASend 동기 실패 / 송신 완료 에러
	SERVER_FULL,        // 정원 초과로 새 연결 거부
	DB_LOAD_FAILED,     // 로그인 중 DB 캐릭터 로드 실패 -> RST 강제 종료(player-less 세션 정리, 서버 TIME_WAIT 회피)
	NO_RECV_TIMEOUT,    // 연결 후 데드라인까지 무수신(무음 소켓) -> RST 강제 종료(슬롯 고갈 방어, TIME_WAIT 회피). 도메인 무지: 인증 여부 모름, 순수 I/O 무수신
	LOGIN_REJECTED,     // 로그인 거부(SC_LOGIN_RESULT{FAIL} 송신 후) -> FIN 정상 종료 (RST면 그 거부 통지가 버려짐)
	COUNT               // 센티널 - 값 수(배열 폭/이탈 분포 집계 폭의 SSOT). 항상 마지막, 실제 사유 아님
};

// 비정상 종료(RST 강제 끊기)인지 판정 - Session::Disconnect 의 소켓 닫는 방식이 이 술어 하나로 갈린다(RST 집합 SSOT).
//   true  = linger{1,0} 즉시 RST (송신 버퍼 버림, TIME_WAIT 회피).
//   false = FIN 정상 종료 (송신 버퍼 마저 보냄). LOGIN_REJECTED/SERVER_FULL 은 실패 통지를 먼저 보내고 끊으므로 반드시 여기(FIN).
inline bool IsAbnormal(EDisconnectReason reason)
{
	return reason == EDisconnectReason::SEND_TIMEOUT
		|| reason == EDisconnectReason::RECV_FAIL
		|| reason == EDisconnectReason::PROTOCOL_VIOLATION
		|| reason == EDisconnectReason::NET_RESET
		|| reason == EDisconnectReason::SEND_FAIL
		|| reason == EDisconnectReason::DB_LOAD_FAILED
		|| reason == EDisconnectReason::NO_RECV_TIMEOUT;
}



// ===== 에러 코드 (성공 0, 실패는 음수) =====

enum class ErrorCode : int
{
	SUCCESS = 0,            // 성공


	FAILED = -1,           // 일반 실패
	INVALID_PARAMETER = -2,// 잘못된 인자
	OUT_OF_MEMEORY=-3,     // 메모리 부족

	NETWORK_INIT_FAILED=-100,  // 네트워크 초기화 실패 (WSAStartup 등)
	SOCKET_CREATE_FAILED=-101, // 소켓 생성 실패
	SEND_FAILED=-105,          // 송신 실패


	THREAD_CREATE_FAILED=-300, // 스레드 생성 실패
	MEMEORY_POOL_FULL=-301     // 메모리 풀 꽉 참

};


static_assert(MAX_PACKET_SIZE <= RECV_BUFFER_SIZE, "패킷 하나는 수신 버퍼에 통째로 담겨야 한다 - 이 등식이 깨지면 End()==true 인 패킷을 받는 쪽이 끊는다");
static_assert(MAX_PACKET_SIZE <= 65535, "길이 접두가 USHORT(LENGTH_PREFIX_SIZE 2) 라 65535 가 wire 천장");

// 송신 큐가 넘쳐 데이터를 버려야 할 때 그 연결을 어떻게 할지 - 호출자가 Send/SendTo 마다 넘긴다.
//   라이브러리는 "이 스트림이 손실을 견디는가" 만 받고 그 이유(예: 게임측 rolling 키가 이미 전진해 한 번 버리면 다시 못 맞춘다)는 모른다.
//   reaper 의 데드라인처럼 메커니즘은 이 층, 값은 호출마다 앱 몫. 처분(끊기 + 축출 집계)은 넘침이 일어나는 그 줄(Session::Send)에서 끝낸다.
enum class ESendDropPolicy : int
{
	KEEP_CONNECTION = 0,   // 버려도 연결 유지 - 호출자가 재전송/재요청으로 복원하는 스트림 (pre-auth 응답 · 거부 통지 · 인터서버 링크)
	EVICT_ON_DROP   = 1    // 넘치면 그 자리에서 Disconnect(SEND_TIMEOUT) - 한 번 버리면 다시 못 맞추는 스트림 (post-auth 난독화 경로)
};
