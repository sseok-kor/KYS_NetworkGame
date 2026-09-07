#pragma once
#include "Protocol/PacketObfuscator.h"   // ObfKeyState 멤버 + InitObfKey/ObfuscateOpcode/DeobfuscateOpcode (opcode 난독화 방향별 rolling 키)

// CPacket 전방 선언 (송신 인자용). 정의는 .cpp 에서 include.
namespace KYS { namespace GAMECOMMON { namespace PROTOCOL { class CPacket; } } }

class PacketProcessor;   // 수신 디스패치 대상 (정의는 .cpp 에서 include)

// 게임 클라이언트의 단일 TCP 연결 + 2단계 로그인 핸드셰이크 + 비차단 framing 수신.
// D3=A(select/비차단 단일스레드): 게임루프가 매 프레임 PollRecv 로 비차단 drain (전용 recv 스레드/락 0).
// 송수신 wire 와 framing 은 DummyClient(DummySession) 를 1:1 미러한다.
class ClientSocket
{
public:
	ClientSocket();
	~ClientSocket();

	ClientSocket(const ClientSocket&) = delete;
	ClientSocket& operator=(const ClientSocket&) = delete;

	// Stage 1: 로그인 서버에서 일회용 토큰 획득 (별도 임시 소켓 blocking, 5s 타임아웃).
	// 성공 시 m_token + 게임 서버 redirect 주소를 보관. pw = 평문 비번(서버가 PBKDF2 해시 후 대조).
	bool AcquireToken(const wchar_t* loginIp, unsigned short loginPort, const wchar_t* id, const wchar_t* pw);

	// 계정 생성 (별도 임시 소켓 blocking): CS_REGISTER 송신 -> SC_REGISTER_RESULT 수신.
	// 반환 = 결과 확정 여부(true: outResult 유효 - 서버 응답 또는 로컬 입력 거부 / false: 통신 실패).
	// outResult = ECreateAccountResult 값(OK/DUP_ID/INVALID/DB_ERROR).
	bool RegisterAccount(const wchar_t* loginIp, unsigned short loginPort, const wchar_t* id, const wchar_t* pw, BYTE& outResult);

	// Stage 2: 게임 서버 접속(blocking) -> CS_GAME_AUTH 송신(blocking, 첫 패킷) -> 비차단 전환.
	bool ConnectGame(const wchar_t* gameIp, unsigned short gamePort);

	// 매 프레임 비차단 수신 + framing 절단 + 통계 갱신 + SC 디스패치. 반환: 연결 유지(false=끊김).
	bool PollRecv(PacketProcessor& processor);

	// 게임 패킷 송신 (CPacket 으로 빌드된 것, 비차단, CS_MOVE 등 S3+ 용).
	bool Send(KYS::GAMECOMMON::PROTOCOL::CPacket& packet);

	// SC_ENTER_WORLD(admission-ack 성공) 수신 직후 호출 - 이 소켓의 방향별 키를 심고 무장(이후 opcode scramble). 서버 admission SeedObfKeys 와 방향 대칭.
	void ArmObfuscation();

	void Close();

	bool               IsConnected() const { return m_sock != INVALID_SOCKET; }
	// 송신 중 실제 에러 발생 여부를 소비(읽고 클리어). armed send 가 실패하면 send 키만 advance 되고 패킷은 안 나가 서버 recv 키와 영구 desync 되므로,
	// main 이 매 프레임 이 값을 확인해 연결을 내린다(재접속이 무장 리셋 + 다음 arm 재seed 로 키 복구). recv 가 멀쩡해도 끊어야 하는 케이스.
	bool               TakeSendFailed() { const bool failed = m_sendFailed; m_sendFailed = false; return failed; }
	unsigned long long GetToken() const { return m_token; }
	UINT32             GetRedirectIp() const { return m_redirectIp; }     // 로그인 서버가 알려준 게임 서버 IP(호스트 정수) - 이 주소로만 ConnectGame 한다. 0 = 서버 오설정(폴백 없음)
	USHORT             GetRedirectPort() const { return m_redirectPort; } // 로그인 서버가 알려준 게임 서버 포트
	long long          GetRecvBytes() const { return m_recvBytesTotal; }
	long long          GetRecvPackets() const { return m_recvPacketsTotal; }
	unsigned short     GetLastPacketType() const { return m_lastPacketType; }

private:
	bool SendGameAuth();       // ConnectGame 내부 (blocking, 토큰 제출 첫 패킷)
	void FrameAndDispatch(PacketProcessor& processor);   // m_recvBuf 의 완성 패킷들을 잘라 디스패치 (부분 패킷 보존)

	static bool BlockingSendAll(SOCKET s, const BYTE* data, int size);
	static bool BlockingRecvFrame(SOCKET s, BYTE* outFrame, int outCap, int& outLen);

	SOCKET m_sock;
	UINT64 m_token;
	UINT32 m_redirectIp;       // SC_LOGIN_TOKEN.serverIp (호스트 정수, 0=미설정) - 접속 시 redirect 존중(GetRedirectIp)
	USHORT m_redirectPort;     // SC_LOGIN_TOKEN.serverPort

	BYTE m_recvBuf[RECV_BUFFER_SIZE];   // framing 누적 버퍼
	int  m_recvLen;                     // 현재 쌓인 바이트 수

	long long      m_recvBytesTotal;    // 통계: 누적 수신 바이트
	long long      m_recvPacketsTotal;  // 통계: 누적 완성 패킷 수
	unsigned short m_lastPacketType;    // 통계: 마지막 수신 패킷 종류

	// opcode 난독화 방향별 rolling 키 (서버 GameSession 과 대칭). armed 전엔 평문(pre-auth), SC_ENTER_WORLD 수신 후 무장.
	KYS::GAMECOMMON::PROTOCOL::ObfKeyState m_sendKey;   // C2S 방향 (내가 보냄 = 서버 recv 키와 같은 시드)
	KYS::GAMECOMMON::PROTOCOL::ObfKeyState m_recvKey;   // S2C 방향 (내가 받음 = 서버 send 키와 같은 시드)
	bool                                   m_obfArmed;  // 키 무장 여부 (false=평문 / true=scramble)
	bool                                   m_sendFailed;  // Send 중 실제 에러 발생(송신 키만 advance, 패킷 미전송) - main 이 소비해 연결 teardown
};
