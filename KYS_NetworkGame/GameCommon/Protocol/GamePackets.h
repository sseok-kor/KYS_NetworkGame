#pragma once
#include "CPacket.h"
#include "GameTypes.h"
#include "GameDefines.h"
#include "../GameServer/Types/Defines.h"

// 클라/서버가 주고받는 패킷 구조체 모음. 각 struct는 PacketType의 한 종류에 대응.
//   Serialize = 필드 -> 패킷, Deserialize = 패킷 -> 필드 (둘은 같은 순서여야 함).

// ===== 시스템 / 연결 =====

struct CS_PING
{
    UINT32 clientTimeMs;   // 클라가 보낸 시각 (응답으로 왕복 시간 계산)
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_PONG
{
    UINT32 clientTimeMs;   // CS_PING 값을 그대로 돌려줌
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

// ===== 로그인 =====

struct LoginReq
{
    static const int ID_MAX = 16;

    wchar_t id[ID_MAX];        // 고정 길이 ID (단순화)
    wchar_t pw[PASSWORD_MAX];  // 비밀번호 평문 (서버가 받아 PBKDF2 해시 후 저장 해시와 대조 - 클라측 사전 해시 안티패턴 회피)

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_REGISTER   // 계정 생성 요청 (로그인 화면 "계정 만들기"). LoginServer 가 PBKDF2 해시 후 accounts INSERT
{
    wchar_t id[LoginReq::ID_MAX];   // 새 계정 로그인 이름 (accounts UNIQUE 키)
    wchar_t pw[PASSWORD_MAX];       // 새 계정 비밀번호 평문 (서버가 해시)

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

enum class ECreateAccountResult : BYTE   // SC_REGISTER_RESULT.result 값
{
    OK       = 0,   // 생성 성공 (그 계정으로 로그인 가능)
    DUP_ID   = 1,   // 이미 있는 id (accounts UNIQUE 충돌)
    INVALID  = 2,   // 입력 거부 (빈 값 / 길이 초과 / 잘못된 문자)
    DB_ERROR = 3    // DB 오류 (INSERT 실패)
};

struct SC_REGISTER_RESULT   // 계정 생성 결과
{
    BYTE result;   // ECreateAccountResult

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

enum class ELoginResult : BYTE   // SC_LOGIN_RESULT.result 값
{
    FAIL        = 0,   // 인증 실패
    OK          = 1,   // 성공 (현행 wire 미사용 - 성공 통지는 SC_ENTER_WORLD)
    SERVER_FULL = 2    // 인원 상한 초과로 거부
};

struct SC_LOGIN_RESULT
{
    BYTE result;   // ELoginResult - 현행 wire 는 거부 전용(FAIL/SERVER_FULL). 성공 통지는 SC_ENTER_WORLD 가 담당(OK=1 은 wire 에 실리지 않음)
    int  chId;     // 잔존 필드 - 송신 2곳(LoginServer 인증 실패/게임서버 거부) 모두 -1 고정, 수신측 판독 0곳

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);   // 클라 수신용 (서버 Serialize와 대칭)
};

// ===== 이동 / 위치 / 스폰 =====

struct CS_MOVE
{
    EMoveState     moveState;   // 멈춤/이동
    EMoveDirection direction;   // 8방향 (0..7)
    int            x;           // 클라가 보고하는 현재 위치
    int            y;
    UINT32         clientSeq;   // 클라 시퀀스 번호 (서버가 브로드캐스트에 되실어 줌)

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_MOVE_BROADCAST   // 다른 플레이어의 이동을 주변에 알림
{
    PlayerId       playerId;
    EMoveState     moveState;
    EMoveDirection direction;
    int            x;
    int            y;
    UINT32         lastSeq;     // 그 플레이어가 마지막으로 보낸 시퀀스 번호

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_SPAWN   // 시야에 플레이어가 들어옴 - 전체 상태를 한 번에 전달 (캐릭터 이름 동봉 - 클라 이름표 즉시 표시)
{
    PlayerId       playerId;
    EMoveState     moveState;
    EMoveDirection direction;
    int            x;
    int            y;
    int            hp;          // 체력바 초기화용 현재 HP
    int            maxHp;
    wchar_t        name[WHISPER_NAME_MAX];   // 캐릭터 이름 (wire = 길이2B + 문자2BxN 가변)

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_DESPAWN   // 시야에서 객체가 사라짐 (이탈 또는 파괴) - id만 전달
{
    PlayerId playerId;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_PORTAL   // 포탈 사용 요청. 클라는 어느 포탈인지만 보내고, 목적지는 서버가 정함
{
    int portalId;   // 포탈 식별자 (서버가 portalId로 목적지/근접 검증)
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_MONSTER_MOVE   // 몬스터 이동 알림. SC_MOVE_BROADCAST와 비슷하나 lastSeq 없음(몬스터는 서버가 권위)
{
    UINT32         monsterId;   // 몬스터 개체 번호 (sid 아님)
    EMoveState     moveState;
    EMoveDirection direction;
    int            x;
    int            y;

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_MONSTER_SPAWN   // 시야에 몬스터가 들어옴 - 전체 상태 (SC_SPAWN의 몬스터판)
{
    UINT32         monsterId;
    EMoveState     moveState;
    EMoveDirection direction;
    int            x;
    int            y;
    int            hp;
    int            maxHp;
    BYTE           objectType;  // 플레이어/몬스터 구분 (EObjectType)
    BYTE           monsterType; // 몬스터 종류 (EMonsterType - 클라가 종류별 그림 선택)
    int            attackRange; // 이 몬스터의 공격 사거리 px (클라가 공격범위 표시에 사용 - 종류별 서버 스탯)
    int            moveSpeed;   // 이동 속도 px/s (클라가 SC_MONSTER_MOVE 사이 위치 예측에 사용 - 스폰 스냅샷 1회 전달로 종류별 속도 지원)
    BYTE           isBoss;      // 보스 여부 (0/1 - 클라 렌더 크기/강조. 종류 하드코딩 판정 대체)

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_MAP_CHANGE   // 자기 자신의 맵/위치 통지 (포탈 후 본인에게) - 클라가 자기 아바타를 정확히 배치 (로그인은 SC_CHAR_INFO가 hp 포함 전체 통지)
{
    PlayerId playerId;   // 내 플레이어 id (자기 식별 - broadcast self-echo / 자기 피격 구분, 산업표준 "클라는 자기 id를 안다")
    int      mapId;      // 내가 지금 있는 맵
    int      x;          // 서버 권위 위치 (포탈 도착 좌표)
    int      y;

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_CHAR_INFO   // 로그인 완료 시 자기(self) 캐릭터 전체 초기 상태 통지 (DB 로드값) - 클라가 자기 hp/maxHp(기본값 max)를 정확값으로 보정. MapleStory getCharInfo 정합 (인증 응답 SC_LOGIN_RESULT와 분리, self 전용)
{
    PlayerId playerId;   // 내 플레이어 id (자기 식별)
    int      mapId;      // 로그인 스폰 맵
    int      x;          // 서버 권위 스폰 위치
    int      y;
    int      hp;         // DB 로드 현재 hp (로그인 시 자기 hp를 아는 유일 경로 - SC_MAP_CHANGE엔 hp가 없어 클라가 max로 표시하던 갭 해소)
    int      maxHp;      // DB 로드 최대 hp
    int      mp;         // DB 로드 mp (클라 현재 미사용, full self-state 완비)

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

// ===== 전투 =====

struct CS_SKILL   // 스킬 사용 요청. 좌표/대상은 안 보냄 - 서버가 시전자 위치로 판정 (skillId 0=평타)
{
    UINT32 skillId;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_DAMAGE   // 피해 발생 알림 (데미지 숫자 + 체력바 동기)
{
    UINT32 atkId;        // 공격자 개체 번호
    UINT32 targetId;     // 피격자 개체 번호
    int    damage;       // 실제 적용된 피해
    int    remainingHp;  // 피격 후 남은 HP

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_DEATH   // 사망 알림 (사망 모션/시신 처리용 - SC_DESPAWN과 구분)
{
    UINT32 targetId;     // 죽은 객체 번호
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_RESPAWN   // 부활 알림 (부활 위치 + 회복된 HP)
{
    PlayerId playerId;
    int      x;
    int      y;
    int      hp;
    int      maxHp;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

// 전투 패킷 조립 헬퍼 - SC_DAMAGE/SC_DEATH 를 프레이밍까지 완성(Begin+본체+End) 한 곳에 응집.
//   스테이징 경로(Map::StageDamage/StageDeath)와 즉시 배칭 경로(Channel::ApplySkillHit)가 공용 - 조립만 공통화, 전송 경로는 각자 유지.
//   atkId/targetId 는 wire id(player=m_playerId, monster 등=통합 m_id).
[[nodiscard]] bool BuildDamagePacket(KYS::GAMECOMMON::PROTOCOL::CPacket& out, UINT32 atkId, UINT32 targetId, int damage, int remainingHp);
[[nodiscard]] bool BuildDeathPacket(KYS::GAMECOMMON::PROTOCOL::CPacket& out, UINT32 targetId);

// ===== 채팅 =====

struct CS_CHAT   // 일반 채팅 입력
{
    wchar_t message[CHAT_MSG_MAX];   // 메시지 본문 (고정 배열)

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_CHAT_BROADCAST   // 같은 맵 전원에게 채팅 전달
{
    PlayerId playerId;                      // 발화자 번호 (시야 안이면 머리 위 말풍선용)
    wchar_t  senderName[WHISPER_NAME_MAX];  // 발신자 이름 (시야 밖 수신자도 채팅창에 이름 표시하려고 직접 실음)
    wchar_t  message[CHAT_MSG_MAX];

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_WHISPER   // 귓속말 요청 (대상 이름으로 라우팅)
{
    wchar_t targetName[WHISPER_NAME_MAX];   // 받는 사람 캐릭터 이름
    wchar_t message[CHAT_MSG_MAX];

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_WHISPER   // 귓속말 전달 (보낸 사람 이름 표기)
{
    wchar_t senderName[WHISPER_NAME_MAX];   // 보낸 사람 이름
    wchar_t message[CHAT_MSG_MAX];

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

enum class EWhisperFail : BYTE   // SC_WHISPER_FAIL.reason 값
{
    OFFLINE      = 0,   // 대상이 접속 중이 아님 (ResolveName 0)
    RATE_LIMITED = 1    // 발신자 채팅 쿨다운 (CanChat 게이트 실패)
};

struct SC_WHISPER_FAIL   // 서버->클라 귓속말 실패 통지 (오프라인/rate-limit - 낙관 self-echo 보완, B4가 실사용)
{
    wchar_t targetName[WHISPER_NAME_MAX];   // 귓속말을 보내려던 대상 이름
    BYTE    reason;                         // EWhisperFail

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

// ===== 로그인 서버 분리 - 클라 핸드오프 (2단계 접속) =====

struct ServerEntry   // 게임 서버 목록의 한 항목 (SC_SERVER_LIST 안에 들어감)
{
    BYTE    serverId;                       // 서버 번호 (CS_SERVER_SELECT로 되돌려 보냄)
    UINT32  ip;                             // 게임 서버 IPv4 (호스트 정수)
    USHORT  port;                           // 게임 서버 포트
    USHORT  population;                      // 현재 접속 인원 (혼잡도 표시용)
    wchar_t name[WHISPER_NAME_MAX];         // 서버 이름

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_SERVER_LIST   // 로그인 서버->클라 게임 서버 목록 (인증 통과 후 push)
{
    BYTE        count;                      // 실제 서버 수 (<= MAX_SERVER_LIST)
    ServerEntry servers[MAX_SERVER_LIST];   // 고정 배열, 앞 count개만 유효

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_SERVER_SELECT   // 클라->로그인 서버 게임 서버 선택 (봇은 첫 번째 자동 선택)
{
    BYTE serverId;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_LOGIN_TOKEN   // 로그인 서버->클라 일회용 토큰 + 게임 서버 주소 (이걸 받으면 로그인 서버 끊고 게임 서버에 재접속)
{
    UINT64 token;        // 일회용 토큰 (게임 서버에 제출, 한 번 쓰면 소멸)
    UINT32 serverIp;     // 재접속할 게임 서버 IPv4 (호스트 정수, FFXIV 교훈=주소를 클라에 박지 말고 그때그때 내려줌)
    USHORT serverPort;   // 게임 서버 클라 listen 포트

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_GAME_AUTH   // 클라->게임 서버 토큰 제출 (재접속 첫 패킷, 게임 서버가 로그인 서버에 검증 질의)
{
    UINT64 token;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_CHANNEL_LIST   // 게임 서버->클라 채널 목록 (verify OK 후 push). 가변(채널 수). 채널=스레드라 게임 서버가 인구 직접 보유
{
    struct Entry
    {
        BYTE channelId;     // 채널 번호 (0..채널수-1)
        int  playerCount;   // 현재 인구
        int  maxPlayers;    // 채널 정원 (MAX_PLAYERS_PER_CHANNEL)
    };
    BYTE  channelCount;                  // 유효 entry 수 (<= MAX_CHANNEL_COUNT)
    Entry entries[MAX_CHANNEL_COUNT];

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_CHANNEL_SELECT   // 클라->게임 서버 채널 선택 (SC_CHANNEL_LIST 보고 유저가 고름)
{
    BYTE channelId;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

// ----- 캐릭터 관리 (채널 선택 후 pre-auth, GameSession 미생성 단계) -----

struct CharSummary   // SC_CHARACTER_LIST 안의 한 캐릭터 (목록 표시용 최소 필드)
{
    UINT32  charId;                     // 캐릭터 고유 번호 (선택/삭제 때 되돌려 보냄)
    BYTE    slotId;                     // 슬롯 번호 (0..MAX_CHAR_SLOTS-1)
    wchar_t name[WHISPER_NAME_MAX];     // 캐릭터 이름
    int     mapId;                      // 마지막 맵
    int     hp;
    int     maxHp;

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_CHARACTER_LIST   // 게임 서버->클라 계정의 캐릭터 목록 (채널 선택 후 push). 가변(0..MAX_CHAR_SLOTS)
{
    BYTE        count;                  // 유효 캐릭터 수 (<= MAX_CHAR_SLOTS)
    CharSummary chars[MAX_CHAR_SLOTS];

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_CHARACTER_SELECT   // 클라->게임 서버 캐릭터 선택 (입장 - 이때 GameSession 생성)
{
    UINT32 charId;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_CHARACTER_CREATE   // 클라->게임 서버 캐릭터 생성 (이름 + 슬롯)
{
    wchar_t name[WHISPER_NAME_MAX];
    BYTE    slotId;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

enum class ECharCreateResult : BYTE   // 캐릭터 생성 결과 코드
{
    OK         = 0,
    DUP_NAME   = 1,   // 이름 중복 (char_name UNIQUE)
    INVALID    = 2,   // 이름 길이/문자 또는 슬롯 범위 위반
    SLOT_TAKEN = 3,   // 그 슬롯에 이미 캐릭터 있음
    DB_ERROR   = 4
};

struct SC_CHARACTER_CREATE_RESULT   // 게임 서버->클라 캐릭터 생성 결과
{
    BYTE   result;    // ECharCreateResult
    UINT32 charId;    // 성공 시 새 캐릭터 번호 (실패 시 0)
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_CHARACTER_DELETE   // 클라->게임 서버 캐릭터 삭제
{
    UINT32 charId;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

enum class ECharDeleteResult : BYTE   // 캐릭터 삭제 결과 코드
{
    OK        = 0,
    NOT_OWNED = 1,   // 그 char_id가 이 계정 소유가 아님 (또는 없음)
    DB_ERROR  = 2
};

struct SC_CHARACTER_DELETE_RESULT   // 게임 서버->클라 캐릭터 삭제 결과
{
    BYTE result;    // ECharDeleteResult
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

// ----- 인게임 채널 변경 (in-place handoff, 같은 캐릭터, 같은 TCP, GameSession 유지) -----

struct CS_CHANNEL_CHANGE   // 클라->게임 서버 인게임 채널 변경 요청
{
    BYTE channelId;    // 이동할 채널 번호
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

enum class EChannelChangeResult : BYTE   // 채널 변경 결과 코드
{
    OK          = 0,
    FULL        = 1,   // 대상 채널 인구 cap 초과
    INVALID     = 2,   // 채널 번호 범위 밖
    SAME        = 3,   // 이미 그 채널에 있음
    NOT_IN_GAME = 4,   // 인게임(캐릭터 입장) 상태가 아님
    RATE_LIMITED = 5   // 채널 변경 쿨다운 (CHANNEL_CHANGE_MIN_INTERVAL_MS 내 재요청, B5가 실사용)
};

struct SC_CHANNEL_CHANGE_RESULT   // 게임 서버->클라 채널 변경 결과 (OK면 클라가 옛 채널 시야 GameState reset)
{
    BYTE result;       // EChannelChangeResult
    BYTE channelId;    // OK 일 때 이동한 채널 (실패 시 현재 채널)
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_ENTER_WORLD   // 게임 서버->클라 입장 완료 신호 (payload 없음 - opcode 도착 자체가 admission-ack. B2가 arm 트리거로 SC_LOGIN_RESULT 대체)
{
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

// ===== 인터서버 (LoginServer <-> ServerApp 전용, 클라엔 안 감) =====

struct IS_REGISTER   // 게임 서버->로그인 서버 링크 등록 (링크 첫 패킷)
{
    USHORT serverId;     // 게임 서버 번호 (서버 목록의 그 번호)
    UINT32 authKeyHash;  // 공유 비밀 해시 (가짜 게임 서버 등록 차단)
    UINT32 listenIp;     // 게임 서버가 클라를 받는 IPv4 (SC_LOGIN_TOKEN redirect용)
    USHORT listenPort;   // 게임 서버 클라 listen 포트

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct IS_REGISTER_ACK   // 로그인 서버->게임 서버 등록 결과 (0=거부 후 close, 1=수락)
{
    BYTE result;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct IS_TOKEN_VERIFY_REQ   // 게임 서버->로그인 서버 토큰 검증 요청
{
    UINT64 sid;     // 게임 세션 sid (응답을 대기 중인 세션으로 라우팅하는 상관 키)
    UINT64 token;   // 클라가 제출한 토큰

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

enum class EVerifyResult : BYTE   // IS_TOKEN_VERIFY_RES.result 값
{
    // 0 은 어느 값도 아니다. 이 리포는 0 을 "무효" 로 예약해 두고 있고(sid 0 = 존재 불가, token 0 = 배제,
    //   accountId 0 = 인증 실패, quantity 0 = 거부), 읽기 가드가 바이트 부족 시 채우는 값도 0 이다.
    //   OK 를 0 에 두면 절단된 응답이 "검증 통과" 로 읽힌다 - 응답 8바이트만 와도 sid 는 완독되고
    //   result 만 0 이 되어, 유일한 방벽인 sid 상관검사(LoginLinkThread)가 통과한다. 그래서 1 부터 매긴다.
    OK             = 1,   // 토큰 유효, 계정 식별 동봉
    NOT_FOUND      = 2,   // 토큰 없음 (위조 또는 이미 소비)
    EXPIRED        = 3,   // 토큰 만료 (TTL 초과)
    ALREADY_ONLINE = 4    // 같은 계정 이미 접속 중 (중복 로그인)
};

struct IS_TOKEN_VERIFY_RES   // 로그인 서버->게임 서버 토큰 검증 응답 (계정 식별 운반)
{
    UINT64  sid;                         // 요청의 sid 그대로 (어느 세션의 응답인지)
    BYTE    result;                      // EVerifyResult
    UINT32  accountId;                   // 인증된 계정 번호 (result=OK일 때만 유효)
    wchar_t loginName[WHISPER_NAME_MAX]; // 계정 로그인 이름 (로그인 서버 발급 응답에 동봉) - 캐릭터 로드 키는 charId+accountId 이지 loginName 이 아니다

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct IS_PLAYER_LEAVE   // 게임 서버->로그인 서버 접속 종료 통지 (온라인 표에서 계정 해제)
{
    UINT32 accountId;
    UINT64 sid;          // 떠나는 게임 세션 sid - 로그인 서버는 online[accountId]==sid 일 때만 해제(같은 계정 재접속이 sid 갈아탄 뒤 늦게 도는 leave의 ghost 봉인)

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct IS_KICK   // 로그인 서버->게임 서버 중복 로그인 축출 명령 (기존 세션 종료)
{
    UINT32 accountId;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct IS_ONLINE_SYNC   // 게임 서버->로그인 서버 권위 online 집합 스냅샷 청크 (online ghost 수렴 - 로그인 서버가 refresh, grace 초과 미refresh 엔트리는 sweep)
{
    static const int MAX_ENTRIES = 500;   // 한 프레임 최대 엔트리 (12B/엔트리 x 500 = 6000B < RECV_BUFFER_SIZE. 초과분은 다음 청크로)
    USHORT count;
    UINT32 accountIds[MAX_ENTRIES];
    UINT64 sids[MAX_ENTRIES];
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

// ===== 0x7000 : 아이템 / 인벤토리 / 바닥 드랍 / 거래 =====

// 인벤토리/거래창 한 칸 내용 (SC_INVENTORY / SC_ITEM_UPDATE / SC_TRADE_UPDATE 공용 원소).
//   uid 는 64비트라 WriteU64/ReadU64 로 직렬화 (SerializationBuffer 는 32비트까지만 native).
struct ItemSlotEntry
{
    BYTE   slot;         // 가방 0..23, 무기 100, 방어구 101 (거래창에선 0..8 거래 칸 번호)
    UINT64 uid;          // 아이템 고유 번호 (0 = 빈 칸 - 클라는 그 칸을 비움)
    int    templateId;   // 아이템 종류 번호 (양쪽이 Data/items.csv 에서 이름/능력치 조회)
    int    quantity;     // 수량 (장비 = 1)

    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_ITEM_MOVE   // 클라->서버 가방 슬롯 이동 (스왑 또는 같은 종류 스택 합류)
{
    BYTE fromSlot;
    BYTE toSlot;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_ITEM_USE   // 클라->서버 소모품 사용 (HP 포션 등 - 서버가 종류/수량 검증 후 효과 적용)
{
    BYTE slot;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_ITEM_EQUIP   // 클라->서버 장착 (가방 슬롯 -> 아이템 종류에 맞는 장착 슬롯)
{
    BYTE slot;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_ITEM_UNEQUIP   // 클라->서버 장착 해제 (장착 슬롯 -> 가방 빈 칸)
{
    BYTE equipSlot;      // EQUIP_SLOT_WEAPON(100) 또는 EQUIP_SLOT_ARMOR(101)
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_ITEM_DISCARD   // 클라->서버 버리기 (가방에서 빼서 자기 위치에 바닥 드랍 생성)
{
    BYTE slot;
    int  quantity;       // 스택 일부만 버리기 허용 (전량이면 슬롯 비움)
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_INVENTORY   // 서버->클라 인벤토리 전체 스냅샷 (입장 시 + 거래 완료 후 재전송)
{
    static const int MAX_ENTRIES = MAX_INVENTORY_SLOTS + 2;   // 가방 24 + 무기 + 방어구
    BYTE          count;
    ItemSlotEntry items[MAX_ENTRIES];
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_ITEM_UPDATE   // 서버->클라 바뀐 슬롯만 통지 (획득 1, 이동/장착 2, 사용/버림 1)
{
    static const int MAX_ENTRIES = 4;   // 한 조작이 바꾸는 슬롯 수 상한 (거래 완료는 SC_INVENTORY 로 전체 재전송)
    BYTE          count;
    ItemSlotEntry items[MAX_ENTRIES];
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

enum class EItemResult : BYTE   // SC_ITEM_RESULT.result 값
{
    OK              = 0,
    INV_FULL        = 1,   // 인벤토리 꽉 참 (줍기/거래 수령 불가)
    BAD_SLOT        = 2,   // 슬롯 번호가 범위 밖이거나 빈 칸
    TOO_FAR         = 3,   // 줍기 거리 초과
    NOT_OWNER       = 4,   // 드랍 소유권 보호 시간 중 (기여 순위 밖)
    NOT_FOUND       = 5,   // 대상 바닥 아이템이 이미 사라짐
    WRONG_TYPE      = 6,   // 장착/사용 불가 종류 (포션을 장착하려는 등)
    LOCKED_IN_TRADE = 7,   // 거래 진행 중이라 인벤 조작 불가 (거래 종결 후 재시도)
};

struct SC_ITEM_RESULT   // 서버->클라 아이템 조작 실패 통지 (성공은 SC_ITEM_UPDATE 가 대신함)
{
    BYTE result;         // EItemResult
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_STAT_UPDATE   // 서버->클라 장착 변화 반영 후 자기 공격력/방어력 통지
{
    int atkPower;        // 기본 공격력 + 장착 무기 합산
    int defPower;        // 장착 방어구 합산 (수신 데미지에서 감산)
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_GROUND_ITEM_SPAWN   // 서버->클라 바닥 아이템 시야 등장 (payload 20B - 스테이징 64B 캡 안)
{
    UINT32 objectId;     // 바닥 아이템의 맵 개체 번호 (줍기 요청 키. 아이템 uid 아님 - uid 는 주운 뒤 발급)
    int    templateId;   // 종류 (클라는 items.csv 에서 이름/아이콘 조회)
    int    x;
    int    y;
    int    quantity;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_ITEM_PICKUP   // 클라->서버 바닥 아이템 줍기 요청 (서버가 거리/소유권/공간 검증)
{
    UINT32 objectId;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_TRADE_REQUEST   // 클라->서버 거래 요청 (/trade 이름 - 같은 맵 상대만)
{
    wchar_t targetName[WHISPER_NAME_MAX];
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_TRADE_REQUEST   // 서버->클라 상대에게 거래 요청 통지
{
    wchar_t requesterName[WHISPER_NAME_MAX];
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_TRADE_RESPONSE   // 클라->서버 받은 거래 요청에 대한 응답
{
    BYTE accept;         // 1 = 수락, 0 = 거절
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_TRADE_OPEN   // 서버->클라 거래 성립 - 양측 거래창 열기
{
    wchar_t partnerName[WHISPER_NAME_MAX];
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_TRADE_ADD_ITEM   // 클라->서버 거래창에 아이템 올리기 (올리면 양측 수락이 풀림)
{
    BYTE bagSlot;        // 내 가방의 어느 칸에서
    int  quantity;       // 몇 개를 (스택 일부 가능)
    BYTE tradeSlot;      // 거래창 몇 번 칸에 (0..MAX_TRADE_SLOTS-1)
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct CS_TRADE_REMOVE_ITEM   // 클라->서버 거래창에서 아이템 내리기 (양측 수락이 풀림)
{
    BYTE tradeSlot;
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

struct SC_TRADE_UPDATE   // 서버->클라 거래창 현재 상태 (오퍼 변경/수락 때마다 양측에 재전송)
{
    BYTE          myAccept;                        // 내 수락 여부 (서버가 리셋했으면 0으로 옴)
    BYTE          partnerAccept;                   // 상대 수락 여부
    BYTE          myCount;                         // 내가 올린 칸 수
    BYTE          partnerCount;                    // 상대가 올린 칸 수
    ItemSlotEntry mine[MAX_TRADE_SLOTS];           // 내 오퍼 (slot = 거래 칸 번호)
    ItemSlotEntry partner[MAX_TRADE_SLOTS];        // 상대 오퍼
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};

enum class ETradeResult : BYTE   // SC_TRADE_RESULT.result 값
{
    COMPLETE     = 0,   // 거래 완료 (양측 인벤 교환 확정)
    CANCELLED    = 1,   // 상대 또는 내가 취소/거절
    PARTNER_LEFT = 2,   // 상대가 나감/죽음/맵 이탈
    INV_FULL     = 3,   // 어느 쪽 인벤에 상대 오퍼가 안 들어감 (수락 리셋 후 계속)
    TOO_FAR      = 4,   // 같은 맵 조건 위반 (요청/커밋 시점 재검증)
    BUSY         = 5,   // 상대가 이미 다른 거래 중
    NOT_FOUND    = 6,   // 그런 이름의 상대가 이 맵에 없음
};

struct SC_TRADE_RESULT   // 서버->클라 거래 종결/실패 통지
{
    BYTE result;         // ETradeResult
    void Serialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const;
    void Deserialize(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt);
};
