#include "pch_gamecommon.h"
#include "GamePackets.h"
#include "PacketType.h"   // BuildDamagePacket/BuildDeathPacket 의 opcode

using namespace KYS::GAMECOMMON::PROTOCOL;

// 각 패킷의 Serialize/Deserialize. 두 함수는 반드시 같은 필드 순서여야 함(어긋나면 값이 밀려 깨짐).
//   정수는 SerializationBuffer가 내부에서 네트워크 바이트순서로 변환. 문자열은 WriteString/ReadString 사용.

// ===== 시스템 / 연결 =====

void CS_PING::Serialize(CPacket& pkt) const { pkt << clientTimeMs; }
void CS_PING::Deserialize(CPacket& pkt) { pkt >> clientTimeMs; }
void SC_PONG::Serialize(CPacket& pkt) const { pkt << clientTimeMs; }
void SC_PONG::Deserialize(CPacket& pkt) { pkt >> clientTimeMs; }

// ===== 로그인 =====

void LoginReq::Serialize(CPacket& pkt) const
{
    pkt.WriteString(id, ID_MAX);   // 문자열은 전용 함수 사용 (길이 prefix + 글자별 기록)
    pkt.WriteString(pw, PASSWORD_MAX);   // 비번 평문 (TLS 가정 - 본 프로젝트는 평문 over wire 한계, 서버측 해시)
}

void LoginReq::Deserialize(CPacket& pkt)
{
    pkt.ReadString(id, ID_MAX);          // maxLen 전달
    pkt.ReadString(pw, PASSWORD_MAX);    // maxLen 전달 (초과분 clamp)
}

void CS_REGISTER::Serialize(CPacket& pkt) const
{
    pkt.WriteString(id, LoginReq::ID_MAX);
    pkt.WriteString(pw, PASSWORD_MAX);
}

void CS_REGISTER::Deserialize(CPacket& pkt)
{
    pkt.ReadString(id, LoginReq::ID_MAX);
    pkt.ReadString(pw, PASSWORD_MAX);
}

void SC_REGISTER_RESULT::Serialize(CPacket& pkt) const
{
    pkt << result;
}

void SC_REGISTER_RESULT::Deserialize(CPacket& pkt)
{
    pkt >> result;
}

void SC_LOGIN_RESULT::Serialize(CPacket& pkt) const
{
    pkt << result;
    pkt << chId;
}

void SC_LOGIN_RESULT::Deserialize(CPacket& pkt)
{
    pkt >> result;
    pkt >> chId;
}

// ===== 이동 / 위치 / 스폰 =====

void CS_MOVE::Serialize(CPacket& pkt) const
{
    pkt << static_cast<BYTE>(moveState);
    pkt << static_cast<BYTE>(direction);
    pkt << x;
    pkt << y;
    pkt << clientSeq;
}
void CS_MOVE::Deserialize(CPacket& pkt)
{
    BYTE ms = 0;
    pkt >> ms;
    moveState = static_cast<EMoveState>(ms);
    BYTE dir = 0;
    pkt >> dir;
    direction = static_cast<EMoveDirection>(dir);
    pkt >> x;
    pkt >> y;
    pkt >> clientSeq;
}

void SC_MOVE_BROADCAST::Serialize(CPacket& pkt) const
{
    pkt << playerId;
    pkt << static_cast<BYTE>(moveState);
    pkt << static_cast<BYTE>(direction);
    pkt << x;
    pkt << y;
    pkt << lastSeq;
}
void SC_MOVE_BROADCAST::Deserialize(CPacket& pkt)
{
    pkt >> playerId;
    BYTE ms = 0;
    pkt >> ms;
    moveState = static_cast<EMoveState>(ms);
    BYTE d = 0;
    pkt >> d;
    direction = static_cast<EMoveDirection>(d);
    pkt >> x;
    pkt >> y;
    pkt >> lastSeq;
}

void SC_SPAWN::Serialize(CPacket& pkt) const
{
    pkt << playerId;
    pkt << static_cast<BYTE>(moveState);
    pkt << static_cast<BYTE>(direction);
    pkt << x;
    pkt << y;
    pkt << hp;
    pkt << maxHp;
    pkt.WriteString(name, WHISPER_NAME_MAX);   // 캐릭터 이름 (길이2B + 문자2BxN)
}
void SC_SPAWN::Deserialize(CPacket& pkt)
{
    pkt >> playerId;
    BYTE ms = 0; pkt >> ms; moveState = static_cast<EMoveState>(ms);
    BYTE dir = 0; pkt >> dir; direction = static_cast<EMoveDirection>(dir);
    pkt >> x;
    pkt >> y;
    pkt >> hp;
    pkt >> maxHp;
    pkt.ReadString(name, WHISPER_NAME_MAX);
}

void SC_DESPAWN::Serialize(CPacket& pkt) const
{
    pkt << playerId;
}
void SC_DESPAWN::Deserialize(CPacket& pkt)
{
    pkt >> playerId;
}

void CS_PORTAL::Serialize(CPacket& pkt) const
{
    pkt << portalId;   // wire 크기는 그대로, 의미만 포탈 식별자
}
void CS_PORTAL::Deserialize(CPacket& pkt)
{
    pkt >> portalId;
}

void SC_MONSTER_MOVE::Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
{
    pkt << monsterId;
    pkt << static_cast<BYTE>(moveState);
    pkt << static_cast<BYTE>(direction);
    pkt << x;
    pkt << y;
}
void SC_MONSTER_MOVE::Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
{
    pkt >> monsterId;
    BYTE ms; pkt >> ms; moveState = static_cast<EMoveState>(ms);
    BYTE dr; pkt >> dr; direction = static_cast<EMoveDirection>(dr);
    pkt >> x;
    pkt >> y;
}

void SC_MONSTER_SPAWN::Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
{
    pkt << monsterId;
    pkt << static_cast<BYTE>(moveState);
    pkt << static_cast<BYTE>(direction);
    pkt << x;
    pkt << y;
    pkt << hp;
    pkt << maxHp;
    pkt << objectType;
    pkt << monsterType;   // objectType(구분) 다음에 종류 1B (Deserialize와 같은 위치)
    pkt << attackRange;   // 종류 다음에 공격 사거리 4B (Deserialize와 같은 위치)
    pkt << moveSpeed;     // 이동 속도 4B (클라 위치 예측용 - 종류별 속도)
    pkt << isBoss;        // 보스 여부 1B (클라 렌더 강조)
}
void SC_MONSTER_SPAWN::Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
{
    pkt >> monsterId;
    BYTE ms; pkt >> ms; moveState = static_cast<EMoveState>(ms);
    BYTE dr; pkt >> dr; direction = static_cast<EMoveDirection>(dr);
    pkt >> x;
    pkt >> y;
    pkt >> hp;
    pkt >> maxHp;
    pkt >> objectType;
    pkt >> monsterType;
    pkt >> attackRange;
    pkt >> moveSpeed;
    pkt >> isBoss;
}

void SC_MAP_CHANGE::Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
{
    pkt << playerId;
    pkt << mapId;
    pkt << x;
    pkt << y;
}
void SC_MAP_CHANGE::Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
{
    pkt >> playerId;
    pkt >> mapId;
    pkt >> x;
    pkt >> y;
}

void SC_CHAR_INFO::Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
{
    pkt << playerId;
    pkt << mapId;
    pkt << x;
    pkt << y;
    pkt << hp;
    pkt << maxHp;
    pkt << mp;
}
void SC_CHAR_INFO::Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
{
    pkt >> playerId;
    pkt >> mapId;
    pkt >> x;
    pkt >> y;
    pkt >> hp;
    pkt >> maxHp;
    pkt >> mp;
}

// ===== 전투 =====

void CS_SKILL::Serialize(CPacket& pkt) const
{
    pkt << skillId;
}
void CS_SKILL::Deserialize(CPacket& pkt)
{
    pkt >> skillId;
}

void SC_DAMAGE::Serialize(CPacket& pkt) const
{
    pkt << atkId;
    pkt << targetId;
    pkt << damage;
    pkt << remainingHp;
}
void SC_DAMAGE::Deserialize(CPacket& pkt)
{
    pkt >> atkId;
    pkt >> targetId;
    pkt >> damage;
    pkt >> remainingHp;
}

void SC_DEATH::Serialize(CPacket& pkt) const
{
    pkt << targetId;
}
void SC_DEATH::Deserialize(CPacket& pkt)
{
    pkt >> targetId;
}

// SC_DAMAGE 를 프레이밍까지 완성(Begin+본체+End). damage=원시 판정치, remainingHp=피격 후 남은 HP.
bool BuildDamagePacket(CPacket& out, UINT32 atkId, UINT32 targetId, int damage, int remainingHp)
{
    out.Begin(static_cast<USHORT>(PacketType::SC_DAMAGE));
    SC_DAMAGE body;
    body.atkId = atkId;
    body.targetId = targetId;
    body.damage = damage;
    body.remainingHp = remainingHp;
    body.Serialize(out);
    return out.End();
}

// SC_DEATH 를 프레이밍까지 완성(Begin+본체+End).
bool BuildDeathPacket(CPacket& out, UINT32 targetId)
{
    out.Begin(static_cast<USHORT>(PacketType::SC_DEATH));
    SC_DEATH body;
    body.targetId = targetId;
    body.Serialize(out);
    return out.End();
}

void SC_RESPAWN::Serialize(CPacket& pkt) const
{
    pkt << playerId;
    pkt << x;
    pkt << y;
    pkt << hp;
    pkt << maxHp;
}
void SC_RESPAWN::Deserialize(CPacket& pkt)
{
    pkt >> playerId;
    pkt >> x;
    pkt >> y;
    pkt >> hp;
    pkt >> maxHp;
}

// ===== 채팅 =====

void CS_CHAT::Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
{
    pkt.WriteString(message, CHAT_MSG_MAX);
}
void CS_CHAT::Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
{
    pkt.ReadString(message, CHAT_MSG_MAX);   // 길이 넘침은 ReadString이 잘라서 막음
}

void SC_CHAT_BROADCAST::Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
{
    pkt << playerId;
    pkt.WriteString(senderName, WHISPER_NAME_MAX);   // 발신자 이름 (playerId 다음, message 앞)
    pkt.WriteString(message, CHAT_MSG_MAX);
}
void SC_CHAT_BROADCAST::Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
{
    pkt >> playerId;
    pkt.ReadString(senderName, WHISPER_NAME_MAX);
    pkt.ReadString(message, CHAT_MSG_MAX);
}

void CS_WHISPER::Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
{
    pkt.WriteString(targetName, WHISPER_NAME_MAX);
    pkt.WriteString(message, CHAT_MSG_MAX);
}
void CS_WHISPER::Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
{
    pkt.ReadString(targetName, WHISPER_NAME_MAX);
    pkt.ReadString(message, CHAT_MSG_MAX);
}

void SC_WHISPER::Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
{
    pkt.WriteString(senderName, WHISPER_NAME_MAX);
    pkt.WriteString(message, CHAT_MSG_MAX);
}
void SC_WHISPER::Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
{
    pkt.ReadString(senderName, WHISPER_NAME_MAX);
    pkt.ReadString(message, CHAT_MSG_MAX);
}
void SC_WHISPER_FAIL::Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
{
    pkt.WriteString(targetName, WHISPER_NAME_MAX);
    pkt << reason;
}
void SC_WHISPER_FAIL::Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt)
{
    pkt.ReadString(targetName, WHISPER_NAME_MAX);
    pkt >> reason;
}

// ===== 로그인 서버 분리 - 클라 핸드오프 =====

void ServerEntry::Serialize(CPacket& pkt) const
{
    pkt << serverId;
    pkt << ip;
    pkt << port;
    pkt << population;
    pkt.WriteString(name, WHISPER_NAME_MAX);
}
void ServerEntry::Deserialize(CPacket& pkt)
{
    pkt >> serverId;
    pkt >> ip;
    pkt >> port;
    pkt >> population;
    pkt.ReadString(name, WHISPER_NAME_MAX);
}

void SC_SERVER_LIST::Serialize(CPacket& pkt) const
{
    const BYTE n = (count > MAX_SERVER_LIST) ? static_cast<BYTE>(MAX_SERVER_LIST) : count;   // servers[] OOB read 방어(Deserialize와 대칭)
    pkt << n;
    for (BYTE i = 0; i < n; ++i)
        servers[i].Serialize(pkt);   // 앞 n개만 기록
}
void SC_SERVER_LIST::Deserialize(CPacket& pkt)
{
    pkt >> count;
    if (count > MAX_SERVER_LIST) count = MAX_SERVER_LIST;   // 배열 넘침 방어 (위조 대비)
    for (BYTE i = 0; i < count; ++i)
        servers[i].Deserialize(pkt);
}

void CS_SERVER_SELECT::Serialize(CPacket& pkt) const { pkt << serverId; }
void CS_SERVER_SELECT::Deserialize(CPacket& pkt) { pkt >> serverId; }

void SC_LOGIN_TOKEN::Serialize(CPacket& pkt) const
{
    pkt.WriteU64(token);   // 64비트는 전용 헬퍼 (상위/하위 32비트 분할)
    pkt << serverIp;
    pkt << serverPort;
}
void SC_LOGIN_TOKEN::Deserialize(CPacket& pkt)
{
    pkt.ReadU64(token);
    pkt >> serverIp;
    pkt >> serverPort;
}

void CS_GAME_AUTH::Serialize(CPacket& pkt) const { pkt.WriteU64(token); }
void CS_GAME_AUTH::Deserialize(CPacket& pkt) { pkt.ReadU64(token); }

void SC_CHANNEL_LIST::Serialize(CPacket& pkt) const
{
    const BYTE n = (channelCount > MAX_CHANNEL_COUNT) ? static_cast<BYTE>(MAX_CHANNEL_COUNT) : channelCount;   // entries[] OOB read 방어(Deserialize와 대칭)
    pkt << n;
    for (int i = 0; i < static_cast<int>(n); ++i)
    {
        pkt << entries[i].channelId;
        pkt << entries[i].playerCount;
        pkt << entries[i].maxPlayers;
    }
}
void SC_CHANNEL_LIST::Deserialize(CPacket& pkt)
{
    pkt >> channelCount;
    if (channelCount > MAX_CHANNEL_COUNT) { channelCount = MAX_CHANNEL_COUNT; }   // OOB 가드 (위조 방어)
    for (int i = 0; i < static_cast<int>(channelCount); ++i)
    {
        pkt >> entries[i].channelId;
        pkt >> entries[i].playerCount;
        pkt >> entries[i].maxPlayers;
    }
}

void CS_CHANNEL_SELECT::Serialize(CPacket& pkt) const { pkt << channelId; }
void CS_CHANNEL_SELECT::Deserialize(CPacket& pkt) { pkt >> channelId; }

// ----- 캐릭터 관리 -----

void CharSummary::Serialize(CPacket& pkt) const
{
    pkt << charId;
    pkt << slotId;
    pkt.WriteString(name, WHISPER_NAME_MAX);
    pkt << mapId;
    pkt << hp;
    pkt << maxHp;
}
void CharSummary::Deserialize(CPacket& pkt)
{
    pkt >> charId;
    pkt >> slotId;
    pkt.ReadString(name, WHISPER_NAME_MAX);
    pkt >> mapId;
    pkt >> hp;
    pkt >> maxHp;
}

void SC_CHARACTER_LIST::Serialize(CPacket& pkt) const
{
    const BYTE n = (count > MAX_CHAR_SLOTS) ? static_cast<BYTE>(MAX_CHAR_SLOTS) : count;   // chars[] OOB read 방어(Deserialize와 대칭)
    pkt << n;
    for (BYTE i = 0; i < n; ++i)
        chars[i].Serialize(pkt);
}
void SC_CHARACTER_LIST::Deserialize(CPacket& pkt)
{
    pkt >> count;
    if (count > MAX_CHAR_SLOTS) { count = static_cast<BYTE>(MAX_CHAR_SLOTS); }   // OOB 가드 (위조 방어)
    for (BYTE i = 0; i < count; ++i)
        chars[i].Deserialize(pkt);
}

void CS_CHARACTER_SELECT::Serialize(CPacket& pkt) const { pkt << charId; }
void CS_CHARACTER_SELECT::Deserialize(CPacket& pkt) { pkt >> charId; }

void CS_CHARACTER_CREATE::Serialize(CPacket& pkt) const
{
    pkt.WriteString(name, WHISPER_NAME_MAX);
    pkt << slotId;
}
void CS_CHARACTER_CREATE::Deserialize(CPacket& pkt)
{
    pkt.ReadString(name, WHISPER_NAME_MAX);
    pkt >> slotId;
}

void SC_CHARACTER_CREATE_RESULT::Serialize(CPacket& pkt) const
{
    pkt << result;
    pkt << charId;
}
void SC_CHARACTER_CREATE_RESULT::Deserialize(CPacket& pkt)
{
    pkt >> result;
    pkt >> charId;
}

void CS_CHARACTER_DELETE::Serialize(CPacket& pkt) const { pkt << charId; }
void CS_CHARACTER_DELETE::Deserialize(CPacket& pkt) { pkt >> charId; }

void SC_CHARACTER_DELETE_RESULT::Serialize(CPacket& pkt) const { pkt << result; }
void SC_CHARACTER_DELETE_RESULT::Deserialize(CPacket& pkt) { pkt >> result; }

// ----- 인게임 채널 변경 -----

void CS_CHANNEL_CHANGE::Serialize(CPacket& pkt) const { pkt << channelId; }
void CS_CHANNEL_CHANGE::Deserialize(CPacket& pkt) { pkt >> channelId; }

void SC_CHANNEL_CHANGE_RESULT::Serialize(CPacket& pkt) const
{
    pkt << result;
    pkt << channelId;
}
void SC_CHANNEL_CHANGE_RESULT::Deserialize(CPacket& pkt)
{
    pkt >> result;
    pkt >> channelId;
}
void SC_ENTER_WORLD::Serialize(CPacket& pkt) const { (void)pkt; }   // payload 없음 - opcode 도착 자체가 신호
void SC_ENTER_WORLD::Deserialize(CPacket& pkt) { (void)pkt; }

// ===== 인터서버 =====

void IS_REGISTER::Serialize(CPacket& pkt) const
{
    pkt << serverId;
    pkt << authKeyHash;
    pkt << listenIp;
    pkt << listenPort;
}
void IS_REGISTER::Deserialize(CPacket& pkt)
{
    pkt >> serverId;
    pkt >> authKeyHash;
    pkt >> listenIp;
    pkt >> listenPort;
}

void IS_REGISTER_ACK::Serialize(CPacket& pkt) const { pkt << result; }
void IS_REGISTER_ACK::Deserialize(CPacket& pkt) { pkt >> result; }

void IS_TOKEN_VERIFY_REQ::Serialize(CPacket& pkt) const
{
    pkt.WriteU64(sid);
    pkt.WriteU64(token);
}
void IS_TOKEN_VERIFY_REQ::Deserialize(CPacket& pkt)
{
    pkt.ReadU64(sid);
    pkt.ReadU64(token);
}

void IS_TOKEN_VERIFY_RES::Serialize(CPacket& pkt) const
{
    pkt.WriteU64(sid);
    pkt << result;
    pkt << accountId;
    pkt.WriteString(loginName, WHISPER_NAME_MAX);
}
void IS_TOKEN_VERIFY_RES::Deserialize(CPacket& pkt)
{
    pkt.ReadU64(sid);
    pkt >> result;
    pkt >> accountId;
    pkt.ReadString(loginName, WHISPER_NAME_MAX);
}

void IS_PLAYER_LEAVE::Serialize(CPacket& pkt) const { pkt << accountId; pkt.WriteU64(sid); }
void IS_PLAYER_LEAVE::Deserialize(CPacket& pkt) { pkt >> accountId; pkt.ReadU64(sid); }

void IS_KICK::Serialize(CPacket& pkt) const { pkt << accountId; }
void IS_KICK::Deserialize(CPacket& pkt) { pkt >> accountId; }

void IS_ONLINE_SYNC::Serialize(CPacket& pkt) const
{
    const USHORT n = (count > MAX_ENTRIES) ? static_cast<USHORT>(MAX_ENTRIES) : count;   // accountIds[]/sids[] OOB read 방어(Deserialize와 대칭)
    pkt << n;
    for (USHORT i = 0; i < n; ++i) { pkt << accountIds[i]; pkt.WriteU64(sids[i]); }
}
void IS_ONLINE_SYNC::Deserialize(CPacket& pkt)
{
    pkt >> count;
    if (count > MAX_ENTRIES) { count = MAX_ENTRIES; }   // 신뢰경계 가드 - 변조/오프레임 시 고정배열 OOB 방지
    for (USHORT i = 0; i < count; ++i) { pkt >> accountIds[i]; pkt.ReadU64(sids[i]); }
}

// ===== 아이템 / 인벤토리 / 바닥 드랍 / 거래 =====

void ItemSlotEntry::Serialize(CPacket& pkt) const
{
    pkt << slot;
    pkt.WriteU64(uid);
    pkt << templateId;
    pkt << quantity;
}
void ItemSlotEntry::Deserialize(CPacket& pkt)
{
    pkt >> slot;
    pkt.ReadU64(uid);
    pkt >> templateId;
    pkt >> quantity;
}

void CS_ITEM_MOVE::Serialize(CPacket& pkt) const { pkt << fromSlot; pkt << toSlot; }
void CS_ITEM_MOVE::Deserialize(CPacket& pkt) { pkt >> fromSlot; pkt >> toSlot; }

void CS_ITEM_USE::Serialize(CPacket& pkt) const { pkt << slot; }
void CS_ITEM_USE::Deserialize(CPacket& pkt) { pkt >> slot; }

void CS_ITEM_EQUIP::Serialize(CPacket& pkt) const { pkt << slot; }
void CS_ITEM_EQUIP::Deserialize(CPacket& pkt) { pkt >> slot; }

void CS_ITEM_UNEQUIP::Serialize(CPacket& pkt) const { pkt << equipSlot; }
void CS_ITEM_UNEQUIP::Deserialize(CPacket& pkt) { pkt >> equipSlot; }

void CS_ITEM_DISCARD::Serialize(CPacket& pkt) const { pkt << slot; pkt << quantity; }
void CS_ITEM_DISCARD::Deserialize(CPacket& pkt) { pkt >> slot; pkt >> quantity; }

void SC_INVENTORY::Serialize(CPacket& pkt) const
{
    const BYTE n = (count > MAX_ENTRIES) ? (BYTE)MAX_ENTRIES : count;
    pkt << n;
    for (BYTE i = 0; i < n; ++i) { items[i].Serialize(pkt); }
}
void SC_INVENTORY::Deserialize(CPacket& pkt)
{
    pkt >> count;
    if (count > MAX_ENTRIES) { count = MAX_ENTRIES; }   // 신뢰경계 가드 - 고정배열 OOB 방지
    for (BYTE i = 0; i < count; ++i) { items[i].Deserialize(pkt); }
}

void SC_ITEM_UPDATE::Serialize(CPacket& pkt) const
{
    const BYTE n = (count > MAX_ENTRIES) ? (BYTE)MAX_ENTRIES : count;
    pkt << n;
    for (BYTE i = 0; i < n; ++i) { items[i].Serialize(pkt); }
}
void SC_ITEM_UPDATE::Deserialize(CPacket& pkt)
{
    pkt >> count;
    if (count > MAX_ENTRIES) { count = MAX_ENTRIES; }   // 신뢰경계 가드
    for (BYTE i = 0; i < count; ++i) { items[i].Deserialize(pkt); }
}

void SC_ITEM_RESULT::Serialize(CPacket& pkt) const { pkt << result; }
void SC_ITEM_RESULT::Deserialize(CPacket& pkt) { pkt >> result; }

void SC_STAT_UPDATE::Serialize(CPacket& pkt) const { pkt << atkPower; pkt << defPower; }
void SC_STAT_UPDATE::Deserialize(CPacket& pkt) { pkt >> atkPower; pkt >> defPower; }

void SC_GROUND_ITEM_SPAWN::Serialize(CPacket& pkt) const
{
    pkt << objectId;
    pkt << templateId;
    pkt << x;
    pkt << y;
    pkt << quantity;
}
void SC_GROUND_ITEM_SPAWN::Deserialize(CPacket& pkt)
{
    pkt >> objectId;
    pkt >> templateId;
    pkt >> x;
    pkt >> y;
    pkt >> quantity;
}

void CS_ITEM_PICKUP::Serialize(CPacket& pkt) const { pkt << objectId; }
void CS_ITEM_PICKUP::Deserialize(CPacket& pkt) { pkt >> objectId; }

void CS_TRADE_REQUEST::Serialize(CPacket& pkt) const { pkt.WriteString(targetName, WHISPER_NAME_MAX); }
void CS_TRADE_REQUEST::Deserialize(CPacket& pkt) { pkt.ReadString(targetName, WHISPER_NAME_MAX); }

void SC_TRADE_REQUEST::Serialize(CPacket& pkt) const { pkt.WriteString(requesterName, WHISPER_NAME_MAX); }
void SC_TRADE_REQUEST::Deserialize(CPacket& pkt) { pkt.ReadString(requesterName, WHISPER_NAME_MAX); }

void CS_TRADE_RESPONSE::Serialize(CPacket& pkt) const { pkt << accept; }
void CS_TRADE_RESPONSE::Deserialize(CPacket& pkt) { pkt >> accept; }

void SC_TRADE_OPEN::Serialize(CPacket& pkt) const { pkt.WriteString(partnerName, WHISPER_NAME_MAX); }
void SC_TRADE_OPEN::Deserialize(CPacket& pkt) { pkt.ReadString(partnerName, WHISPER_NAME_MAX); }

void CS_TRADE_ADD_ITEM::Serialize(CPacket& pkt) const { pkt << bagSlot; pkt << quantity; pkt << tradeSlot; }
void CS_TRADE_ADD_ITEM::Deserialize(CPacket& pkt) { pkt >> bagSlot; pkt >> quantity; pkt >> tradeSlot; }

void CS_TRADE_REMOVE_ITEM::Serialize(CPacket& pkt) const { pkt << tradeSlot; }
void CS_TRADE_REMOVE_ITEM::Deserialize(CPacket& pkt) { pkt >> tradeSlot; }

void SC_TRADE_UPDATE::Serialize(CPacket& pkt) const
{
    pkt << myAccept;
    pkt << partnerAccept;
    const BYTE mineN = (myCount > MAX_TRADE_SLOTS) ? (BYTE)MAX_TRADE_SLOTS : myCount;
    const BYTE partnerN = (partnerCount > MAX_TRADE_SLOTS) ? (BYTE)MAX_TRADE_SLOTS : partnerCount;
    pkt << mineN;
    pkt << partnerN;
    for (BYTE i = 0; i < mineN; ++i) { mine[i].Serialize(pkt); }
    for (BYTE i = 0; i < partnerN; ++i) { partner[i].Serialize(pkt); }
}
void SC_TRADE_UPDATE::Deserialize(CPacket& pkt)
{
    pkt >> myAccept;
    pkt >> partnerAccept;
    pkt >> myCount;
    pkt >> partnerCount;
    if (myCount > MAX_TRADE_SLOTS) { myCount = MAX_TRADE_SLOTS; }             // 신뢰경계 가드
    if (partnerCount > MAX_TRADE_SLOTS) { partnerCount = MAX_TRADE_SLOTS; }
    for (BYTE i = 0; i < myCount; ++i) { mine[i].Deserialize(pkt); }
    for (BYTE i = 0; i < partnerCount; ++i) { partner[i].Deserialize(pkt); }
}

void SC_TRADE_RESULT::Serialize(CPacket& pkt) const { pkt << result; }
void SC_TRADE_RESULT::Deserialize(CPacket& pkt) { pkt >> result; }
