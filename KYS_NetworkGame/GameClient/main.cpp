#include "pch_gameclient.h"

#include "Network/ClientSocket.h"
#include "Handler/PacketProcessor.h"
#include "Game/GameState.h"
#include "Game/LocalPlayer.h"
#include "Game/PortalLayout.h"
#include "MapData/PortalTable.h"          // LoadPortalTable (공유 포탈 표 - 서버와 같은 Data/portals.csv)
#include "ItemData/ItemTable.h"            // LoadItemTable / FindItemDef (공유 아이템 사전 - 서버와 같은 Data/items.csv)
#include "MapData/MapTable.h"             // LoadMapTable (공유 맵 메타 - 맵별 크기. 서버와 같은 Data/maps.csv)
#include "MapData/WalkableTable.h"        // LoadWalkableTable (통행 격자 - 이동 예측이 서버와 같은 벽에서 멈추게)
#include "Pathfinding/Direction.h"            // DirectionToward (WASD 축 델타 -> 8방 - 서버 조향과 같은 양자화)
#include "Render/GameRenderer.h"
#include "Protocol/CPacket.h"
#include "Protocol/GamePackets.h"
#include "Protocol/PacketType.h"
#include "GameDefines.h"   // PLAYER_RESPAWN_DELAY_SEC (사망 후 자동 부활 카운트다운 표시) / LOGIN_CLIENT_PORT (로그인 서버 클라 대면 포트)

// ImGui + DX11 백엔드.
// 외부 라이브러리 헤더는 /W4 경고를 발생시키므로 push(0)/pop 으로 감싸
// 우리 코드의 "경고 0" 정책과 격리한다.
#pragma warning(push, 0)
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <d3d11.h>
#include <dxgi.h>
#pragma warning(pop)

// imgui_impl_win32.cpp 가 정의하는 Win32 메시지 핸들러 전방 선언.
// imgui_impl_win32.h 는 이 선언을 #if 0 블록 안에 두어 사용자 .cpp 가 직접 선언하게 한다.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 자기 예측(dead-reckon) 고정 스텝 - 서버 30Hz tick(33333us) 과 동일해야 비트 일치.
static const double kTickSeconds = 33333.0 / 1000000.0;
static const float  kSimDt       = 33333.0f / 1000000.0f;

// 클라 화면 상태 (단일 상태변수 - 매 프레임 derive 후 화면별 분기). 화면이 늘어 불리언 조합 파생을 enum 으로 대체.
//   Login/AccountCreate = pre-connect 클라-로컬(연결 없음), 나머지는 서버 반응(연결 후 패킷 상태로 derive).
//   접속 흐름: Connecting -> ChannelSelect(채널 목록) -> CharSelect(캐릭터 목록) -> [CharCreate] -> InGame(캐릭터 선택=admission).
enum class ClientScreen { Login, AccountCreate, Connecting, ChannelSelect, CharSelect, CharCreate, InGame };

// DX11 전역 디바이스 - 단일 윈도우라 전역 보유(클라 1개).
static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_deviceContext = nullptr;
static IDXGISwapChain*         g_swapChain = nullptr;
static ID3D11RenderTargetView* g_renderTargetView = nullptr;

// UTF-8 char 입력(ImGui InputText) -> wchar_t (소켓 W-API / 패킷 id) 변환.
static void ToWide(const char* src, wchar_t* dst, int dstCap)
{
	const int n = ::MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dstCap);
	if (n <= 0 && dstCap > 0)
		dst[0] = L'\0';
}

// 서버 wchar_t 문자열 -> ImGui 표시용 UTF-8 char (채팅 로그 표시). 스택 버퍼.
static const char* ToUtf8(const wchar_t* src, char* dst, int dstCap)
{
	const int n = ::WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstCap, nullptr, nullptr);
	if (n <= 0 && dstCap > 0)
		dst[0] = '\0';
	return dst;
}

static bool CreateRenderTarget()
{
	ID3D11Texture2D* backBuffer = nullptr;
	if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || backBuffer == nullptr)
		return false;
	const HRESULT hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTargetView);
	backBuffer->Release();
	return SUCCEEDED(hr);
}

static void CleanupRenderTarget()
{
	if (g_renderTargetView) { g_renderTargetView->Release(); g_renderTargetView = nullptr; }
}

static bool CreateDeviceD3D(HWND hwnd)
{
	DXGI_SWAP_CHAIN_DESC desc;
	::ZeroMemory(&desc, sizeof(desc));
	desc.BufferCount = 2;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferDesc.RefreshRate.Numerator = 60;
	desc.BufferDesc.RefreshRate.Denominator = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.OutputWindow = hwnd;
	desc.SampleDesc.Count = 1;
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
	const HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, 2, D3D11_SDK_VERSION,
		&desc, &g_swapChain, &g_device, &chosen, &g_deviceContext);
	if (FAILED(hr))
		return false;
	return CreateRenderTarget();
}

static void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_swapChain)     { g_swapChain->Release();     g_swapChain = nullptr; }
	if (g_deviceContext) { g_deviceContext->Release(); g_deviceContext = nullptr; }
	if (g_device)        { g_device->Release();        g_device = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// ImGui win32 백엔드가 먼저 입력(키보드/마우스)을 가로채게 한다.
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return 1;

	switch (msg)
	{
	case WM_SIZE:
		// 창 크기 변경 시 렌더 타겟 재생성 (최소화는 무시).
		if (g_device != nullptr && wParam != SIZE_MINIMIZED)
		{
			CleanupRenderTarget();
			g_swapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)),
				DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();
		}
		return 0;
	case WM_DESTROY:
		::PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

int main()
{
	// (0) Winsock 초기화
	WSADATA wsa;
	if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return 1;

	// 공유 포탈 표 적재 (서버와 같은 Data/portals.csv). 실패해도 GUI는 뜬다(포탈만 미표시).
	LoadPortalTable();
	// 공유 아이템 사전 적재 (서버와 같은 Data/items.csv). 클라는 관대 - 실패해도 GUI는 뜬다(이름 대신 item#N 표시).
	LoadItemTable();
	// 공유 맵 메타 + 통행 격자 적재 (서버와 같은 Data/maps.csv + 통행 CSV) - 이동 예측이 서버와 같은 벽에서
	//   멈추기 위한 데이터. 클라는 관대 - 실패하면 "벽을 모르는 강등 모드"(전 칸 통행 + 경계 4000 폴백)로 뜬다:
	//   플레이는 되지만 벽 근처에서 서버 보정 되당김이 생긴다. 화면 경고는 렌더러가 미적재 상태를 보고 띄운다.
	if (LoadMapTable() < 0 || LoadWalkableTable() < 0)
	{
		wprintf(L"[client] 맵/통행 데이터 적재 실패 - 지형 강등 모드로 진행 (벽 근처 위치 보정 발생 가능)\n");
	}

	// (1) 윈도우 등록 + 생성 (W-API)
	WNDCLASSEXW wc;
	::ZeroMemory(&wc, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.style = CS_CLASSDC;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = ::GetModuleHandleW(nullptr);
	wc.lpszClassName = L"KYS_GameClient";
	::RegisterClassExW(&wc);

	HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"KYS NetworkGame - GameClient",
		WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800,
		nullptr, nullptr, wc.hInstance, nullptr);

	// (2) DX11 디바이스/스왑체인
	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		::UnregisterClassW(wc.lpszClassName, wc.hInstance);
		::WSACleanup();
		return 1;
	}

	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	// (3) ImGui 초기화 + 한글 폰트
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr; // imgui.ini 비생성
	// 맑은 고딕 + 한글 글리프 범위(0xAC00~0xD7A3). 폰트 로드 실패 시 기본 폰트로 폴백(한글 미표시).
	io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesKorean());
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_device, g_deviceContext);

	// (3-1) 네트워크 + 게임 상태 + 렌더러
	ClientSocket    clientSocket;
	LocalPlayer     localPlayer;
	GameState       gameState;
	PacketProcessor packetProcessor(localPlayer, gameState);
	GameRenderer    gameRenderer;
	char inputServerIp[64] = "127.0.0.1";
	char inputId[32] = "bot00000";
	char inputPw[64] = "loadtest";    // 비밀번호 입력(UTF-8) - 봇 계정 기본값(데모 편의)
	bool accountCreateOpen = false;   // true = 계정 생성 화면(연결 전 클라-로컬 토글)
		char chatInput[256] = "";   // 채팅 입력 버퍼(UTF-8)
	char statusMsg[256] = "";

	// 캐릭터 선택/생성 화면 상태 (채널 선택 후~입장 전).
	bool charCreateOpen = false;      // true = 캐릭터 생성 폼(빈 슬롯 "생성" 누름). 생성 OK/취소면 false 로 선택 화면 복귀
	BYTE charCreateSlot = 0;          // 생성 폼이 채울 슬롯 번호
	char charNameInput[64] = "";      // 캐릭터 이름 입력(UTF-8)
	char charStatusMsg[256] = "";     // 캐릭터 화면 피드백(생성/삭제 결과 사유)
	bool admissionPending = false;    // CS_CHARACTER_SELECT 송신 후 admission 응답(SC_ENTER_WORLD/배치) 대기 - 캐릭터 관리 버튼 비활성(pending 창 stray C2S 봉인)
	unsigned int admissionListVersion = 0;   // pending 시작 시 캐릭터 목록 버전 스냅샷 - 서버 재푸시(거부/재리스트)를 감지해 pending 해제

	// 인게임 채널 변경(C) 상태 (in-place handoff - 같은 캐릭터로 다른 채널 이동).
	bool channelChangeOpen = false;   // true = 인게임 채널 picker 오버레이 열림
	char channelChangeMsg[128] = "";  // 채널 변경 피드백(성공/실패 사유)
	bool channelChangePending = false;   // CS_CHANNEL_CHANGE 송신 후 결과 대기 - picker 버튼 비활성화(우발적 이중 이관 방지)

	// 아이템/거래 UI 상태.
	bool inventoryOpen = false;       // 인벤 창 열림 (I 키 토글)
	char itemStatusMsg[128] = "";     // 아이템/거래 피드백(실패 사유/결과 토스트)
	int  tradeAddQuantity = 1;        // 거래창에 올릴 수량 입력(가방 칸 클릭 시 이 수량만큼)
	int  selectedBagSlot = -1;        // 인벤 창에서 선택한 가방 칸(-1 = 없음) - 장착/사용/버림 대상

	// (3-2) 30Hz dead-reckon 타이밍 (QueryPerformanceCounter - std::chrono 금지)
	LARGE_INTEGER perfFreq;
	::QueryPerformanceFrequency(&perfFreq);
	LARGE_INTEGER lastCounter;
	::QueryPerformanceCounter(&lastCounter);
	double simAccumulator = 0.0;
	double moveSendTimer = 0.0;
	double pingSendTimer = 0.0;   // keepalive - 가만히 있어도 주기적 CS_PING 으로 연결 유지(서버 idle sweep 회피 + RTT)
	double skillCooldown = 0.0;   // 스킬 연타 도배 방지(쿨다운)
	double deathElapsed = 0.0;    // 사망 후 경과 시간(자동 부활 카운트다운 표시용)
		double chatCooldown = 0.0;    // 채팅 연타 도배 방지
	double portalCooldown = 0.0;
	double pickupCooldown = 0.0;  // 줍기(Z) 연타 도배 방지
	EMoveDirection prevDir = EMoveDirection::DOWN;
	EMoveState prevState = EMoveState::STOP;

	// (4) 메인 루프
	bool running = true;
	while (running)
	{
		// (4-1) Win32 메시지 펌프 - ImGui IO 갱신에 필수.
		MSG msg;
		while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessageW(&msg);
			if (msg.message == WM_QUIT)
				running = false;
		}
		if (!running)
			break;

		// (4-2) 비차단 수신 폴링 (연결 상태일 때만) - ioctlsocket(FIONBIO)로 전환한 소켓에 recv, 단일스레드.
		if (clientSocket.IsConnected())
		{
			// recv 폴링 + 송신 실패를 함께 확인. 송신 실패(armed send 는 send 키만 advance 되고 패킷 미전송 -> 서버 recv 키와 영구 desync)는
			//   recv 가 멀쩡해도 연결을 내려야 한다(재접속이 무장 리셋 + 다음 arm 재seed 로 키 복구). 둘 중 하나라도 끊김이면 아래 정리 실행.
			const bool recvAlive = clientSocket.PollRecv(packetProcessor);
			const bool sendAlive = !clientSocket.TakeSendFailed();
			if (!recvAlive || !sendAlive)
			{
				clientSocket.Close();
				localPlayer.Reset();
				gameState.Reset();
				packetProcessor.ClearChannelList();
				packetProcessor.ClearCharacterList();   // 캐릭터 선택 단계에서 끊겨도 화면 분기가 남지 않게
				packetProcessor.ClearInGameState();     // 옛 세션 인벤/거래 창이 재접속 후 남지 않게(종료 통지는 끊긴 연결로 못 옴·Path B 대칭)
				packetProcessor.ResetRtt();   // 죽은 연결의 RTT가 끊김/재접속 후 표시되지 않게 초기화 (-1=측정 중)
				inventoryOpen = false;        // 인벤 창/선택 슬롯/아이템 메시지도 초기화(재접속 후 잔상 방지)
				selectedBagSlot = -1;
				itemStatusMsg[0] = '\0';
				charCreateOpen = false;       // 캐릭터 폼/피드백도 초기화(재접속 후 잔상 방지)
				channelChangeOpen = false;    // 채널 picker/피드백도 초기화
				channelChangeMsg[0] = '\0';
				channelChangePending = false;
				charStatusMsg[0] = '\0';
				admissionPending = false;   // 끊김 = admission pending 해제 (재접속 후 캐릭터 버튼이 잠긴 채 남지 않게)
				// 게임 중 갑작스런 끊김은 서버 종료/중복 로그인 축출, 또는 송신 실패(커널 송신 버퍼 고갈 등)일 수 있다.
				::strcpy_s(statusMsg, sendAlive
					? u8"서버 연결이 끊겼습니다 (서버 종료 또는 같은 계정 중복 로그인 축출일 수 있습니다)."
					: u8"송신 실패로 연결이 끊겼습니다 (다시 접속하세요).");
#ifdef _DEBUG
				packetProcessor.ClearRttEchoQueue();   // H6: 끊김 - 옛 세션 지연 에코가 재접속 후 적용되지 않게 비운다
#endif
			}
#ifdef _DEBUG
			else
				packetProcessor.PollDelayedEchoes();   // H6: 연결 유지 프레임 - lag 경과한 지연 self-echo 적용(off/빈 큐면 no-op)
#endif
		}

		// (4-3) ImGui 프레임 시작
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// 캐릭터 생성/삭제 결과 소비 (서버 회신). OK 면 폼 닫기, 실패면 사유 표시. 목록 표시는 재푸시된 SC_CHARACTER_LIST 가 권위.
		//   derive 전에 처리 - 생성 OK 프레임에 바로 선택 화면(charCreateOpen=false)으로 derive 되어 1프레임 지연이 없다.
		{
			BYTE createResult;
			unsigned int newCharId;
			if (packetProcessor.TakeCreateResult(createResult, newCharId))
			{
				if (createResult == static_cast<BYTE>(ECharCreateResult::OK))
				{
					charCreateOpen = false;   // 생성 성공 -> 선택 화면 복귀(재푸시된 목록에 새 캐릭터가 보임)
					::strcpy_s(charStatusMsg, u8"캐릭터가 생성되었습니다.");
				}
				else if (createResult == static_cast<BYTE>(ECharCreateResult::DUP_NAME))
					::strcpy_s(charStatusMsg, u8"이미 있는 캐릭터 이름입니다.");
				else if (createResult == static_cast<BYTE>(ECharCreateResult::INVALID))
					::strcpy_s(charStatusMsg, u8"이름이 올바르지 않습니다 (1~15자).");
				else if (createResult == static_cast<BYTE>(ECharCreateResult::SLOT_TAKEN))
					::strcpy_s(charStatusMsg, u8"그 슬롯엔 이미 캐릭터가 있습니다.");
				else
					::strcpy_s(charStatusMsg, u8"캐릭터 생성 실패 (서버 오류).");
			}
			BYTE deleteResult;
			if (packetProcessor.TakeDeleteResult(deleteResult))
			{
				if (deleteResult == static_cast<BYTE>(ECharDeleteResult::OK))
					::strcpy_s(charStatusMsg, u8"캐릭터를 삭제했습니다.");
				else if (deleteResult == static_cast<BYTE>(ECharDeleteResult::NOT_OWNED))
					::strcpy_s(charStatusMsg, u8"삭제할 수 없는 캐릭터입니다.");
				else
					::strcpy_s(charStatusMsg, u8"캐릭터 삭제 실패 (서버 오류).");
			}
		}

		// 인게임 채널 변경 결과 소비 (서버 회신). OK면 GameState.Reset은 PacketProcessor가 이미 함 - 여기선 피드백/picker 닫기만.
		{
			BYTE ccResult;
			if (packetProcessor.TakeChannelChangeResult(ccResult))
			{
				channelChangeOpen = false;   // picker 닫기(성공/실패 무관 - 실패면 현 채널 유지)
				channelChangePending = false;   // 응답 도착 - 다음 요청 허용
				if (ccResult == static_cast<BYTE>(EChannelChangeResult::OK))
					::strcpy_s(channelChangeMsg, u8"채널을 이동했습니다.");
				else if (ccResult == static_cast<BYTE>(EChannelChangeResult::FULL))
					::strcpy_s(channelChangeMsg, u8"대상 채널이 가득 찼습니다.");
				else if (ccResult == static_cast<BYTE>(EChannelChangeResult::SAME))
					::strcpy_s(channelChangeMsg, u8"이미 그 채널에 있습니다.");
				else if (ccResult == static_cast<BYTE>(EChannelChangeResult::INVALID))
					::strcpy_s(channelChangeMsg, u8"잘못된 채널입니다.");
				else if (ccResult == static_cast<BYTE>(EChannelChangeResult::RATE_LIMITED))
					::strcpy_s(channelChangeMsg, u8"너무 자주 시도했습니다. 잠시 후 다시.");
				else
					::strcpy_s(channelChangeMsg, u8"채널을 변경할 수 없습니다.");
			}
		}

		// admission(캐릭터 선택) pending 해제 - 배치되면(InGame 진입) 또는 서버가 캐릭터 목록을 재푸시하면(거부/재리스트) 다음 선택을 허용.
		//   무장(arm)은 SC_ENTER_WORLD 수신 시 ClientSocket 이 수행. 여기선 pending 창(버튼 비활성)만 닫는다. 끊김/재접속 해제는 각 지점에서 따로.
		if (admissionPending && (localPlayer.IsPlaced() || packetProcessor.GetCharacterListVersion() != admissionListVersion))
			admissionPending = false;

		// 화면 상태 단일 derive (이 프레임 기준). 이후 모든 UI/입력 분기는 이 screen 으로 한다.
		//   Login/AccountCreate = 연결 없음(accountCreateOpen 토글). 나머지는 연결 후 서버 상태로 결정.
		//   캐릭터 단계(HasCharacterList)를 채널 단계(HasChannelList)보다 먼저 검사 - 채널 선택 후 캐릭터 목록이 와도
		//   옛 채널 목록 플래그가 남아 채널 화면으로 되돌아가지 않게(캐릭터 선택이 더 진행된 단계).
		ClientScreen screen;
		if (!clientSocket.IsConnected())
			screen = accountCreateOpen ? ClientScreen::AccountCreate : ClientScreen::Login;
		else if (localPlayer.IsPlaced())
			screen = ClientScreen::InGame;
		else if (packetProcessor.HasCharacterList())
			screen = charCreateOpen ? ClientScreen::CharCreate : ClientScreen::CharSelect;
		else if (packetProcessor.HasChannelList())
			screen = ClientScreen::ChannelSelect;
		else
			screen = ClientScreen::Connecting;

		// 입력 -> 이동 의도 (WASD/화살표). 텍스트 입력 중(io.WantCaptureKeyboard)이면 정지.
		if (screen == ClientScreen::InGame)
		{
			EMoveDirection dir = localPlayer.GetDir();
			EMoveState state = EMoveState::STOP;
			if (!io.WantCaptureKeyboard && !localPlayer.IsDead())   // 사망 중엔 이동 입력 무시(STOP 유지)
			{
				// WASD/화살표를 축별로 읽어 8방 조합. 같은 축 두 키 동시(W+S)는 상쇄(그 축 없음),
				//   3키(W+A+D)는 상쇄 축을 없음 처리, 양축 다 없으면 STOP(직전 dir 유지 - 관측자 향 방향 안 튐).
				const bool up    = ImGui::IsKeyDown(ImGuiKey_UpArrow)    || ImGui::IsKeyDown(ImGuiKey_W);
				const bool down  = ImGui::IsKeyDown(ImGuiKey_DownArrow)  || ImGui::IsKeyDown(ImGuiKey_S);
				const bool left  = ImGui::IsKeyDown(ImGuiKey_LeftArrow)  || ImGui::IsKeyDown(ImGuiKey_A);
				const bool right = ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_D);
				const int horz = (right && !left) ? 1 : ((left && !right) ? -1 : 0);   // +1 오른 / -1 왼 / 0 없음(상쇄 포함)
				const int vert = (down && !up)   ? 1 : ((up && !down)   ? -1 : 0);     // +1 아래 / -1 위 / 0 없음
				if (horz != 0 || vert != 0)   // 방향 입력 있음 -> 8방 (둘 다 없으면 STOP + dir 유지)
				{
					dir = DirectionToward(horz, vert);   // 서버 몬스터 조향과 같은 양자화(축 델타 -> 8방)
					state = EMoveState::START;
				}
			}
			localPlayer.SetMoveIntent(dir, state);
		}

		// 경과 시간 -> 30Hz 고정 스텝 자기 예측(서버 tick 과 비트 일치).
		LARGE_INTEGER nowCounter;
		::QueryPerformanceCounter(&nowCounter);
		double elapsedSec = static_cast<double>(nowCounter.QuadPart - lastCounter.QuadPart) / static_cast<double>(perfFreq.QuadPart);
		lastCounter = nowCounter;
		if (elapsedSec > 0.25)
			elapsedSec = 0.25;   // 긴 정지(창 드래그 등) 후 폭주 방지
		simAccumulator += elapsedSec;
		while (simAccumulator >= kTickSeconds)
		{
			localPlayer.Update(kSimDt);
			gameState.AdvanceDeadReckon(kSimDt, localPlayer.GetMapId());   // 타인/몬스터도 같은 스텝으로 외삽 (원격 목록 = 자기 맵뿐)
			simAccumulator -= kTickSeconds;
		}

		// CS_MOVE 송신: 의도 변경 시 즉시 + 이동 중 167ms 주기(서버 event-driven broadcast 트리거).
		moveSendTimer += elapsedSec;
		if (screen == ClientScreen::InGame)
		{
			const bool intentChanged = (localPlayer.GetDir() != prevDir) || (localPlayer.GetMoveState() != prevState);
			const bool periodic = (localPlayer.GetMoveState() == EMoveState::START) && (moveSendTimer >= 0.167);
			if (intentChanged || periodic)
			{
				KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
				out.Begin(static_cast<USHORT>(PacketType::CS_MOVE));
				CS_MOVE req{};
				req.moveState = localPlayer.GetMoveState();
				req.direction = localPlayer.GetDir();
				req.x = localPlayer.GetX();
				req.y = localPlayer.GetY();
				req.clientSeq = localPlayer.NextSeq();
				req.Serialize(out);
				const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
				if (built) { clientSocket.Send(out); }
				moveSendTimer = 0.0;
			}
			prevDir = localPlayer.GetDir();
			prevState = localPlayer.GetMoveState();
		}

		// keepalive: 일정 주기마다 CS_PING 송신 - 가만히 있어도 연결 유지(서버는 일정시간 무수신이면 좀비로 보고 정리).
		//   IsPlaced()(=SC_CHAR_INFO 수신, InGame) 게이트로 arm(SC_ENTER_WORLD) 이후에만 송신 - CS_PING 은 게임세션 keepalive 라
		//   arm 전엔 의미가 없고, arm-창(admission ack 대기)에 평문 CS_PING 이 나가면 admission 완료된 서버가 이를 descramble 해
		//   recv 키만 굴러 C2S 영구 desync 가 된다. DummyClient 의 m_placed 게이트와 대칭.
		pingSendTimer += elapsedSec;
		if (localPlayer.IsPlaced() && clientSocket.IsConnected() && pingSendTimer >= (KEEPALIVE_INTERVAL_MS / 1000.0))   // 서버 NO_RECV_TIMEOUT(game 35s)과 결합된 keepalive 주기(ms->s)
		{
			KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
			out.Begin(static_cast<USHORT>(PacketType::CS_PING));
			CS_PING req{};
			req.clientTimeMs = static_cast<UINT32>(::GetTickCount64());   // 왕복시간(RTT) 측정용 - SC_PONG 이 그대로 되돌림
			req.Serialize(out);
			const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
			if (built) { clientSocket.Send(out); }
			pingSendTimer = 0.0;
		}

		// 스킬: Space 누른 순간 CS_SKILL 송신 - 서버가 사거리 내 몬스터 타격(skillId 0=평타). 연타 도배 방지 쿨다운.
		skillCooldown -= elapsedSec;
		if (screen == ClientScreen::InGame && !io.WantCaptureKeyboard
			&& ImGui::IsKeyPressed(ImGuiKey_Space, false) && skillCooldown <= 0.0 && !localPlayer.IsDead())
		{
			KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
			out.Begin(static_cast<USHORT>(PacketType::CS_SKILL));
			CS_SKILL req{};
			req.skillId = 0;
			req.Serialize(out);
			const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
			if (built) { clientSocket.Send(out); }
			localPlayer.TriggerAttackFlash();   // 논타겟팅: 스윙 순간 내 공격범위 표시(히트 무관). SC_DAMAGE는 히트 시만 와서 헛스윙이 안 보이던 것 보완.
			skillCooldown = 0.3;
		}

		// 줍기: Z 누른 순간 PICKUP_RANGE 안 가장 가까운 바닥 드랍의 objectId 로 CS_ITEM_PICKUP. 소유권/거리 최종 판정은 서버.
		pickupCooldown -= elapsedSec;
		if (screen == ClientScreen::InGame && !io.WantCaptureKeyboard
			&& ImGui::IsKeyPressed(ImGuiKey_Z, false) && pickupCooldown <= 0.0 && localPlayer.IsPlaced() && !localPlayer.IsDead())
		{
			const std::unordered_map<unsigned int, GroundItemView>& items = gameState.GroundItems();
			unsigned int bestId = 0;
			long long bestDistSq = static_cast<long long>(PICKUP_RANGE) * PICKUP_RANGE;   // 범위 밖은 후보 아님
			for (std::unordered_map<unsigned int, GroundItemView>::const_iterator it = items.begin(); it != items.end(); ++it)
			{
				const long long dx = static_cast<long long>(localPlayer.GetX()) - it->second.x;
				const long long dy = static_cast<long long>(localPlayer.GetY()) - it->second.y;
				const long long distSq = dx * dx + dy * dy;
				if (distSq <= bestDistSq) { bestDistSq = distSq; bestId = it->second.objectId; }   // 가장 가까운 것
			}
			if (bestId != 0)
			{
				KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
				out.Begin(static_cast<USHORT>(PacketType::CS_ITEM_PICKUP));
				CS_ITEM_PICKUP req{};
				req.objectId = bestId;
				req.Serialize(out);
				const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
				if (built) { clientSocket.Send(out); }
				pickupCooldown = 0.25;
			}
		}

		// 포탈 자동 진입: 현재 맵 포탈 반경 안에 들어가면 CS_PORTAL 송신(쿨다운으로 spam/경계거부 재시도 관리).
		if (portalCooldown > 0.0)
			portalCooldown -= elapsedSec;
		if (screen == ClientScreen::InGame)
		{
			const int portalId = FindPortalNear(localPlayer.GetMapId(), localPlayer.GetX(), localPlayer.GetY());
			if (portalId >= 0 && portalCooldown <= 0.0 && !localPlayer.IsDead())
			{
				KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
				out.Begin(static_cast<USHORT>(PacketType::CS_PORTAL));
				CS_PORTAL req{};
				req.portalId = portalId;
				req.Serialize(out);
				const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
				if (built) { clientSocket.Send(out); }
				portalCooldown = 0.4;   // 텔레포트 도착 전 중복/경계 거부 재시도 관리(과송신 방지)
			}
		}

		// 게임 월드 렌더 (배경 DrawList - 자기중심 격자 + 내 아바타)
		if (screen == ClientScreen::InGame)
			gameRenderer.Draw(localPlayer, gameState);

		// 채널 선택 전용 전체 화면 (로그인 후~배치 전). 채널 고르면 SC_CHAR_INFO(hp 포함 자기 상태)로 배치되어 맵이 렌더된다.
		if (screen == ClientScreen::ChannelSelect)
		{
			const ImVec2 csDisp = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(csDisp);
			ImGui::Begin(u8"채널 선택", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
			ImGui::Dummy(ImVec2(0.0f, csDisp.y * 0.22f));
			ImGui::SetWindowFontScale(1.7f);
			ImGui::TextUnformatted(u8"    접속할 채널을 선택하세요");
			ImGui::SetWindowFontScale(1.0f);
			ImGui::Dummy(ImVec2(0.0f, 24.0f));
			const SC_CHANNEL_LIST& csList = packetProcessor.GetChannelList();
			for (int i = 0; i < static_cast<int>(csList.channelCount); ++i)
			{
				char csBtn[96];
				::sprintf_s(csBtn, u8"    채널 %d          인구 %d / %d    ##fch%d", csList.entries[i].channelId, csList.entries[i].playerCount, csList.entries[i].maxPlayers, i);
				if (ImGui::Button(csBtn, ImVec2(380.0f, 56.0f)))
				{
					KYS::GAMECOMMON::PROTOCOL::CPacket csOut(MAX_PACKET_SIZE);
					csOut.Begin(static_cast<USHORT>(PacketType::CS_CHANNEL_SELECT));
					CS_CHANNEL_SELECT csReq{};
					csReq.channelId = csList.entries[i].channelId;
					csReq.Serialize(csOut);
					const bool built = csOut.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", csOut.GetType(), csOut.GetSize()); }
					if (built) { clientSocket.Send(csOut); }
				}
				ImGui::Dummy(ImVec2(0.0f, 8.0f));
			}
			ImGui::End();
		}

		// 캐릭터 선택 전용 전체 화면 (채널 선택 후~입장 전). 슬롯별로 캐릭터(입장/삭제) 또는 빈 슬롯(생성)을 보여준다.
		if (screen == ClientScreen::CharSelect)
		{
			const ImVec2 chDisp = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(chDisp);
			ImGui::Begin(u8"캐릭터 선택", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
			ImGui::Dummy(ImVec2(0.0f, chDisp.y * 0.16f));
			ImGui::SetWindowFontScale(1.7f);
			ImGui::TextUnformatted(u8"    캐릭터를 선택하세요");
			ImGui::SetWindowFontScale(1.0f);
			ImGui::Dummy(ImVec2(0.0f, 24.0f));
			const SC_CHARACTER_LIST& chList = packetProcessor.GetCharacterList();
			// admission(입장) 응답 대기 중이면 선택/삭제/생성 버튼을 비활성 - pending 창에 stray C2S 가 나가지 않게(중복 클릭 봉인).
			ImGui::BeginDisabled(admissionPending);
			for (int slot = 0; slot < MAX_CHAR_SLOTS; ++slot)
			{
				// 이 슬롯을 쓰는 캐릭터 찾기 (목록은 슬롯 순서를 보장하지 않음).
				const CharSummary* found = nullptr;
				for (int i = 0; i < static_cast<int>(chList.count) && i < MAX_CHAR_SLOTS; ++i)
				{
					if (chList.chars[i].slotId == static_cast<BYTE>(slot))
					{
						found = &chList.chars[i];
						break;
					}
				}
				if (found != nullptr)
				{
					char nameUtf8[64];
					ToUtf8(found->name, nameUtf8, sizeof(nameUtf8));
					char enterBtn[192];
					::sprintf_s(enterBtn, u8"    [%d] %s     맵 %d   HP %d/%d    ##enter%d", slot, nameUtf8, found->mapId, found->hp, found->maxHp, slot);
					if (ImGui::Button(enterBtn, ImVec2(480.0f, 52.0f)))
					{
						// 캐릭터 선택 = admission(이때 서버가 GameSession 생성 -> SC_CHAR_INFO 로 배치).
						KYS::GAMECOMMON::PROTOCOL::CPacket chOut(MAX_PACKET_SIZE);
						chOut.Begin(static_cast<USHORT>(PacketType::CS_CHARACTER_SELECT));
						CS_CHARACTER_SELECT sel{};
						sel.charId = found->charId;
						sel.Serialize(chOut);
						const bool built = chOut.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", chOut.GetType(), chOut.GetSize()); }
						if (built) { clientSocket.Send(chOut); }            // CS_CHARACTER_SELECT 는 평문 송신 - 무장(arm)은 서버 SC_ENTER_WORLD(admission 성공) 수신 시로 미룬다
						admissionPending = true;             // admission 응답 전까지 캐릭터 관리 재송신 차단 (pending 창 stray C2S 봉인)
						admissionListVersion = packetProcessor.GetCharacterListVersion();   // 현재 목록 버전 스냅샷 - 서버가 재푸시(거부)하면 이 값이 바뀌어 pending 해제
						localPlayer.SetName(found->name);   // 내 이름표 = 선택한 캐릭터 이름 (로그인 때 넣은 계정 id 를 대체)
					}
					ImGui::SameLine();
					char delBtn[48];
					::sprintf_s(delBtn, u8"삭제##del%d", slot);
					if (ImGui::Button(delBtn, ImVec2(90.0f, 52.0f)))
					{
						// 삭제 = 즉시 hard delete. OK 면 서버가 갱신된 목록을 재푸시해 슬롯이 비워진다.
						KYS::GAMECOMMON::PROTOCOL::CPacket chOut(MAX_PACKET_SIZE);
						chOut.Begin(static_cast<USHORT>(PacketType::CS_CHARACTER_DELETE));
						CS_CHARACTER_DELETE del{};
						del.charId = found->charId;
						del.Serialize(chOut);
						const bool built = chOut.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", chOut.GetType(), chOut.GetSize()); }
						if (built) { clientSocket.Send(chOut); }
						charStatusMsg[0] = '\0';
					}
				}
				else
				{
					char makeBtn[80];
					::sprintf_s(makeBtn, u8"    [%d] 빈 슬롯 - 캐릭터 생성    ##make%d", slot, slot);
					if (ImGui::Button(makeBtn, ImVec2(480.0f, 52.0f)))
					{
						charCreateOpen = true;
						charCreateSlot = static_cast<BYTE>(slot);
						charNameInput[0] = '\0';
						charStatusMsg[0] = '\0';
					}
				}
				ImGui::Dummy(ImVec2(0.0f, 8.0f));
			}
			ImGui::EndDisabled();
			if (admissionPending)
			{
				ImGui::Dummy(ImVec2(0.0f, 8.0f));
				ImGui::TextUnformatted(u8"    입장 처리 중...");
			}
			if (charStatusMsg[0] != '\0')
			{
				ImGui::Dummy(ImVec2(0.0f, 8.0f));
				ImGui::TextWrapped("    %s", charStatusMsg);
			}
			ImGui::End();
		}

		// 캐릭터 생성 전용 전체 화면 (빈 슬롯에서 진입). 이름 입력 -> CS_CHARACTER_CREATE (서버가 길이/중복/슬롯 검증).
		if (screen == ClientScreen::CharCreate)
		{
			const ImVec2 ccDisp = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ccDisp);
			ImGui::Begin(u8"캐릭터 생성", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
			ImGui::Dummy(ImVec2(0.0f, ccDisp.y * 0.22f));
			ImGui::SetWindowFontScale(1.7f);
			ImGui::Text(u8"    슬롯 %d - 새 캐릭터", static_cast<int>(charCreateSlot));
			ImGui::SetWindowFontScale(1.0f);
			ImGui::Dummy(ImVec2(0.0f, 24.0f));
			ImGui::TextUnformatted(u8"    캐릭터 이름 (1~15자)");
			ImGui::SetNextItemWidth(360.0f);
			ImGui::InputText(u8"##charname", charNameInput, sizeof(charNameInput));
			ImGui::Dummy(ImVec2(0.0f, 12.0f));
			// admission(입장) 응답 대기 중이면 생성도 비활성 - pending 창 stray C2S 봉인(선택과 대칭).
			ImGui::BeginDisabled(admissionPending);
			if (ImGui::Button(u8"    생성    ", ImVec2(180.0f, 48.0f)))
			{
				CS_CHARACTER_CREATE req{};
				ToWide(charNameInput, req.name, WHISPER_NAME_MAX);   // UTF-8 입력 -> wchar (WHISPER_NAME_MAX 로 절단/널종료)
				if (req.name[0] == L'\0')
					::strcpy_s(charStatusMsg, u8"이름을 입력하세요.");
				else
				{
					KYS::GAMECOMMON::PROTOCOL::CPacket ccOut(MAX_PACKET_SIZE);
					ccOut.Begin(static_cast<USHORT>(PacketType::CS_CHARACTER_CREATE));
					req.slotId = charCreateSlot;
					req.Serialize(ccOut);
					const bool built = ccOut.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", ccOut.GetType(), ccOut.GetSize()); }
					if (built) { clientSocket.Send(ccOut); }
				}
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button(u8"    취소    ", ImVec2(180.0f, 48.0f)))
			{
				charCreateOpen = false;   // 생성 취소 -> 선택 화면 복귀
				charStatusMsg[0] = '\0';
			}
			if (charStatusMsg[0] != '\0')
			{
				ImGui::Dummy(ImVec2(0.0f, 12.0f));
				ImGui::TextWrapped("    %s", charStatusMsg);
			}
			ImGui::End();
		}

			// 인게임 채널 변경 picker 오버레이 (InGame + "채널 변경" 버튼 눌림). 캐시된 채널 목록에서 대상 선택 -> CS_CHANNEL_CHANGE.
			//   현재 채널은 클라가 추적 안 함 - 같은 채널 선택 시 서버가 SAME 으로 회신(클라 상태 불필요).
			if (screen == ClientScreen::InGame && channelChangeOpen)
			{
				const ImVec2 pkDisp = ImGui::GetIO().DisplaySize;
				ImGui::SetNextWindowPos(ImVec2(pkDisp.x * 0.5f, pkDisp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
				ImGui::Begin(u8"채널 변경", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
				ImGui::TextUnformatted(u8"이동할 채널을 선택하세요 (같은 캐릭터로 이동).");
				ImGui::Dummy(ImVec2(0.0f, 6.0f));
				const SC_CHANNEL_LIST& ccList = packetProcessor.GetChannelList();   // picker 열 때 CS_CHANNEL_LIST_REQUEST 로 갱신 - 응답 전 첫 프레임만 직전 캐시값
				if (channelChangePending) { ImGui::TextUnformatted(u8"채널 이동 중..."); }
				ImGui::BeginDisabled(channelChangePending);   // 응답 대기 중엔 재선택 차단
				for (int i = 0; i < static_cast<int>(ccList.channelCount); ++i)
				{
					char ccBtn[96];
					::sprintf_s(ccBtn, u8"채널 %d   (인구 %d)##cc%d", ccList.entries[i].channelId, ccList.entries[i].playerCount, i);
					if (ImGui::Button(ccBtn, ImVec2(240.0f, 40.0f)))
					{
						KYS::GAMECOMMON::PROTOCOL::CPacket ccOut(MAX_PACKET_SIZE);
						ccOut.Begin(static_cast<USHORT>(PacketType::CS_CHANNEL_CHANGE));
						CS_CHANNEL_CHANGE ccReq{};
						ccReq.channelId = static_cast<BYTE>(ccList.entries[i].channelId);
						ccReq.Serialize(ccOut);
						const bool built = ccOut.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", ccOut.GetType(), ccOut.GetSize()); }
						if (built) { clientSocket.Send(ccOut); }
						channelChangeMsg[0] = '\0';   // 응답 대기 - 이전 피드백 지움
						channelChangePending = true;   // 결과 도착까지 재선택 차단
					}
				}
				ImGui::EndDisabled();
				ImGui::Dummy(ImVec2(0.0f, 6.0f));
				if (ImGui::Button(u8"취소", ImVec2(240.0f, 32.0f)))
					{
						channelChangeOpen = false;
						channelChangePending = false;   // 취소 시에도 pending 해제 (재시도 가능)
					}
				ImGui::End();
			}

			// 사망 화면: 입력은 위에서 차단됨. 중앙에 사망 팝업 + 자동 부활 카운트다운.
			//   부활은 서버 권위(PLAYER_RESPAWN_DELAY_SEC 후 SC_RESPAWN) - 클라는 대기만, 도착하면 hp 복원돼 팝업 자동 해제.
			if (screen == ClientScreen::InGame && localPlayer.IsDead())
			{
				deathElapsed += elapsedSec;
				const ImVec2 dispSize = ImGui::GetIO().DisplaySize;
				ImGui::SetNextWindowPos(ImVec2(dispSize.x * 0.5f, dispSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
				ImGui::Begin(u8"사망", nullptr,
					ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
				ImGui::TextUnformatted(u8"사망했습니다.");
				float remain = static_cast<float>(PLAYER_RESPAWN_DELAY_SEC) - static_cast<float>(deathElapsed);
				if (remain > 0.0f)
					ImGui::Text(u8"%.1f초 후 자동 부활합니다...", remain);
				else
					ImGui::TextUnformatted(u8"부활 중...");
				ImGui::End();
			}
			else
			{
				deathElapsed = 0.0;
			}

		// (4-5) 컨트롤 패널 - 2단계 로그인 + 네트워크 통계 + 내 배치
		ImGui::Begin(u8"KYS 게임 클라이언트");
		if (!clientSocket.IsConnected())
		{
			const bool creating = accountCreateOpen;   // 연결 전 화면: false=로그인 / true=계정 생성 (둘 다 같은 입력칸, 버튼만 다름)
			ImGui::TextUnformatted(creating
				? u8"새 계정을 만듭니다 (id + 비밀번호)."
				: u8"게임 서버에 2단계로 접속합니다 (로그인 서버 -> 토큰 -> 게임 서버).");
			ImGui::InputText(u8"서버 IP", inputServerIp, sizeof(inputServerIp));
			ImGui::InputText(u8"계정 ID", inputId, sizeof(inputId));
			ImGui::InputText(u8"비밀번호", inputPw, sizeof(inputPw), ImGuiInputTextFlags_Password);
			if (!creating)
			{
				if (ImGui::Button(u8"접속 (2단계 로그인)"))
				{
					wchar_t wip[64];
					wchar_t wid[32];
					wchar_t wpw[64];
					ToWide(inputServerIp, wip, 64);
					ToWide(inputId, wid, 32);
					ToWide(inputPw, wpw, 64);
					// (1) 토큰 획득 -> (2) 게임 서버 접속. 게임 서버 주소는 로그인 서버가 SC_LOGIN_TOKEN 에 실어
					//   반드시 하달한다 - 클라가 입력 IP 로 대신 붙는 폴백은 없다(rip == 0 이면 서버 오설정).
					bool gameAddressMissing = false;   // 토큰은 받았는데 게임 서버 주소가 비어 있었다 (아래 실패 메시지 분기용)
					bool ok = clientSocket.AcquireToken(wip, LOGIN_CLIENT_PORT, wid, wpw);
					if (ok)
					{
						const UINT32 rip = clientSocket.GetRedirectIp();
						if (rip == 0)
						{
							gameAddressMissing = true;
							ok = false;   // 붙을 주소가 없다 - 여기서 실패로 확정하지 않으면 ConnectGame 없이 "접속 성공"이 뜬다
						}
						else
						{
							wchar_t ripStr[32];
							::swprintf_s(ripStr, L"%u.%u.%u.%u", (rip >> 24) & 0xFF, (rip >> 16) & 0xFF, (rip >> 8) & 0xFF, rip & 0xFF);
							ok = clientSocket.ConnectGame(ripStr, clientSocket.GetRedirectPort());
						}
					}
					if (ok)
						::strcpy_s(statusMsg, u8"접속 성공 - 게임 스트림 수신 중");
					else if (gameAddressMissing)
						::strcpy_s(statusMsg, u8"접속 실패 - 서버가 게임 서버 주소를 주지 않음 (서버 오설정)");
					else
						::strcpy_s(statusMsg, u8"접속 실패 (id/비밀번호/서버 실행 확인)");
					if (ok)
					{
						localPlayer.SetName(wid);   // 임시 이름표 = 로그인 id (캐릭터 선택 시 캐릭터 이름으로 대체)
						admissionPending = false;   // 새 연결 = admission pending 초기화 (직전 세션 잔여 봉인)
					}
					else
						clientSocket.Close();
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"계정 만들기"))
				{
					accountCreateOpen = true;
					statusMsg[0] = '\0';
				}
			}
			else
			{
				if (ImGui::Button(u8"만들기"))
				{
					wchar_t wip[64];
					wchar_t wid[32];
					wchar_t wpw[64];
					ToWide(inputServerIp, wip, 64);
					ToWide(inputId, wid, 32);
					ToWide(inputPw, wpw, 64);
					BYTE result = 0;
					const bool reached = clientSocket.RegisterAccount(wip, LOGIN_CLIENT_PORT, wid, wpw, result);
					if (!reached)
						::strcpy_s(statusMsg, u8"계정 생성 실패 (로그인 서버 응답 없음).");
					else if (result == static_cast<BYTE>(ECreateAccountResult::OK))
					{
						::strcpy_s(statusMsg, u8"계정이 생성되었습니다. 로그인하세요.");
						accountCreateOpen = false;   // 성공 -> 로그인 화면으로 복귀
					}
					else if (result == static_cast<BYTE>(ECreateAccountResult::DUP_ID))
						::strcpy_s(statusMsg, u8"이미 있는 계정 ID 입니다.");
					else if (result == static_cast<BYTE>(ECreateAccountResult::INVALID))
						::strcpy_s(statusMsg, u8"입력이 올바르지 않습니다 (id 1~15자 / 비번 1~31자).");
					else
						::strcpy_s(statusMsg, u8"계정 생성 실패 (서버 오류).");
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"뒤로"))
				{
					accountCreateOpen = false;
					statusMsg[0] = '\0';
				}
			}
			if (statusMsg[0] != '\0')
				ImGui::TextWrapped("%s", statusMsg);
		}
		else
		{
			ImGui::TextUnformatted(u8"게임 서버 접속됨 (2단계 핸드셰이크 완료).");
			if (localPlayer.IsPlaced())
			{
				ImGui::Text(u8"내 ID  : %u", localPlayer.GetId());
				ImGui::Text(u8"내 위치: map=%d  (%d, %d)", localPlayer.GetMapId(), localPlayer.GetX(), localPlayer.GetY());
					ImGui::Text(u8"내 HP   : %d / %d", localPlayer.GetHp(), localPlayer.GetMaxHp());
					if (ImGui::Button(u8"채널 변경"))
						{
							channelChangeOpen = true;   // 인게임 채널 picker 오버레이 열기
							channelChangePending = false;   // 새로 열 때 pending 리셋 (직전 요청 응답 유실로 영구 잠김 방지)
							// 최신 채널 인구 재요청 - 로그인 때 캐시된 목록은 stale(입장 후 변동 미반영). 응답 SC_CHANNEL_LIST 가 캐시 갱신(picker 가 다음 프레임 재렌더).
							KYS::GAMECOMMON::PROTOCOL::CPacket clOut(MAX_PACKET_SIZE);
							clOut.Begin(static_cast<USHORT>(PacketType::CS_CHANNEL_LIST_REQUEST));   // payload 없음
							const bool built = clOut.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", clOut.GetType(), clOut.GetSize()); }
							if (built) { clientSocket.Send(clOut); }
						}
					ImGui::SameLine();
					if (ImGui::Button(u8"소지품 (I)"))
						inventoryOpen = !inventoryOpen;
					ImGui::TextUnformatted(u8"Z=줍기  I=소지품  Space=공격  '/trade 이름'=거래");
					if (channelChangeMsg[0] != '\0')
						ImGui::TextWrapped("%s", channelChangeMsg);
			}
			else if (packetProcessor.HasCharacterList())
			{
				ImGui::TextUnformatted(charCreateOpen ? u8"캐릭터 생성 화면 표시 중..." : u8"캐릭터 선택 화면 표시 중...");
			}
			else if (packetProcessor.HasChannelList())
			{
				ImGui::TextUnformatted(u8"채널 선택 화면 표시 중...");
			}
			else
			{
				ImGui::TextUnformatted(u8"채널 목록 대기 중...");
			}
			ImGui::Separator();
			ImGui::Text(u8"토큰       : 0x%016llX", clientSocket.GetToken());
			ImGui::Text(u8"수신 패킷 수 : %lld", clientSocket.GetRecvPackets());
			ImGui::Text(u8"수신 바이트  : %lld", clientSocket.GetRecvBytes());
			ImGui::Text(u8"마지막 종류  : 0x%04X", static_cast<unsigned int>(clientSocket.GetLastPacketType()));
			if (ImGui::Button(u8"연결 종료"))
			{
				clientSocket.Close();
				localPlayer.Reset();
				gameState.Reset();
				packetProcessor.ClearChannelList();
				packetProcessor.ClearCharacterList();   // 캐릭터 선택 단계에서 종료해도 화면 분기가 남지 않게
				packetProcessor.ClearInGameState();     // 옛 세션 인벤/거래 창이 재접속 후 남지 않게(종료 통지는 끊긴 연결로 못 옴)
				packetProcessor.ResetRtt();   // 죽은 연결의 RTT가 끊김/재접속 후 표시되지 않게 초기화 (-1=측정 중)
				inventoryOpen = false;
				selectedBagSlot = -1;
				itemStatusMsg[0] = '\0';
				charCreateOpen = false;       // 캐릭터 폼/피드백 초기화
				channelChangeOpen = false;    // 채널 picker/피드백도 초기화
				channelChangeMsg[0] = '\0';
				channelChangePending = false;
				charStatusMsg[0] = '\0';
				admissionPending = false;   // 종료 = admission pending 해제 (재접속 후 캐릭터 버튼이 잠긴 채 남지 않게)
				::strcpy_s(statusMsg, u8"연결을 종료했습니다.");
			}
		}
		ImGui::Separator();
		const int rttMs = packetProcessor.GetLastRttMs();
		if (rttMs < 0)
			ImGui::Text(u8"RTT: 측정 중   FPS: %.1f", static_cast<double>(io.Framerate));
		else
			ImGui::Text(u8"RTT: %d ms   FPS: %.1f", rttMs, static_cast<double>(io.Framerate));
#ifdef _DEBUG
		// H6 (검증 하니스): 인위 RTT lag 이 켜져 있으면 러버밴드 카운트를 표시(버그면 계속 증가, 수정 후 ~정지).
		if (packetProcessor.GetRttSimLagMs() > 0)
			ImGui::Text(u8"[H6] RTT sim %d ms   러버밴드 %lld회",
				packetProcessor.GetRttSimLagMs(),
				static_cast<long long>(packetProcessor.GetRttRubberBandCount()));
#endif
		ImGui::End();

		// (4-5) 채팅: 로그 + 입력(Enter 송신). "/w 이름 메시지" = 귓속말, 아니면 일반 채팅.
		if (screen == ClientScreen::InGame)
		{
			chatCooldown -= elapsedSec;
			ImGui::Begin(u8"채팅");
			const std::deque<ChatLine>& chatLog = gameState.ChatLog();
			ImGui::BeginChild("chatlog", ImVec2(0.0f, 180.0f), true);
			for (std::deque<ChatLine>::const_iterator it = chatLog.begin(); it != chatLog.end(); ++it)
			{
				char s[CHAT_SENDER_MAX * 3];   // UTF-8 한글 3바이트/자 -> 원본 wchar x3
				char m[240];
				ToUtf8(it->sender, s, sizeof(s));
				ToUtf8(it->message, m, sizeof(m));
				if (it->isWhisper)
					ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), u8"[귓속말] %s: %s", s, m);
				else
					ImGui::Text("%s: %s", s, m);
			}
			ImGui::EndChild();
			if (ImGui::InputText(u8"입력(Enter)", chatInput, sizeof(chatInput), ImGuiInputTextFlags_EnterReturnsTrue)
				&& chatInput[0] != '\0' && chatCooldown <= 0.0)
			{
				if (chatInput[0] == '/' && chatInput[1] == 'w' && chatInput[2] == ' ')
				{
					char* rest = chatInput + 3;
					char* sp = ::strchr(rest, ' ');
					if (sp != nullptr)
					{
						*sp = '\0';
						wchar_t wname[WHISPER_NAME_MAX];
						wchar_t wmsg[CHAT_MSG_MAX];
						ToWide(rest, wname, WHISPER_NAME_MAX);
						ToWide(sp + 1, wmsg, CHAT_MSG_MAX);
						KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
						out.Begin(static_cast<USHORT>(PacketType::CS_WHISPER));
						CS_WHISPER req{};
						::wcscpy_s(req.targetName, WHISPER_NAME_MAX, wname);
						::wcscpy_s(req.message, CHAT_MSG_MAX, wmsg);
						req.Serialize(out);
						const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
						if (built) { clientSocket.Send(out); }
						// 발신자 자기 채팅창에도 보낸 귓속말 표시(서버는 대상에게만 보냄)
						wchar_t selfTag[CHAT_SENDER_MAX];
						::swprintf_s(selfTag, CHAT_SENDER_MAX, L">%s", wname);
						gameState.AddChat(selfTag, wmsg, true);
					}
				}
				else if (::strncmp(chatInput, "/trade ", 7) == 0 && chatInput[7] != '\0')
				{
					// "/trade 이름" - 같은 맵 상대에게 거래 요청. 성립 검증(같은 맵/BUSY/NOT_FOUND)은 서버.
					wchar_t wname[WHISPER_NAME_MAX];
					ToWide(chatInput + 7, wname, WHISPER_NAME_MAX);
					KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
					out.Begin(static_cast<USHORT>(PacketType::CS_TRADE_REQUEST));
					CS_TRADE_REQUEST req{};
					::wcscpy_s(req.targetName, WHISPER_NAME_MAX, wname);
					req.Serialize(out);
					const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
					if (built) { clientSocket.Send(out); }
					::strcpy_s(itemStatusMsg, u8"거래 요청을 보냈습니다 (상대 수락 대기).");
				}
				else
				{
					wchar_t wmsg[CHAT_MSG_MAX];
					ToWide(chatInput, wmsg, CHAT_MSG_MAX);
					KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
					out.Begin(static_cast<USHORT>(PacketType::CS_CHAT));
					CS_CHAT req{};
					::wcscpy_s(req.message, CHAT_MSG_MAX, wmsg);
					req.Serialize(out);
					const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
					if (built) { clientSocket.Send(out); }
				}
				chatCooldown = 0.5;
				chatInput[0] = '\0';
				ImGui::SetKeyboardFocusHere(-1);   // 전송 후 입력창 포커스 유지
			}
			ImGui::End();
		}

		// (4-6) 아이템 / 거래 UI (InGame 전용) - 인벤 창(I 토글) + 거래 요청 다이얼로그 + 거래 창.
		if (screen == ClientScreen::InGame)
		{
			// 결과 토스트 소비 (아이템 조작 실패 / 거래 종결) - one-shot.
			BYTE itemRes = 0;
			if (packetProcessor.TakeItemResult(itemRes))
			{
				switch (static_cast<EItemResult>(itemRes))
				{
				case EItemResult::INV_FULL:        ::strcpy_s(itemStatusMsg, u8"인벤토리가 가득 찼습니다."); break;
				case EItemResult::BAD_SLOT:        ::strcpy_s(itemStatusMsg, u8"잘못된 슬롯입니다."); break;
				case EItemResult::TOO_FAR:         ::strcpy_s(itemStatusMsg, u8"너무 멀어 주울 수 없습니다."); break;
				case EItemResult::NOT_OWNER:       ::strcpy_s(itemStatusMsg, u8"아직 주울 수 없습니다 (소유권 보호 중)."); break;
				case EItemResult::NOT_FOUND:       ::strcpy_s(itemStatusMsg, u8"아이템이 이미 사라졌습니다."); break;
				case EItemResult::WRONG_TYPE:      ::strcpy_s(itemStatusMsg, u8"장착/사용할 수 없는 종류입니다."); break;
				case EItemResult::LOCKED_IN_TRADE: ::strcpy_s(itemStatusMsg, u8"거래 중엔 인벤을 조작할 수 없습니다."); break;
				default: break;
				}
			}
			BYTE tradeRes = 0;
			if (packetProcessor.TakeTradeResult(tradeRes))
			{
				switch (static_cast<ETradeResult>(tradeRes))
				{
				case ETradeResult::COMPLETE:     ::strcpy_s(itemStatusMsg, u8"거래가 완료되었습니다."); break;
				case ETradeResult::CANCELLED:    ::strcpy_s(itemStatusMsg, u8"거래가 취소되었습니다."); break;
				case ETradeResult::PARTNER_LEFT: ::strcpy_s(itemStatusMsg, u8"상대가 거래를 떠났습니다."); break;
				case ETradeResult::INV_FULL:     ::strcpy_s(itemStatusMsg, u8"인벤 공간이 부족합니다 (오퍼 재조정 후 재수락)."); break;
				case ETradeResult::TOO_FAR:      ::strcpy_s(itemStatusMsg, u8"같은 맵이 아니라 거래할 수 없습니다."); break;
				case ETradeResult::BUSY:         ::strcpy_s(itemStatusMsg, u8"상대가 다른 거래 중입니다."); break;
				case ETradeResult::NOT_FOUND:    ::strcpy_s(itemStatusMsg, u8"그 이름의 상대가 이 맵에 없습니다."); break;
				default: break;
				}
			}

			// 인벤 창 토글 (I). 채팅 입력 중(WantCaptureKeyboard)엔 무시.
			if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_I, false))
				inventoryOpen = !inventoryOpen;

			// 들어온 거래 요청 다이얼로그 (수락/거절).
			if (packetProcessor.HasTradeRequest())
			{
				ImGui::Begin(u8"거래 요청", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
				char rn[WHISPER_NAME_MAX * 3];
				ToUtf8(packetProcessor.GetTradeRequesterName(), rn, sizeof(rn));
				ImGui::Text(u8"%s 님이 거래를 요청했습니다.", rn);
				const bool accept = ImGui::Button(u8"수락");
				ImGui::SameLine();
				const bool decline = ImGui::Button(u8"거절");
				if (accept || decline)
				{
					KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
					out.Begin(static_cast<USHORT>(PacketType::CS_TRADE_RESPONSE));
					CS_TRADE_RESPONSE req{};
					req.accept = accept ? 1 : 0;
					req.Serialize(out);
					const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
					if (built) { clientSocket.Send(out); }
					packetProcessor.ClearTradeRequest();   // 응답 보냈으니 다이얼로그 닫기(수락이면 곧 SC_TRADE_OPEN)
				}
				ImGui::End();
			}

			// 인벤 창 - 장착 2칸 + 가방 6열 그리드 + 선택 칸 액션(장착/사용/버림).
			if (inventoryOpen && packetProcessor.HasInventory())
			{
				const ClientInventory& inv = packetProcessor.GetInventory();
				ImGui::SetNextWindowSize(ImVec2(330.0f, 0.0f), ImGuiCond_Appearing);
				ImGui::Begin(u8"소지품 (I)", &inventoryOpen);
				ImGui::Text(u8"공격력 %d    방어력 %d", packetProcessor.GetAtkPower(), packetProcessor.GetDefPower());
				ImGui::Separator();

				// 장착 슬롯 (무기/방어구) + 해제 버튼.
				{
					char wname[ITEM_NAME_MAX * 3];
					if (inv.weapon.templateId != 0) { const ItemDef* d = FindItemDef(inv.weapon.templateId); ToUtf8(d ? d->name : L"?", wname, sizeof(wname)); }
					else ::strcpy_s(wname, u8"(없음)");
					ImGui::Text(u8"무기  : %s", wname);
					if (inv.weapon.templateId != 0)
					{
						ImGui::SameLine();
						if (ImGui::SmallButton(u8"해제##w"))
						{
							KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
							out.Begin(static_cast<USHORT>(PacketType::CS_ITEM_UNEQUIP));
							CS_ITEM_UNEQUIP req{}; req.equipSlot = EQUIP_SLOT_WEAPON; req.Serialize(out); const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
							if (built) { clientSocket.Send(out); }
						}
					}
					char aname[ITEM_NAME_MAX * 3];
					if (inv.armor.templateId != 0) { const ItemDef* d = FindItemDef(inv.armor.templateId); ToUtf8(d ? d->name : L"?", aname, sizeof(aname)); }
					else ::strcpy_s(aname, u8"(없음)");
					ImGui::Text(u8"방어구: %s", aname);
					if (inv.armor.templateId != 0)
					{
						ImGui::SameLine();
						if (ImGui::SmallButton(u8"해제##a"))
						{
							KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
							out.Begin(static_cast<USHORT>(PacketType::CS_ITEM_UNEQUIP));
							CS_ITEM_UNEQUIP req{}; req.equipSlot = EQUIP_SLOT_ARMOR; req.Serialize(out); const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
							if (built) { clientSocket.Send(out); }
						}
					}
				}
				ImGui::Separator();

				// 가방 6열 그리드 - 칸 클릭 = 선택. 빈 칸은 선택 해제.
				for (int slot = 0; slot < MAX_INVENTORY_SLOTS; ++slot)
				{
					if (slot % 6 != 0) ImGui::SameLine();
					const ClientItemSlot& s = inv.bag[slot];
					char btn[ITEM_NAME_MAX * 3 + 24];
					if (s.templateId != 0)
					{
						const ItemDef* d = FindItemDef(s.templateId);
						char nm[ITEM_NAME_MAX * 3];
						ToUtf8(d ? d->name : L"?", nm, sizeof(nm));
						::sprintf_s(btn, "%.6s\nx%d##b%d", nm, s.quantity, slot);   // 좁은 칸 - 이름 앞부분 + 수량
					}
					else
					{
						::sprintf_s(btn, "-##b%d", slot);
					}
					const bool selected = (selectedBagSlot == slot);
					if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
					if (ImGui::Button(btn, ImVec2(48.0f, 40.0f)))
						selectedBagSlot = (s.templateId != 0) ? slot : -1;
					if (selected) ImGui::PopStyleColor();

					// 드래그&드롭 - 아이템 있는 칸을 잡아 다른 칸에 놓으면 CS_ITEM_MOVE(스왑 또는 같은 종류 스택 합류). 서버 권위.
					if (s.templateId != 0 && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
					{
						ImGui::SetDragDropPayload("BAG_SLOT", &slot, sizeof(int));   // 출발 칸 번호를 payload 로 (ImGui 가 값 복사)
						ImGui::Text(u8"이동: 칸 %d", slot);                          // 드래그 중 미리보기
						ImGui::EndDragDropSource();
					}
					if (ImGui::BeginDragDropTarget())
					{
						const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("BAG_SLOT");
						if (payload != nullptr)
						{
							const int fromSlot = *static_cast<const int*>(payload->Data);
							if (fromSlot != slot)   // 같은 칸에 놓기 무시
							{
								KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
								out.Begin(static_cast<USHORT>(PacketType::CS_ITEM_MOVE));
								CS_ITEM_MOVE req{}; req.fromSlot = static_cast<BYTE>(fromSlot); req.toSlot = static_cast<BYTE>(slot); req.Serialize(out); const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
								if (built) { clientSocket.Send(out); }
							}
						}
						ImGui::EndDragDropTarget();
					}
				}

				// 선택 칸 액션.
				if (selectedBagSlot >= 0 && selectedBagSlot < MAX_INVENTORY_SLOTS && inv.bag[selectedBagSlot].templateId != 0)
				{
					const ClientItemSlot& s = inv.bag[selectedBagSlot];
					const ItemDef* d = FindItemDef(s.templateId);
					ImGui::Separator();
					char nm[ITEM_NAME_MAX * 3];
					ToUtf8(d ? d->name : L"?", nm, sizeof(nm));
					ImGui::Text(u8"선택: %s (칸 %d)", nm, selectedBagSlot);
					if (d != nullptr && (d->itemType == EItemType::EQUIP_WEAPON || d->itemType == EItemType::EQUIP_ARMOR))
					{
						if (ImGui::Button(u8"장착"))
						{
							KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
							out.Begin(static_cast<USHORT>(PacketType::CS_ITEM_EQUIP));
							CS_ITEM_EQUIP req{}; req.slot = static_cast<BYTE>(selectedBagSlot); req.Serialize(out); const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
							if (built) { clientSocket.Send(out); }
						}
						ImGui::SameLine();
					}
					if (d != nullptr && d->itemType == EItemType::CONSUME)
					{
						if (ImGui::Button(u8"사용"))
						{
							KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
							out.Begin(static_cast<USHORT>(PacketType::CS_ITEM_USE));
							CS_ITEM_USE req{}; req.slot = static_cast<BYTE>(selectedBagSlot); req.Serialize(out); const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
							if (built) { clientSocket.Send(out); }
						}
						ImGui::SameLine();
					}
					if (ImGui::Button(u8"1개 버림"))
					{
						KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
						out.Begin(static_cast<USHORT>(PacketType::CS_ITEM_DISCARD));
						CS_ITEM_DISCARD req{}; req.slot = static_cast<BYTE>(selectedBagSlot); req.quantity = 1; req.Serialize(out); const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
						if (built) { clientSocket.Send(out); }
					}
				}
				if (itemStatusMsg[0] != '\0')
				{
					ImGui::Separator();
					ImGui::TextWrapped("%s", itemStatusMsg);
				}
				ImGui::End();
			}

			// 거래 창 - 양측 오퍼 + 수락 상태 + 가방에서 올리기 + 수락/취소.
			if (packetProcessor.IsTradeOpen())
			{
				const SC_TRADE_UPDATE& t = packetProcessor.GetTradeState();
				const ClientInventory& inv = packetProcessor.GetInventory();
				char pn[WHISPER_NAME_MAX * 3];
				ToUtf8(packetProcessor.GetTradePartnerName(), pn, sizeof(pn));
				ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
				ImGui::Begin(u8"거래", nullptr, ImGuiWindowFlags_NoCollapse);
				ImGui::Text(u8"상대: %s", pn);
				ImGui::Text(u8"내 수락: %s     상대 수락: %s",
					t.myAccept ? u8"O" : u8"X", t.partnerAccept ? u8"O" : u8"X");
				ImGui::Separator();

				// 양측 오퍼 (좌=내 오퍼[내리기 가능], 우=상대 오퍼[읽기]).
				ImGui::Columns(2, "tradeoffers", true);
				ImGui::TextUnformatted(u8"내 오퍼");
				for (int i = 0; i < t.myCount && i < MAX_TRADE_SLOTS; ++i)
				{
					const ItemSlotEntry& e = t.mine[i];
					const ItemDef* d = FindItemDef(e.templateId);
					char nm[ITEM_NAME_MAX * 3];
					ToUtf8(d ? d->name : L"?", nm, sizeof(nm));
					ImGui::Text(u8"%s x%d", nm, e.quantity);
					ImGui::SameLine();
					char rm[24]; ::sprintf_s(rm, u8"내리기##t%d", e.slot);
					if (ImGui::SmallButton(rm))
					{
						KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
						out.Begin(static_cast<USHORT>(PacketType::CS_TRADE_REMOVE_ITEM));
						CS_TRADE_REMOVE_ITEM req{}; req.tradeSlot = e.slot; req.Serialize(out); const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
						if (built) { clientSocket.Send(out); }
					}
				}
				ImGui::NextColumn();
				ImGui::TextUnformatted(u8"상대 오퍼");
				for (int i = 0; i < t.partnerCount && i < MAX_TRADE_SLOTS; ++i)
				{
					const ItemSlotEntry& e = t.partner[i];
					const ItemDef* d = FindItemDef(e.templateId);
					char nm[ITEM_NAME_MAX * 3];
					ToUtf8(d ? d->name : L"?", nm, sizeof(nm));
					ImGui::Text(u8"%s x%d", nm, e.quantity);
				}
				ImGui::Columns(1);
				ImGui::Separator();

				// 가방에서 거래에 올리기 - 다음 빈 거래칸을 찾아 CS_TRADE_ADD_ITEM.
				ImGui::SetNextItemWidth(120.0f);
				if (ImGui::InputInt(u8"올릴 수량", &tradeAddQuantity)) { if (tradeAddQuantity < 1) tradeAddQuantity = 1; }
				bool tradeUsed[MAX_TRADE_SLOTS] = { false };
				for (int i = 0; i < t.myCount && i < MAX_TRADE_SLOTS; ++i)
					if (t.mine[i].slot < MAX_TRADE_SLOTS) tradeUsed[t.mine[i].slot] = true;
				int nextFree = -1;
				for (int i = 0; i < MAX_TRADE_SLOTS; ++i) { if (!tradeUsed[i]) { nextFree = i; break; } }
				ImGui::TextUnformatted(u8"가방 (클릭 = 거래에 올리기):");
				for (int slot = 0; slot < MAX_INVENTORY_SLOTS; ++slot)
				{
					const ClientItemSlot& s = inv.bag[slot];
					if (s.templateId == 0) continue;
					const ItemDef* d = FindItemDef(s.templateId);
					char nm[ITEM_NAME_MAX * 3];
					ToUtf8(d ? d->name : L"?", nm, sizeof(nm));
					char b[ITEM_NAME_MAX * 3 + 24];
					::sprintf_s(b, u8"%s x%d##add%d", nm, s.quantity, slot);
					ImGui::BeginDisabled(nextFree < 0);   // 거래칸 9개 다 차면 비활성
					if (ImGui::Button(b))
					{
						KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
						out.Begin(static_cast<USHORT>(PacketType::CS_TRADE_ADD_ITEM));
						CS_TRADE_ADD_ITEM req{};
						req.bagSlot = static_cast<BYTE>(slot);
						req.quantity = tradeAddQuantity;
						req.tradeSlot = static_cast<BYTE>(nextFree);
						req.Serialize(out); const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
						if (built) { clientSocket.Send(out); }
					}
					ImGui::EndDisabled();
				}
				ImGui::Separator();
				if (ImGui::Button(u8"수락"))
				{
					KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
					out.Begin(static_cast<USHORT>(PacketType::CS_TRADE_ACCEPT));   // payload 없음
					const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
					if (built) { clientSocket.Send(out); }
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"취소"))
				{
					KYS::GAMECOMMON::PROTOCOL::CPacket out(MAX_PACKET_SIZE);
					out.Begin(static_cast<USHORT>(PacketType::CS_TRADE_CANCEL));   // payload 없음
					const bool built = out.End(); if (!built) { ::wprintf(L"[CLIENT] 직렬화 실패 - 버퍼 초과 (type=0x%04X size=%d)\n", out.GetType(), out.GetSize()); }
					if (built) { clientSocket.Send(out); }
				}
				ImGui::End();
			}
		}

		// (4-5) 렌더 + Present(vsync)
		ImGui::Render();
		const float clearColor[4] = { 0.06f, 0.07f, 0.09f, 1.0f };
		g_deviceContext->OMSetRenderTargets(1, &g_renderTargetView, nullptr);
		g_deviceContext->ClearRenderTargetView(g_renderTargetView, clearColor);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_swapChain->Present(1, 0);
	}

	// (5) 정리
	clientSocket.Close();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);
	::WSACleanup();
	return 0;
}
