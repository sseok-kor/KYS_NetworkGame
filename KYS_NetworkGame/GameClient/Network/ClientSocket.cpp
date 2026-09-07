#include "pch_gameclient.h"
#include "ClientSocket.h"
#include "../Handler/PacketProcessor.h"
#include "Protocol/CPacket.h"
#include "Protocol/GamePackets.h"
#include "Protocol/PacketType.h"
#include "Protocol/PacketHeader.h"

using namespace KYS::GAMECOMMON::PROTOCOL;

ClientSocket::ClientSocket()
	: m_sock(INVALID_SOCKET)
	, m_token(0)
	, m_redirectIp(0)
	, m_redirectPort(0)
	, m_recvLen(0)
	, m_recvBytesTotal(0)
	, m_recvPacketsTotal(0)
	, m_lastPacketType(0)
	, m_sendKey{0}
	, m_recvKey{0}
	, m_obfArmed(false)
	, m_sendFailed(false)
{
}

ClientSocket::~ClientSocket()
{
	Close();
}

void ClientSocket::Close()
{
	if (m_sock != INVALID_SOCKET)
	{
		::closesocket(m_sock);
		m_sock = INVALID_SOCKET;
	}
}

// 로그인 서버로 blocking TCP 소켓을 열고 5s recv 타임아웃을 건 뒤 연결한다.
//   AcquireToken/RegisterAccount 공용. 실패 시(생성/주소변환/connect 어느 단계든) 만든 소켓을 닫고 INVALID_SOCKET 반환.
static SOCKET OpenBlockingLoginSocket(const wchar_t* host, unsigned short port)
{
	SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (ls == INVALID_SOCKET)
		return INVALID_SOCKET;

	DWORD timeout = 5000;   // recv 타임아웃 (서버 무응답 시 영구 대기 방지)
	::setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

	sockaddr_in addr;
	::ZeroMemory(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = ::htons(port);
	if (::InetPtonW(AF_INET, host, &addr.sin_addr) != 1) { ::closesocket(ls); return INVALID_SOCKET; }
	if (::connect(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { ::closesocket(ls); return INVALID_SOCKET; }

	return ls;
}

// ===== Stage 1: 로그인 서버에서 일회용 토큰 획득 (blocking) =====
bool ClientSocket::AcquireToken(const wchar_t* loginIp, unsigned short loginPort, const wchar_t* id, const wchar_t* pw)
{
	m_token = 0;
	m_redirectIp = 0;
	m_redirectPort = 0;

	// wire 고정 배열(id[16]/pw[32]) 초과 입력은 송신 전 거부 - wcscpy_s 는 절단하지 않고 프로세스를 종료시킨다(서버 IsValidCredential 과 동일 상한).
	if (::wcslen(id) >= LoginReq::ID_MAX || ::wcslen(pw) >= PASSWORD_MAX)
		return false;

	SOCKET ls = OpenBlockingLoginSocket(loginIp, loginPort);
	if (ls == INVALID_SOCKET)
		return false;

	const int headerSize = static_cast<int>(sizeof(PacketHeader));
	BYTE frame[1024];
	int  frameLen = 0;

	// (1) CS_LOGIN 송신
	{
		CPacket out(MAX_PACKET_SIZE);
		out.Begin(static_cast<USHORT>(PacketType::CS_LOGIN));
		LoginReq req;
		::ZeroMemory(&req, sizeof(req));
		::wcscpy_s(req.id, LoginReq::ID_MAX, id);
		::wcscpy_s(req.pw, PASSWORD_MAX, pw);   // 평문 비번 (서버가 PBKDF2 해시 후 대조)
		req.Serialize(out);
		if (!out.End()) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); ::closesocket(ls); return false; }
		if (!BlockingSendAll(ls, out.GetBuffer(), out.GetSize())) { ::closesocket(ls); return false; }
	}

	// (2) SC_SERVER_LIST 수신 (타입만 검증 - 서버 1개라 선택은 0번 고정)
	if (!BlockingRecvFrame(ls, frame, static_cast<int>(sizeof(frame)), frameLen)) { ::closesocket(ls); return false; }
	if (frameLen < headerSize ||
		::ntohs(*reinterpret_cast<UINT16*>(frame + sizeof(UINT16))) != static_cast<USHORT>(PacketType::SC_SERVER_LIST))
	{ ::closesocket(ls); return false; }

	// (3) CS_SERVER_SELECT(0) 송신
	{
		CPacket out(MAX_PACKET_SIZE);
		out.Begin(static_cast<USHORT>(PacketType::CS_SERVER_SELECT));
		CS_SERVER_SELECT sel;
		::ZeroMemory(&sel, sizeof(sel));
		sel.serverId = 0;
		sel.Serialize(out);
		if (!out.End()) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); ::closesocket(ls); return false; }
		if (!BlockingSendAll(ls, out.GetBuffer(), out.GetSize())) { ::closesocket(ls); return false; }
	}

	// (4) SC_LOGIN_TOKEN 수신 -> 토큰 + 게임 서버 redirect 추출
	if (!BlockingRecvFrame(ls, frame, static_cast<int>(sizeof(frame)), frameLen)) { ::closesocket(ls); return false; }
	if (frameLen <= headerSize ||
		::ntohs(*reinterpret_cast<UINT16*>(frame + sizeof(UINT16))) != static_cast<USHORT>(PacketType::SC_LOGIN_TOKEN))
	{ ::closesocket(ls); return false; }
	{
		CPacket pkt(frameLen - headerSize);
		pkt.Write(frame + headerSize, frameLen - headerSize);
		SC_LOGIN_TOKEN tok;
		::ZeroMemory(&tok, sizeof(tok));
		tok.Deserialize(pkt);
		m_token = tok.token;
		m_redirectIp = tok.serverIp;       // 게임 서버 redirect 주소 - main 의 접속부가 GetRedirectIp() 로 읽어 이 주소로 ConnectGame(0 이면 서버 오설정으로 접속 중단)
		m_redirectPort = tok.serverPort;
	}

	::closesocket(ls);
	return m_token != 0;
}

// ===== 계정 생성: CS_REGISTER -> SC_REGISTER_RESULT (별도 임시 소켓 blocking, 5s 타임아웃) =====
bool ClientSocket::RegisterAccount(const wchar_t* loginIp, unsigned short loginPort, const wchar_t* id, const wchar_t* pw, BYTE& outResult)
{
	outResult = static_cast<BYTE>(ECreateAccountResult::DB_ERROR);   // 통신 실패 시 기본값(서버 미응답)

	// wire 고정 배열(id[16]/pw[32]) 초과 입력은 송신 전 로컬 거부 - wcscpy_s 비절단 종료 방지. 결과는 INVALID 로 확정(서버 왕복 불요).
	if (::wcslen(id) >= LoginReq::ID_MAX || ::wcslen(pw) >= PASSWORD_MAX)
	{
		outResult = static_cast<BYTE>(ECreateAccountResult::INVALID);
		return true;
	}

	SOCKET ls = OpenBlockingLoginSocket(loginIp, loginPort);
	if (ls == INVALID_SOCKET)
		return false;

	const int headerSize = static_cast<int>(sizeof(PacketHeader));
	BYTE frame[256];
	int  frameLen = 0;

	// CS_REGISTER 송신 (id + 평문 비번)
	{
		CPacket out(MAX_PACKET_SIZE);
		out.Begin(static_cast<USHORT>(PacketType::CS_REGISTER));
		CS_REGISTER req;
		::ZeroMemory(&req, sizeof(req));
		::wcscpy_s(req.id, LoginReq::ID_MAX, id);
		::wcscpy_s(req.pw, PASSWORD_MAX, pw);
		req.Serialize(out);
		if (!out.End()) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); ::closesocket(ls); return false; }
		if (!BlockingSendAll(ls, out.GetBuffer(), out.GetSize())) { ::closesocket(ls); return false; }
	}

	// SC_REGISTER_RESULT 수신
	if (!BlockingRecvFrame(ls, frame, static_cast<int>(sizeof(frame)), frameLen)) { ::closesocket(ls); return false; }
	::closesocket(ls);

	if (frameLen <= headerSize ||
		::ntohs(*reinterpret_cast<UINT16*>(frame + sizeof(UINT16))) != static_cast<USHORT>(PacketType::SC_REGISTER_RESULT))
		return false;

	CPacket pkt(frameLen - headerSize);
	pkt.Write(frame + headerSize, frameLen - headerSize);
	SC_REGISTER_RESULT res;
	::ZeroMemory(&res, sizeof(res));
	res.Deserialize(pkt);
	outResult = res.result;
	return true;
}

// ===== Stage 2: 게임 서버 접속 + CS_GAME_AUTH + 비차단 전환 =====
bool ClientSocket::ConnectGame(const wchar_t* gameIp, unsigned short gamePort)
{
	m_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_sock == INVALID_SOCKET)
		return false;

	sockaddr_in addr;
	::ZeroMemory(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = ::htons(gamePort);
	if (::InetPtonW(AF_INET, gameIp, &addr.sin_addr) != 1) { Close(); return false; }

	// blocking connect (단발).
	if (::connect(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { Close(); return false; }

	// 첫 패킷 CS_GAME_AUTH 는 소켓이 아직 blocking 일 때 보내 확실히 전송한다.
	if (!SendGameAuth()) { Close(); return false; }

	// 이후 수신 폴링을 위해 비차단으로 전환 (게임루프의 recv 폴링과 정합 - select 는 쓰지 않는다).
	u_long nonblocking = 1;
	if (::ioctlsocket(m_sock, FIONBIO, &nonblocking) != 0) { Close(); return false; }

	m_recvLen = 0;
	m_obfArmed = false;   // 새 연결 = pre-auth 평문으로 복귀 (재사용 소켓 객체의 직전 연결 무장 잔여 봉인 - 서버 슬롯 회수 키 리셋과 대칭)
	m_sendFailed = false; // 새 연결 = 직전 연결의 송신 실패 상태 리셋
	return true;
}

// SC_ENTER_WORLD(admission 성공)을 평문으로 받은 직후 호출 - 이 소켓의 방향별 키를 심고 무장한다 (서버 SeedObfKeys 와 방향 대칭).
void ClientSocket::ArmObfuscation()
{
	InitObfKey(m_sendKey, m_token, ObfDirection::CLIENT_TO_SERVER);   // 내가 보내는 방향 (서버 recv 와 일치)
	InitObfKey(m_recvKey, m_token, ObfDirection::SERVER_TO_CLIENT);   // 내가 받는 방향 (서버 send 와 일치)
	m_obfArmed = true;
}

bool ClientSocket::SendGameAuth()
{
	CPacket out(MAX_PACKET_SIZE);
	out.Begin(static_cast<USHORT>(PacketType::CS_GAME_AUTH));
	CS_GAME_AUTH req;
	::ZeroMemory(&req, sizeof(req));
	req.token = m_token;   // Stage 1에서 받은 일회용 토큰
	req.Serialize(out);
	if (!out.End()) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); return false; }
	return BlockingSendAll(m_sock, out.GetBuffer(), out.GetSize());
}

// ===== 매 프레임 비차단 수신 + framing =====
bool ClientSocket::PollRecv(PacketProcessor& processor)
{
	if (m_sock == INVALID_SOCKET)
		return false;

	for (;;)
	{
		int space = RECV_BUFFER_SIZE - m_recvLen;
		if (space <= 0) { m_recvLen = 0; space = RECV_BUFFER_SIZE; }   // 과대 프레임 방어 (정상 wire 에선 발생 X)

		const int n = ::recv(m_sock, reinterpret_cast<char*>(m_recvBuf + m_recvLen), space, 0);
		if (n > 0)
		{
			m_recvBytesTotal += n;
			m_recvLen += n;
			FrameAndDispatch(processor);
			continue;   // 비차단이라 더 있을 수 있다 - WSAEWOULDBLOCK 까지 drain
		}
		if (n == 0)
			return false;   // graceful close (서버 종료 또는 중복 로그인 kick)

		const int err = ::WSAGetLastError();
		if (err == WSAEWOULDBLOCK)
			break;          // 더 받을 데이터 없음 - 정상
		return false;       // 진짜 소켓 에러
	}
	return true;
}

// 누적 버퍼에서 완성된 패킷들을 잘라 디스패치. 부분 패킷은 앞으로 당겨 보존.
void ClientSocket::FrameAndDispatch(PacketProcessor& processor)
{
	const int headerSize = static_cast<int>(sizeof(PacketHeader));
	int offset = 0;
	while (m_recvLen - offset >= headerSize)
	{
		const UINT16 pktSize = ::ntohs(*reinterpret_cast<UINT16*>(m_recvBuf + offset));   // 앞 2B = 전체 길이(BE)
		if (pktSize < static_cast<UINT16>(sizeof(PacketHeader)) || static_cast<int>(pktSize) > RECV_BUFFER_SIZE)
		{
			m_recvLen = 0;   // 길이 비정상 = wire 깨짐 -> 버퍼 리셋
			offset = 0;
			break;
		}
		if (m_recvLen - offset < static_cast<int>(pktSize))
			break;   // 부분 패킷 - 다음 recv 까지 보존

		if (m_obfArmed)
			DeobfuscateOpcode(m_recvBuf + offset, m_recvKey);   // opcode 평문화 + recv 키 advance (완성 패킷당 정확히 1회)

		const UINT16 pktType = ::ntohs(*reinterpret_cast<UINT16*>(m_recvBuf + offset + sizeof(UINT16)));   // 다음 2B = type(BE)
		m_lastPacketType = pktType;
		++m_recvPacketsTotal;
		const BYTE* payload    = m_recvBuf + offset + headerSize;
		const int   payloadLen = static_cast<int>(pktSize) - headerSize;
		processor.Dispatch(pktType, payload, payloadLen);

		// SC_ENTER_WORLD(입장 완료 = admission-ack)를 평문으로 받은 직후 무장한다. 이 패킷 자체는 아직 unarmed 라 정상 판독되고,
		// 같은 루프의 다음 패킷(SC_INVENTORY)부터 m_obfArmed 로 descramble 된다(무장이 다음 반복에 동기 반영).
		// 성공 경로만 이 opcode 를 보내므로(거부/인증실패는 SC_LOGIN_RESULT{FAIL}) payload 검사 없이 opcode 수신만으로 무장 - 과부하된 SC_LOGIN_RESULT 결합을 끊는다.
		if (!m_obfArmed
			&& pktType == static_cast<UINT16>(PacketType::SC_ENTER_WORLD))
			ArmObfuscation();

		offset += pktSize;   // 완성 패킷 소비
	}

	if (offset > 0)
	{
		m_recvLen -= offset;
		if (m_recvLen > 0)
			::memmove(m_recvBuf, m_recvBuf + offset, static_cast<size_t>(m_recvLen));
	}
}

bool ClientSocket::Send(KYS::GAMECOMMON::PROTOCOL::CPacket& packet)
{
	if (m_sock == INVALID_SOCKET)
		return false;

	BYTE*     data = packet.GetBuffer();
	const int size = packet.GetSize();
	if (m_obfArmed && size >= static_cast<int>(sizeof(PacketHeader)))
		ObfuscateOpcode(data, m_sendKey);   // opcode scramble + send 키 advance (송신 직전)

	int sent = 0;
	while (sent < size)
	{
		const int n = ::send(m_sock, reinterpret_cast<const char*>(data + sent), size - sent, 0);
		if (n > 0) { sent += n; continue; }
		if (n == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK)
			continue;   // 송신 버퍼 일시 가득 - 작은 패킷이라 곧 비워짐
		m_sendFailed = true;   // armed 였다면 send 키가 이미 advance 됨 -> main 이 감지해 연결 teardown(재접속으로 키 재seed)
		return false;   // 진짜 에러
	}
	return true;
}

bool ClientSocket::BlockingSendAll(SOCKET s, const BYTE* data, int size)
{
	int sent = 0;
	while (sent < size)
	{
		const int n = ::send(s, reinterpret_cast<const char*>(data + sent), size - sent, 0);
		if (n == SOCKET_ERROR || n == 0)
			return false;
		sent += n;
	}
	return true;
}

bool ClientSocket::BlockingRecvFrame(SOCKET s, BYTE* outFrame, int outCap, int& outLen)
{
	int have = 0;
	while (have < static_cast<int>(sizeof(UINT16)))   // 길이 2B 먼저 확보
	{
		const int n = ::recv(s, reinterpret_cast<char*>(outFrame + have), outCap - have, 0);
		if (n <= 0)
			return false;
		have += n;
	}
	const int pktSize = static_cast<int>(::ntohs(*reinterpret_cast<UINT16*>(outFrame)));
	if (pktSize < static_cast<int>(sizeof(PacketHeader)) || pktSize > outCap)
		return false;
	while (have < pktSize)   // 나머지 수신
	{
		const int n = ::recv(s, reinterpret_cast<char*>(outFrame + have), outCap - have, 0);
		if (n <= 0)
			return false;
		have += n;
	}
	outLen = pktSize;
	return true;
}
