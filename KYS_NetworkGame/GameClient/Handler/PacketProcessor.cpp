#include "pch_gameclient.h"
#include "PacketProcessor.h"
#include "../Game/LocalPlayer.h"
#include "../Game/GameState.h"
#include "Protocol/GamePackets.h"
#include "Protocol/PacketType.h"
#ifdef _DEBUG
#include <stdlib.h>   // H6: _wdupenv_s / _wtoi / abs / free (인위 RTT lag 환경변수 읽기 + 변위 측정)
#endif

using namespace KYS::GAMECOMMON::PROTOCOL;

namespace
{
	// 슬롯 번호로 클라 인벤 칸 참조 (없는 번호 = nullptr). 가방 0..MAX-1 + 장착 무기/방어구.
	ClientItemSlot* SlotRef(ClientInventory& inv, int slot)
	{
		if (slot >= 0 && slot < MAX_INVENTORY_SLOTS) { return &inv.bag[slot]; }
		if (slot == EQUIP_SLOT_WEAPON) { return &inv.weapon; }
		if (slot == EQUIP_SLOT_ARMOR)  { return &inv.armor; }
		return nullptr;
	}
	// 한 슬롯 엔트리 반영 (templateId 0 = 빈 칸으로 비움 - SC_ITEM_UPDATE 가 비운 칸).
	void ApplyEntry(ClientInventory& inv, const ItemSlotEntry& e)
	{
		ClientItemSlot* s = SlotRef(inv, e.slot);
		if (s == nullptr) { return; }
		s->uid = e.uid;
		s->templateId = e.templateId;
		s->quantity = e.quantity;
	}
	void ClearInventory(ClientInventory& inv)
	{
		for (int i = 0; i < MAX_INVENTORY_SLOTS; ++i) { inv.bag[i].uid = 0; inv.bag[i].templateId = 0; inv.bag[i].quantity = 0; }
		inv.weapon.uid = 0; inv.weapon.templateId = 0; inv.weapon.quantity = 0;
		inv.armor.uid = 0;  inv.armor.templateId = 0;  inv.armor.quantity = 0;
	}
}

PacketProcessor::PacketProcessor(LocalPlayer& localPlayer, GameState& gameState)
	: m_localPlayer(localPlayer)
	, m_gameState(gameState)
	, m_parsePkt(RECV_BUFFER_SIZE)
	, m_channelList()
	, m_hasChannelList(false)
	, m_characterList()
	, m_hasCharacterList(false)
	, m_characterListVersion(0)
	, m_hasCreateResult(false)
	, m_createResult(0)
	, m_createResultCharId(0)
	, m_hasDeleteResult(false)
	, m_deleteResult(0)
	, m_hasChannelChangeResult(false)
	, m_channelChangeResult(0)
	, m_inventory()
	, m_hasInventory(false)
	, m_atkPower(0)
	, m_defPower(0)
	, m_hasItemResult(false)
	, m_itemResult(0)
	, m_hasTradeRequest(false)
	, m_tradeOpen(false)
	, m_tradeState()
	, m_hasTradeResult(false)
	, m_tradeResult(0)
	, m_lastRttMs(-1)
{
	m_tradeRequesterName[0] = L'\0';
	m_tradePartnerName[0] = L'\0';
#ifdef _DEBUG
	// H6: 인위 RTT lag 을 환경변수 KYS_RTT_SIM_MS 에서 읽는다(0=off). 큐/카운터 초기화.
	m_rttEchoHead = 0;
	m_rttEchoCount = 0;
	m_rttSimLagMs = 0;
	m_rttRubberBandCount = 0;
	wchar_t* lagEnv = nullptr;
	size_t   lagLen = 0;
	if (::_wdupenv_s(&lagEnv, &lagLen, L"KYS_RTT_SIM_MS") == 0 && lagEnv != nullptr)
	{
		m_rttSimLagMs = ::_wtoi(lagEnv);
		if (m_rttSimLagMs < 0) { m_rttSimLagMs = 0; }
	}
	::free(lagEnv);
#endif
}

void PacketProcessor::Dispatch(unsigned short type, const unsigned char* payload, int payloadLen)
{
	switch (static_cast<PacketType>(type))
	{
	case PacketType::SC_MAP_CHANGE:   // 16B: playerId4 + mapId4 + x4 + y4 - 자기 배치(서버 권위 좌표, 포탈. 로그인은 SC_CHAR_INFO)
	{
		if (payloadLen != 16)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_MAP_CHANGE body{};
		body.Deserialize(m_parsePkt);
		// 맵이 바뀌면 옛 맵의 원격 엔티티를 비운다. 같은 맵 재배치는 유지.
		if (m_localPlayer.IsPlaced() && m_localPlayer.GetMapId() != body.mapId)
			m_gameState.Reset();
		m_localPlayer.OnMapChange(body.playerId, body.mapId, body.x, body.y);
#ifdef _DEBUG
		ClearRttEchoQueue();   // H6: 포탈/재배치 - 지연 큐의 옛 위치 에코 버림(순간이동 후 stale snap 방지)
#endif
		break;
	}
	case PacketType::SC_CHAR_INFO:   // 28B: playerId4+mapId4+x4+y4+hp4+maxHp4+mp4 - 로그인 시 자기 캐릭터 전체 초기 상태(hp 포함)
	{
		if (payloadLen != 28)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_CHAR_INFO body{};
		body.Deserialize(m_parsePkt);
		// 로그인 배치 - 맵이 바뀌면(첫 진입엔 항상) 원격 엔티티 비우고 자기 전체 상태(위치+hp) 설정.
		if (m_localPlayer.IsPlaced() && m_localPlayer.GetMapId() != body.mapId)
			m_gameState.Reset();
		m_localPlayer.OnCharInfo(body.playerId, body.mapId, body.x, body.y, body.hp, body.maxHp);
#ifdef _DEBUG
		ClearRttEchoQueue();   // H6: 로그인/채널변경 재배치 - 구채널 좌표 지연 에코 버림(텔레포트 false positive 방지)
#endif
		break;
	}
	case PacketType::SC_MOVE_BROADCAST:   // 18B: id4+ms1+dir1+x4+y4+seq4 - self-echo 면 reconcile
	{
		if (payloadLen != 18)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_MOVE_BROADCAST body{};
		body.Deserialize(m_parsePkt);
		if (m_localPlayer.GetId() != 0 && body.playerId == m_localPlayer.GetId())
		{
#ifdef _DEBUG
			if (m_rttSimLagMs > 0) { EnqueueDelayedEcho(body.lastSeq, body.x, body.y); }   // H6: 인위 RTT - 지연 큐로 보류(러버밴드 재현)
			else                   { ApplySelfEcho(body.lastSeq, body.x, body.y); }
#else
			m_localPlayer.OnMoveEcho(body.lastSeq, body.x, body.y);
#endif
		}
		else
			m_gameState.OnEntityMove(body.playerId, body.moveState, body.direction, body.x, body.y);
		break;
	}
	case PacketType::SC_SPAWN:   // 가변(24B~): id4+ms1+dir1+x4+y4+hp4+maxHp4+이름(길이2+문자2xN) - 타플레이어 시야 진입
	{
		if (payloadLen < 24)
			break;   // 고정 22B + 이름 길이 프리픽스 2B 최소
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_SPAWN body{};
		body.Deserialize(m_parsePkt);
		if (m_localPlayer.GetId() != 0 && body.playerId == m_localPlayer.GetId())
			break;   // 자기 자신은 GameState 에 안 넣음(서버가 안 보내지만 방어)
		RemoteEntity e{};
		e.id = body.playerId;
		e.objectType = EObjectType::PLAYER;
		e.x = body.x;
		e.y = body.y;
		e.moveState = body.moveState;
		e.direction = body.direction;
		e.hp = body.hp;
		e.maxHp = body.maxHp;
		e.dead = (body.hp <= 0);   // hp=0 스폰 = death-grace 시체 - 클라가 회색 시신으로 렌더(서버 가드 제거로 신규 진입자도 시체를 대칭 관측)
		::wcscpy_s(e.name, CHAT_SENDER_MAX, body.name);   // 스폰에 동봉된 캐릭터 이름 - 즉시 이름표
		m_gameState.OnSpawn(e);
		break;
	}
	case PacketType::SC_DESPAWN:   // 4B: id - 시야 이탈(플레이어/몬스터 공용)
	{
		if (payloadLen != 4)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_DESPAWN body{};
		body.Deserialize(m_parsePkt);
		m_gameState.OnDespawn(body.playerId);
		break;
	}
	case PacketType::SC_MONSTER_SPAWN:   // 33B: id4+ms1+dir1+x4+y4+hp4+maxHp4+objType1+monType1+atkRange4+moveSpeed4+isBoss1 - 몬스터 시야 진입
	{
		if (payloadLen != 33)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_MONSTER_SPAWN body{};
		body.Deserialize(m_parsePkt);
		RemoteEntity e{};
		e.id = body.monsterId;
		e.objectType = static_cast<EObjectType>(body.objectType);     // 서버가 MONSTER 로 채워 보냄
		e.monsterType = static_cast<EMonsterType>(body.monsterType);  // 종류별 글리프 선택용
			e.attackRange = body.attackRange;                             // 공격범위 표시용 사거리(몬스터 전용)
		e.moveSpeed = body.moveSpeed;                                  // 위치 예측 속도(종류별 - 서버 스냅샷 값)
		e.isBoss = (body.isBoss != 0);                                 // 보스 렌더 강조(서버 판정 - 종류 하드코딩 대체)
		e.x = body.x;
		e.y = body.y;
		e.moveState = body.moveState;
		e.direction = body.direction;
		e.hp = body.hp;
		e.maxHp = body.maxHp;
		e.dead = (body.hp <= 0);   // hp=0 스폰 = death-grace 시체 - 클라가 회색 시신으로 렌더(서버 가드 제거로 신규 진입자도 시체를 대칭 관측)
		m_gameState.OnSpawn(e);   // player 와 같은 맵에 저장(id 공간 공유)
		break;
	}
	case PacketType::SC_MONSTER_MOVE:   // 14B: id4+ms1+dir1+x4+y4 - 몬스터 이동 anchor(seq 없음, 서버 권위)
	{
		if (payloadLen != 14)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_MONSTER_MOVE body{};
		body.Deserialize(m_parsePkt);
		m_gameState.OnEntityMove(body.monsterId, body.moveState, body.direction, body.x, body.y);
		break;
	}
	case PacketType::SC_DAMAGE:   // 16B: atkId4+targetId4+damage4+remainingHp4 - 피격(체력바 동기 + 플로팅 데미지)
	{
		if (payloadLen != 16)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_DAMAGE body{};
		body.Deserialize(m_parsePkt);
		if (m_localPlayer.GetId() != 0 && body.targetId == m_localPlayer.GetId())
		{
			m_localPlayer.SetHp(body.remainingHp);           // 내가 맞음 - 내 hp 갱신
			m_localPlayer.TriggerDamagePopup(body.damage);   // 내 머리 위 피격 데미지 숫자
		}
		else
			m_gameState.OnDamage(body.targetId, body.damage, body.remainingHp);   // 타 객체 hp + 플로팅 데미지
			// 공격범위 표시 - 원격 공격자(타플레이어/몬스터)에게 flash. 자기 공격은 CS_SKILL 송신 시점(main.cpp)에 이미 표시(헛스윙 포함)라 여기 self-branch는 히트 시 refresh만. (위 target if/else와 별개로 항상 실행)
			if (m_localPlayer.GetId() != 0 && body.atkId == m_localPlayer.GetId())
				m_localPlayer.TriggerAttackFlash();
			else
				m_gameState.OnAttackerFlash(body.atkId);
		break;
	}
	case PacketType::SC_DEATH:   // 4B: targetId - 사망(시신 상태 전환 - 제거는 SC_DESPAWN)
	{
		if (payloadLen != 4)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_DEATH body{};
		body.Deserialize(m_parsePkt);
		if (m_localPlayer.GetId() != 0 && body.targetId == m_localPlayer.GetId())
			m_localPlayer.SetHp(0);   // 내 사망 - hp바 0 (곧 SC_RESPAWN 으로 복원)
		else
			m_gameState.OnEntityDeath(body.targetId);   // 시신 상태(회색, 1초) - 즉시 지우면 막타 데미지 숫자까지 소실. 실제 제거 = 서버 시신 유예 후 SC_DESPAWN
		break;
	}
	case PacketType::SC_RESPAWN:   // 20B: playerId4+x4+y4+hp4+maxHp4 - 부활(hp/위치 복원)
	{
		if (payloadLen != 20)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_RESPAWN body{};
		body.Deserialize(m_parsePkt);
		if (m_localPlayer.GetId() != 0 && body.playerId == m_localPlayer.GetId())
		{
			m_localPlayer.OnRespawn(body.x, body.y, body.hp, body.maxHp);   // 내 부활
#ifdef _DEBUG
			ClearRttEchoQueue();   // H6: 부활 순간이동 - 사망 전 위치 에코가 시신 위치로 되끌지 않게(텔레포트 false positive 방지)
#endif
		}
		else
			m_gameState.OnRespawn(body.playerId, body.x, body.y, body.hp, body.maxHp);   // 타 플레이어 부활
		break;
	}
	case PacketType::SC_CHAT_BROADCAST:   // 가변길이: playerId4 + sender(str) + message(str) - 채팅창 + 말풍선
	{
		if (payloadLen < 8)
			break;   // 최소 = playerId4 + 빈 문자열 2 + 빈 문자열 2 (ReadString이 내부 경계 검사)
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_CHAT_BROADCAST body{};
		body.Deserialize(m_parsePkt);
		m_gameState.AddChat(body.senderName, body.message, false);
		if (m_localPlayer.GetId() != 0 && body.playerId == m_localPlayer.GetId())
			m_localPlayer.SetChatBubble(body.message);                // 자기 발화 - 내 머리 위 말풍선(LocalPlayer 보유)
		else
		{
			m_gameState.SetEntityName(body.playerId, body.senderName);   // 발화자 이름표 학습(P{id} -> 실제 이름)
			m_gameState.SetChatBubble(body.playerId, body.message);     // 원격 발화자 머리 위 말풍선
		}
		break;
	}
	case PacketType::SC_WHISPER:   // 가변길이: sender(str) + message(str) - 귓속말(별도 표기)
	{
		if (payloadLen < 4)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_WHISPER body{};
		body.Deserialize(m_parsePkt);
		m_gameState.AddChat(body.senderName, body.message, true);
		break;
	}
	case PacketType::SC_WHISPER_FAIL:   // 가변길이: targetName(str) + reason(1) - 귓속말 실패(오프라인/도배 제한) 통지
		{
			if (payloadLen < 3)   // targetName(len 2B >= 0) + reason(1B)
				break;
			m_parsePkt.Reset();
			m_parsePkt.Write(payload, payloadLen);
			SC_WHISPER_FAIL body{};
			body.Deserialize(m_parsePkt);
			const wchar_t* failLabel = (body.reason == static_cast<BYTE>(EWhisperFail::OFFLINE))
				? L"귓속말 실패(오프라인)"
				: L"귓속말 실패(잠시 후)";
			m_gameState.AddChat(failLabel, body.targetName, true);   // 발신자 화면에 실패 사유 + 대상 이름 표기
			break;
		}
		case PacketType::SC_CHANNEL_LIST:   // 가변길이: channelCount1 + [channelId1+playerCount4+maxPlayers4]*N - 채널 선택 UI용
	{
		if (payloadLen < 1)
			break;   // 최소 channelCount(1B). Deserialize가 count를 MAX_CHANNEL_COUNT로 클램프
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		m_channelList.Deserialize(m_parsePkt);
		m_hasChannelList = true;   // main이 채널 선택 UI 표시
		break;
	}
	case PacketType::SC_CHARACTER_LIST:   // 가변길이: count1 + [charId4+slot1+name+mapId4+hp4+maxHp4]*N - 캐릭터 선택 UI용
	{
		if (payloadLen < 1)
			break;   // 최소 count(1B). Deserialize가 count를 MAX_CHAR_SLOTS로 클램프
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		m_characterList.Deserialize(m_parsePkt);
		m_hasCharacterList = true;   // main이 캐릭터 선택 UI 표시
		++m_characterListVersion;    // 목록 재푸시 카운트 - main 의 admission pending 해제(거부/재리스트 감지)
		m_hasChannelList = false;    // 캐릭터 단계로 진입 - 옛 채널 목록이 화면 분기에 남지 않게(캐릭터 선택이 우선)
		break;
	}
	case PacketType::SC_CHARACTER_CREATE_RESULT:   // 5B: result1 + charId4 - 생성 결과(폼 피드백). OK면 목록 재푸시가 뒤따름
	{
		if (payloadLen < 5)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_CHARACTER_CREATE_RESULT body{};
		body.Deserialize(m_parsePkt);
		m_createResult = body.result;
		m_createResultCharId = body.charId;
		m_hasCreateResult = true;   // main이 소비(OK면 생성 폼 닫기, 실패면 에러 표시)
		break;
	}
	case PacketType::SC_CHARACTER_DELETE_RESULT:   // 1B: result - 삭제 결과(에러 피드백). OK면 목록 재푸시가 뒤따름
	{
		if (payloadLen < 1)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_CHARACTER_DELETE_RESULT body{};
		body.Deserialize(m_parsePkt);
		m_deleteResult = body.result;
		m_hasDeleteResult = true;   // main이 소비(실패면 에러 표시, OK는 재푸시된 목록이 비움)
		break;
	}
	case PacketType::SC_CHANNEL_CHANGE_RESULT:   // 2B: result1 + channelId1 - 인게임 채널 변경 결과. OK면 옛 채널 시야 즉시 reset
	{
		if (payloadLen < 2)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_CHANNEL_CHANGE_RESULT body{};
		body.Deserialize(m_parsePkt);
		if (body.result == static_cast<BYTE>(EChannelChangeResult::OK))
			m_gameState.Reset();   // 옛 채널의 원격 엔티티 제거 - 새 채널 spawn/SC_CHAR_INFO가 곧 채운다(RESULT가 spawn보다 먼저 도착)
		m_channelChangeResult = body.result;
		m_hasChannelChangeResult = true;   // main이 소비(피드백 + picker 닫기)
		break;
	}
	case PacketType::SC_PONG:   // 4B: clientTimeMs echo - CS_PING 보낸 시각을 그대로 돌려받아 왕복 시간(RTT) 계산
	{
		if (payloadLen != 4)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_PONG body{};
		body.Deserialize(m_parsePkt);
		const UINT32 now = static_cast<UINT32>(::GetTickCount64());
		m_lastRttMs = static_cast<int>(now - body.clientTimeMs);   // UINT32 차 - 작은 간격이라 wrap 안전
		break;
	}
	case PacketType::SC_INVENTORY:   // 가변: count1 + [slot1+uid8+templateId4+quantity4]*N - 전량 스냅샷(입장/거래 완료)
	{
		if (payloadLen < 1)
			break;   // 최소 count(1B). Deserialize 가 count 를 MAX_ENTRIES 로 클램프
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_INVENTORY body{};
		body.Deserialize(m_parsePkt);
		ClearInventory(m_inventory);   // 전량 교체 - 옛 내용 비우고 받은 칸만 채움
		for (int i = 0; i < body.count && i < SC_INVENTORY::MAX_ENTRIES; ++i)
			ApplyEntry(m_inventory, body.items[i]);
		m_hasInventory = true;   // 인벤 창 표시 조건
		break;
	}
	case PacketType::SC_ITEM_UPDATE:   // 가변: count1 + 엔트리*N - 바뀐 칸만 패치(획득/이동/장착/사용/버림)
	{
		if (payloadLen < 1)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_ITEM_UPDATE body{};
		body.Deserialize(m_parsePkt);
		for (int i = 0; i < body.count && i < SC_ITEM_UPDATE::MAX_ENTRIES; ++i)
			ApplyEntry(m_inventory, body.items[i]);   // 빈 칸(templateId 0)도 그대로 반영
		m_hasInventory = true;
		break;
	}
	case PacketType::SC_ITEM_RESULT:   // 1B: result - 아이템 조작 실패 사유(성공은 UPDATE/INVENTORY 가 대신)
	{
		if (payloadLen < 1)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_ITEM_RESULT body{};
		body.Deserialize(m_parsePkt);
		m_itemResult = body.result;
		m_hasItemResult = true;   // main 이 소비(에러 토스트)
		break;
	}
	case PacketType::SC_STAT_UPDATE:   // 8B: atkPower4 + defPower4 - 장착 반영 후 자기 공/방
	{
		if (payloadLen != 8)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_STAT_UPDATE body{};
		body.Deserialize(m_parsePkt);
		m_atkPower = body.atkPower;
		m_defPower = body.defPower;
		break;
	}
	case PacketType::SC_GROUND_ITEM_SPAWN:   // 20B: objectId4+templateId4+x4+y4+quantity4 - 바닥 드랍 시야 등장
	{
		if (payloadLen != 20)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_GROUND_ITEM_SPAWN body{};
		body.Deserialize(m_parsePkt);
		GroundItemView g{};
		g.objectId = body.objectId;
		g.templateId = body.templateId;
		g.x = body.x;
		g.y = body.y;
		g.quantity = body.quantity;
		m_gameState.OnGroundItemSpawn(g);   // despawn 은 SC_DESPAWN 공용(GameState 가 두 컨테이너에서 제거)
		break;
	}
	case PacketType::SC_TRADE_REQUEST:   // 가변: requesterName(str) - 상대가 나에게 거래 요청
	{
		if (payloadLen < 2)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_TRADE_REQUEST body{};
		body.Deserialize(m_parsePkt);
		::wcscpy_s(m_tradeRequesterName, WHISPER_NAME_MAX, body.requesterName);
		m_hasTradeRequest = true;   // main 이 수락/거절 다이얼로그
		break;
	}
	case PacketType::SC_TRADE_OPEN:   // 가변: partnerName(str) - 거래 성립, 거래창 열기
	{
		if (payloadLen < 2)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_TRADE_OPEN body{};
		body.Deserialize(m_parsePkt);
		::wcscpy_s(m_tradePartnerName, WHISPER_NAME_MAX, body.partnerName);
		m_tradeOpen = true;
		m_hasTradeRequest = false;   // 요청 다이얼로그가 남아 있으면 닫음(내가 수락한 쪽/요청한 쪽 모두 거래창으로)
		m_tradeState = SC_TRADE_UPDATE{};   // 초기 빈 상태(곧 SC_TRADE_UPDATE 가 채움)
		break;
	}
	case PacketType::SC_TRADE_UPDATE:   // 가변: myAccept1+partnerAccept1+myCount1+partnerCount1 + 오퍼*N - 거래창 현재 상태
	{
		if (payloadLen < 4)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		m_tradeState.Deserialize(m_parsePkt);   // Deserialize 가 count 를 MAX_TRADE_SLOTS 로 클램프
		break;
	}
	case PacketType::SC_TRADE_RESULT:   // 1B: result - 거래 종결(완료/취소/상대 이탈/공간 부족)
	{
		if (payloadLen < 1)
			break;
		m_parsePkt.Reset();
		m_parsePkt.Write(payload, payloadLen);
		SC_TRADE_RESULT body{};
		body.Deserialize(m_parsePkt);
		// INV_FULL 은 거래창 유지(재조정 후 재수락) - 그 외(완료/취소/이탈)는 창 닫기.
		if (body.result != static_cast<BYTE>(ETradeResult::INV_FULL))
		{
			m_tradeOpen = false;
			m_hasTradeRequest = false;
		}
		m_tradeResult = body.result;
		m_hasTradeResult = true;   // main 이 소비(결과 토스트)
		break;
	}
	default:
		// SC_LOGIN_RESULT 는 거부 전용(토큰 검증 실패/만석) - 서버가 직후 연결을 끊으므로 별도 UI 처리 없이 끊김 감지가 정리한다.
		break;
	}
}

bool PacketProcessor::TakeCreateResult(BYTE& outResult, unsigned int& outCharId)
{
	if (!m_hasCreateResult)
		return false;
	outResult = m_createResult;
	outCharId = m_createResultCharId;
	m_hasCreateResult = false;   // 1회 소비
	return true;
}

bool PacketProcessor::TakeDeleteResult(BYTE& outResult)
{
	if (!m_hasDeleteResult)
		return false;
	outResult = m_deleteResult;
	m_hasDeleteResult = false;   // 1회 소비
	return true;
}

bool PacketProcessor::TakeChannelChangeResult(BYTE& outResult)
{
	if (!m_hasChannelChangeResult)
		return false;
	outResult = m_channelChangeResult;
	m_hasChannelChangeResult = false;   // 1회 소비
	return true;
}

bool PacketProcessor::TakeItemResult(BYTE& outResult)
{
	if (!m_hasItemResult)
		return false;
	outResult = m_itemResult;
	m_hasItemResult = false;   // 1회 소비
	return true;
}

bool PacketProcessor::TakeTradeResult(BYTE& outResult)
{
	if (!m_hasTradeResult)
		return false;
	outResult = m_tradeResult;
	m_hasTradeResult = false;   // 1회 소비
	return true;
}

void PacketProcessor::ClearInGameState()
{
	ClearInventory(m_inventory);
	m_hasInventory = false;
	m_atkPower = 0;
	m_defPower = 0;
	m_hasItemResult = false;
	m_hasTradeRequest = false;
	m_tradeRequesterName[0] = L'\0';
	m_tradeOpen = false;
	m_tradePartnerName[0] = L'\0';
	m_tradeState = SC_TRADE_UPDATE{};
	m_hasTradeResult = false;
	m_hasCreateResult = false;        // 끊김 정리와 같은 프레임의 1회 소비가 초기화된 상태 메시지를 되살리지 않게 (재접속 후 유령 토스트 방지)
	m_hasDeleteResult = false;
	m_hasChannelChangeResult = false;
#ifdef _DEBUG
	ClearRttEchoQueue();   // H6: 끊김/재접속 - 옛 세션 지연 에코를 새 세션에 적용하지 않게 비운다
#endif
}

#ifdef _DEBUG
// H6 (검증 하니스) - 인위 RTT self-echo 지연/측정. 이 블록 전체가 Release 에서 컴파일 제외된다.

// self-echo 를 지연 큐에 push (arrival = 지금). 큐 포화(비정상)면 드롭.
void PacketProcessor::EnqueueDelayedEcho(unsigned int lastSeq, int x, int y)
{
	if (m_rttEchoCount >= RTT_SIM_QUEUE_CAP) { return; }
	const int slot = (m_rttEchoHead + m_rttEchoCount) % RTT_SIM_QUEUE_CAP;
	m_rttEchoQueue[slot].lastSeq = lastSeq;
	m_rttEchoQueue[slot].x = x;
	m_rttEchoQueue[slot].y = y;
	m_rttEchoQueue[slot].arrivalMs = ::GetTickCount64();
	++m_rttEchoCount;
}

// self-echo 적용 + 변위 측정. reconcile 후 위치 변화가 1틱(6px) 을 넘으면 러버밴드로 집계.
//   수정 전(무조건 snap) + lag 이면 매 에코가 예측 위치에서 stale 서버 위치로 크게 되당겨 카운터가 오른다.
//   수정 후(seq reconcile): 서버가 보고를 수용한 에코는 무변위(현재 예측 유지)라 카운터 ~0 - fix 효능 지표.
void PacketProcessor::ApplySelfEcho(unsigned int lastSeq, int x, int y)
{
	const int beforeX = m_localPlayer.GetX();
	const int beforeY = m_localPlayer.GetY();
	m_localPlayer.OnMoveEcho(lastSeq, x, y);
	const int dispPx = ::abs(m_localPlayer.GetX() - beforeX) + ::abs(m_localPlayer.GetY() - beforeY);
	if (m_rttSimLagMs > 0 && dispPx > 6) { ++m_rttRubberBandCount; }   // ~1 tick step(6px) 초과 = 러버밴드
}

// 매 프레임 - lag 이 경과한 지연 에코를 FIFO 순서로 적용. (뒤 항목은 더 최근이라 앞이 미경과면 멈춘다.)
void PacketProcessor::PollDelayedEchoes()
{
	const unsigned long long now = ::GetTickCount64();
	while (m_rttEchoCount > 0)
	{
		const DelayedEcho& e = m_rttEchoQueue[m_rttEchoHead];
		if (now - e.arrivalMs < static_cast<unsigned long long>(m_rttSimLagMs)) { break; }
		ApplySelfEcho(e.lastSeq, e.x, e.y);
		m_rttEchoHead = (m_rttEchoHead + 1) % RTT_SIM_QUEUE_CAP;
		--m_rttEchoCount;
	}
}
#endif
