#pragma once
#include "Protocol/CPacket.h"   // 재사용 역직렬화 버퍼 멤버용
#include "Protocol/GamePackets.h"   // SC_CHANNEL_LIST 보관 (채널 선택)

class LocalPlayer;
class GameState;

// 클라 인벤 칸 하나 - 서버 슬롯 레이아웃 미러(가방 0..MAX_INVENTORY_SLOTS-1 + 장착 무기/방어구).
//   templateId 0 = 빈 칸. SC_INVENTORY 는 전량 교체, SC_ITEM_UPDATE 는 지정 칸만 패치.
struct ClientItemSlot
{
	unsigned long long uid;
	int                templateId;   // 0 = 빈 칸
	int                quantity;
};

// 클라가 표시용으로 보관하는 소지품 전량 (서버가 권위, 클라는 미러).
struct ClientInventory
{
	ClientItemSlot bag[MAX_INVENTORY_SLOTS];
	ClientItemSlot weapon;
	ClientItemSlot armor;
};

// 수신 SC 패킷을 종류별로 해석해 LocalPlayer/GameState 를 갱신한다.
// ClientSocket 의 framing 이 완성 패킷을 잘라 Dispatch 로 넘긴다.
class PacketProcessor
{
public:
	PacketProcessor(LocalPlayer& localPlayer, GameState& gameState);

	PacketProcessor(const PacketProcessor&) = delete;
	PacketProcessor& operator=(const PacketProcessor&) = delete;

	void Dispatch(unsigned short type, const unsigned char* payload, int payloadLen);

	// 채널 선택(B) - 게임 서버 접속(verify) 후 받은 채널 목록 보관. main이 조회해 채널 선택 UI 렌더.
	bool HasChannelList() const { return m_hasChannelList; }
	const SC_CHANNEL_LIST& GetChannelList() const { return m_channelList; }
	void ClearChannelList() { m_hasChannelList = false; }

	// 캐릭터 선택(B) - 채널 선택 후 서버가 push 한 캐릭터 목록. 생성/삭제 OK 시 서버가 갱신본 재푸시.
	bool HasCharacterList() const { return m_hasCharacterList; }
	const SC_CHARACTER_LIST& GetCharacterList() const { return m_characterList; }
	void ClearCharacterList() { m_hasCharacterList = false; }
	// 캐릭터 목록 재푸시 횟수 - 서버가 목록을 보낼 때마다 1씩 증가. main 이 admission 대기(pending) 해제(거부/재리스트 감지)에 사용.
	unsigned int GetCharacterListVersion() const { return m_characterListVersion; }

	// 생성/삭제 결과 - main이 한 번 읽고 소비(폼 닫기/에러 표시). 없으면 false. 목록 표시는 재푸시된 목록이 권위.
	bool TakeCreateResult(BYTE& outResult, unsigned int& outCharId);
	bool TakeDeleteResult(BYTE& outResult);

	// 인게임 채널 변경(C) 결과 - main이 한 번 읽고 소비(피드백/picker 닫기). OK 시 GameState.Reset은 수신 즉시 처리됨(옛 채널 시야 제거).
	bool TakeChannelChangeResult(BYTE& outResult);

	// 소지품(SC_INVENTORY 전량 / SC_ITEM_UPDATE 패치) - main 이 인벤 창 렌더에 조회. 장착 반영 공/방(SC_STAT_UPDATE)도.
	bool                   HasInventory() const { return m_hasInventory; }
	const ClientInventory& GetInventory() const { return m_inventory; }
	int                    GetAtkPower() const { return m_atkPower; }
	int                    GetDefPower() const { return m_defPower; }
	// 아이템 조작 실패 사유(SC_ITEM_RESULT) - main 이 한 번 읽고 소비(에러 토스트). EItemResult 값.
	bool TakeItemResult(BYTE& outResult);

	// 거래 요청 수신(SC_TRADE_REQUEST) - main 이 수락/거절 다이얼로그. 응답/취소 시 Clear.
	bool           HasTradeRequest() const { return m_hasTradeRequest; }
	const wchar_t* GetTradeRequesterName() const { return m_tradeRequesterName; }
	void           ClearTradeRequest() { m_hasTradeRequest = false; }
	// 거래창 열림(SC_TRADE_OPEN/UPDATE) - main 이 거래 창 렌더. 상대 이름 + 마지막 상태(양측 오퍼/수락).
	bool                   IsTradeOpen() const { return m_tradeOpen; }
	const wchar_t*         GetTradePartnerName() const { return m_tradePartnerName; }
	const SC_TRADE_UPDATE& GetTradeState() const { return m_tradeState; }
	// 거래 종결(SC_TRADE_RESULT) - main 이 한 번 읽고 소비(창 닫기 + 결과 토스트). ETradeResult 값.
	bool TakeTradeResult(BYTE& outResult);

	// 연결 종료 시 인게임 상태(인벤/스탯/거래) 초기화 - 재접속 후 옛 세션 창이 남지 않게(종료 통지는 끊긴 연결로 못 옴).
	void ClearInGameState();

	// RTT(왕복 시간) - 마지막 CS_PING 송신과 그 SC_PONG 수신 사이 ms. main HUD 표시용. -1 = 아직 측정 안 됨.
	int GetLastRttMs() const { return m_lastRttMs; }
	// 끊김 시 RTT 측정 상태 초기화 - 죽은 연결의 옛 RTT가 끊김 화면/재접속 후 표시되지 않게 -1(미측정)로 되돌린다.
	void ResetRtt() { m_lastRttMs = -1; }

#ifdef _DEBUG
	// H6 (검증 하니스): 인위 RTT - self-echo SC_MOVE_BROADCAST 를 lag ms 지연 적용해 러버밴드를 재현/관측.
	//   lag 은 환경변수 KYS_RTT_SIM_MS 로 설정(0=off, 기본). Release 는 이 코드가 전부 컴파일 제외돼 즉시 적용(오늘과 동일).
	void      PollDelayedEchoes();               // 매 프레임 - lag 경과한 지연 self-echo 를 적용(off면 no-op)
	void      ClearRttEchoQueue() { m_rttEchoHead = 0; m_rttEchoCount = 0; }   // 끊김/재접속 시 옛 세션 지연 에코 버림
	long long GetRttRubberBandCount() const { return m_rttRubberBandCount; }
	int       GetRttSimLagMs() const { return m_rttSimLagMs; }
#endif

private:
	LocalPlayer& m_localPlayer;
	GameState&   m_gameState;
	KYS::GAMECOMMON::PROTOCOL::CPacket m_parsePkt;   // 재사용 역직렬화 버퍼(매 패킷 Reset + Write)
	SC_CHANNEL_LIST m_channelList;      // 마지막 수신 채널 목록
	bool            m_hasChannelList;   // 채널 목록 수신 여부 (채널 선택 UI 표시 조건)

	SC_CHARACTER_LIST m_characterList;     // 마지막 수신 캐릭터 목록 (선택 화면 권위)
	bool              m_hasCharacterList;  // 캐릭터 목록 수신 여부 (캐릭터 선택 UI 표시 조건)
	unsigned int      m_characterListVersion;  // 캐릭터 목록 재푸시 횟수 (main 이 admission pending 거부/재리스트 감지)
	bool              m_hasCreateResult;   // 생성 결과 대기 (main이 소비)
	BYTE              m_createResult;      // ECharCreateResult 값
	unsigned int      m_createResultCharId;// 생성 성공 시 새 char_id
	bool              m_hasDeleteResult;   // 삭제 결과 대기 (main이 소비)
	BYTE              m_deleteResult;      // ECharDeleteResult 값

	bool              m_hasChannelChangeResult;   // 채널 변경 결과 대기 (main이 소비)
	BYTE              m_channelChangeResult;      // EChannelChangeResult 값

	// 소지품/스탯 (서버 미러 - 표시용)
	ClientInventory   m_inventory;        // 마지막 수신 소지품 (SC_INVENTORY 전량 / SC_ITEM_UPDATE 패치)
	bool              m_hasInventory;     // 인벤 수신 여부 (인벤 창 표시 조건)
	int               m_atkPower;         // SC_STAT_UPDATE 공격력 (장착 반영)
	int               m_defPower;         // SC_STAT_UPDATE 방어력
	bool              m_hasItemResult;    // 아이템 조작 실패 대기 (main 소비)
	BYTE              m_itemResult;       // EItemResult 값

	// 거래 (상태기계 미러)
	bool              m_hasTradeRequest;                       // 들어온 거래 요청 대기 (수락/거절 다이얼로그)
	wchar_t           m_tradeRequesterName[WHISPER_NAME_MAX];  // 요청자 이름
	bool              m_tradeOpen;                             // 거래창 열림
	wchar_t           m_tradePartnerName[WHISPER_NAME_MAX];    // 거래 상대 이름
	SC_TRADE_UPDATE   m_tradeState;                            // 마지막 거래창 상태 (양측 오퍼 + 수락)
	bool              m_hasTradeResult;                        // 거래 종결 대기 (main 소비)
	BYTE              m_tradeResult;                           // ETradeResult 값

	int             m_lastRttMs;        // 마지막 CS_PING->SC_PONG 왕복 시간(ms). 초기 -1(미측정).

#ifdef _DEBUG
	// H6 지연 self-echo 큐 (고정 링 - Fixed Array 규약). lag>0 일 때만 사용, 평시 count=0.
	struct DelayedEcho { unsigned int lastSeq; int x; int y; unsigned long long arrivalMs; };
	static const int RTT_SIM_QUEUE_CAP = 64;
	DelayedEcho m_rttEchoQueue[RTT_SIM_QUEUE_CAP];
	int         m_rttEchoHead;         // pop 위치
	int         m_rttEchoCount;        // 큐에 든 개수
	int         m_rttSimLagMs;         // 0 = off (환경변수 KYS_RTT_SIM_MS 로 설정)
	long long   m_rttRubberBandCount;  // self-echo 적용 변위 > 1틱(6px) 인 횟수 (러버밴드 지표 - 버그면 증가/수정 후 ~0)
	void EnqueueDelayedEcho(unsigned int lastSeq, int x, int y);   // 지연 큐 push (arrival = 지금)
	void ApplySelfEcho(unsigned int lastSeq, int x, int y);        // OnMoveEcho(lastSeq) + 변위 측정(러버밴드 카운트)
#endif
};
