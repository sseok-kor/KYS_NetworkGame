#pragma once

// 패킷 종류 식별자 (wire에 2바이트로 실림). 0x1000 단위로 기능별 묶음.
enum class PacketType : unsigned short
{
    // ===== 0x0000 : 시스템 / 연결 =====
    NONE    = 0x0000,                                   // 미지정, 예약
    CS_PING = 0x0001,                                   // 클라->서버 생존 확인
    SC_PONG = 0x0002,                                   // 서버->클라 응답 (왕복 시간 측정)

    // ===== 0x1000 : 로그인 =====
    CS_LOGIN           = 0x1000,                        // 클라->서버 로그인 요청
    SC_LOGIN_RESULT    = 0x1001,                        // 서버->클라 로그인 결과
    CS_REGISTER        = 0x1002,                        // 클라->로그인서버 계정 생성 요청 (id/비번)
    SC_REGISTER_RESULT = 0x1003,                        // 로그인서버->클라 계정 생성 결과 (성공/중복/거부)

    // ===== 0x2000 : 이동 / 위치 =====
    CS_MOVE           = 0x2000,                         // 클라->서버 이동 보고
    SC_MOVE_BROADCAST = 0x2001,                         // 서버->클라 다른 플레이어 이동 알림
    SC_SPAWN          = 0x2002,                         // 서버->클라 시야에 객체 등장 (전체 상태)
    SC_DESPAWN        = 0x2003,                         // 서버->클라 시야에서 객체 사라짐
    CS_PORTAL         = 0x2004,                         // 클라->서버 포탈 사용 요청
    SC_MONSTER_MOVE   = 0x2005,                         // 서버->클라 몬스터 이동 알림
    SC_MONSTER_SPAWN  = 0x2006,                         // 서버->클라 몬스터 시야 등장
    SC_MAP_CHANGE     = 0x2007,                         // 서버->클라 자기 맵/위치 통지 (포탈 후 본인에게)
    SC_CHAR_INFO      = 0x2008,                         // 서버->클라 로그인 시 자기 캐릭터 전체 초기 상태(hp/위치 등, DB 로드값, MapleStory getCharInfo 정합, 인증 응답과 분리)

    // ===== 0x3000 : 채팅 =====
    CS_CHAT           = 0x3000,                         // 클라->서버 일반 채팅
    SC_CHAT_BROADCAST = 0x3001,                         // 서버->클라 같은 맵 채팅 전달
    CS_WHISPER        = 0x3002,                         // 클라->서버 귓속말 요청
    SC_WHISPER        = 0x3003,                         // 서버->클라 귓속말 전달
    SC_WHISPER_FAIL   = 0x3004,                         // 서버->클라 귓속말 실패 통지 (오프라인/rate-limit - 낙관 self-echo 보완, B4가 실사용)

    // ===== 0x4000 : 전투 =====
    CS_SKILL   = 0x4000,                                // 클라->서버 스킬 사용
    SC_DAMAGE  = 0x4001,                                // 서버->클라 피해 발생 알림
    SC_DEATH   = 0x4002,                                // 서버->클라 사망 알림
    SC_RESPAWN = 0x4003,                                // 서버->클라 부활 알림

    // ===== 0x5000 : 로그인 서버 분리 - 클라 핸드오프 (2단계 접속) =====
    SC_SERVER_LIST   = 0x5000,                          // 로그인 서버->클라 게임 서버 목록 (인증 통과 후 push)
    CS_SERVER_SELECT = 0x5001,                          // 클라->로그인 서버 게임 서버 선택
    SC_LOGIN_TOKEN   = 0x5002,                          // 로그인 서버->클라 일회용 토큰 + 게임 서버 주소
    CS_GAME_AUTH     = 0x5003,                          // 클라->게임 서버 토큰 제출 (재접속 첫 패킷)
    SC_CHANNEL_LIST  = 0x5004,                          // 게임 서버->클라 채널 목록 (verify OK 후 push, 채널=스레드라 게임서버가 제공)
    CS_CHANNEL_SELECT= 0x5005,                          // 클라->게임 서버 채널 선택

    // ----- 캐릭터 관리 (채널 선택 후 pre-auth, GameSession 미생성 단계) -----
    SC_CHARACTER_LIST          = 0x5006,                // 게임 서버->클라 계정의 캐릭터 목록 (채널 선택 후 push)
    CS_CHARACTER_SELECT        = 0x5007,                // 클라->게임 서버 캐릭터 선택 (입장 - 이때 GameSession 생성)
    CS_CHARACTER_CREATE        = 0x5008,                // 클라->게임 서버 캐릭터 생성 (이름 + 슬롯)
    SC_CHARACTER_CREATE_RESULT = 0x5009,                // 게임 서버->클라 캐릭터 생성 결과 (성공/이름중복/거부)
    CS_CHARACTER_DELETE        = 0x500A,                // 클라->게임 서버 캐릭터 삭제 (char_id)
    SC_CHARACTER_DELETE_RESULT = 0x500B,                // 게임 서버->클라 캐릭터 삭제 결과

    // ----- 인게임 채널 변경 (in-place handoff, 같은 캐릭터, 같은 TCP) -----
    CS_CHANNEL_CHANGE          = 0x500C,                // 클라->게임 서버 인게임 채널 변경 요청 (channelId)
    SC_CHANNEL_CHANGE_RESULT   = 0x500D,                // 게임 서버->클라 채널 변경 결과 (OK면 클라 옛 채널 시야 reset)
    CS_CHANNEL_LIST_REQUEST    = 0x500E,                // 클라->게임 서버 인게임 채널변경 picker 인구 재조회 (payload 없음. 응답 = SC_CHANNEL_LIST 재사용)
    SC_ENTER_WORLD             = 0x500F,                // 게임 서버->클라 입장 완료 (캐릭터 선택 admission 후. obfuscation admission-ack arm 트리거 - B2가 SC_LOGIN_RESULT 대체)

    // ===== 0x6000 : 인터서버 (LoginServer <-> ServerApp 전용, 클라엔 안 감) =====
    IS_REGISTER         = 0x6000,                       // 게임 서버->로그인 서버 링크 등록 (공유 비밀 + 게임 listen 주소)
    IS_REGISTER_ACK     = 0x6001,                       // 로그인 서버->게임 서버 등록 결과
    IS_TOKEN_VERIFY_REQ = 0x6002,                       // 게임 서버->로그인 서버 토큰 검증 요청
    IS_TOKEN_VERIFY_RES = 0x6003,                       // 로그인 서버->게임 서버 토큰 검증 응답 (계정 식별 운반)
    IS_PLAYER_LEAVE     = 0x6004,                       // 게임 서버->로그인 서버 접속 종료 통지 (온라인 표 해제)
    IS_KICK             = 0x6005,                       // 로그인 서버->게임 서버 중복 로그인 축출 명령
    IS_ONLINE_SYNC      = 0x6006,                       // 게임 서버->로그인 서버 권위 online 집합 스냅샷 (주기적+재연결 - online ghost 수렴)

    // ===== 0x7000 : 아이템 / 인벤토리 / 바닥 드랍 / 거래 =====
    CS_ITEM_MOVE    = 0x7000,                           // 클라->서버 가방 슬롯 이동 (스왑/스택 합류)
    CS_ITEM_USE     = 0x7001,                           // 클라->서버 소모품 사용 (HP 포션 등)
    CS_ITEM_EQUIP   = 0x7002,                           // 클라->서버 장착 (가방 -> 무기/방어구 슬롯)
    CS_ITEM_UNEQUIP = 0x7003,                           // 클라->서버 장착 해제 (장착 슬롯 -> 가방)
    CS_ITEM_DISCARD = 0x7004,                           // 클라->서버 버리기 (가방 -> 자기 위치 바닥 드랍)
    SC_INVENTORY    = 0x7005,                           // 서버->클라 인벤토리 전체 스냅샷 (입장/거래 완료 후)
    SC_ITEM_UPDATE  = 0x7006,                           // 서버->클라 바뀐 슬롯만 통지 (획득/사용/이동/장착 결과)
    SC_ITEM_RESULT  = 0x7007,                           // 서버->클라 아이템 조작 실패 사유 (꽉 참/거리/소유권 등)
    SC_STAT_UPDATE  = 0x7008,                           // 서버->클라 장착 반영 후 자기 공격력/방어력 통지

    SC_GROUND_ITEM_SPAWN = 0x7010,                      // 서버->클라 바닥 아이템 시야 등장 (despawn 은 SC_DESPAWN 재사용)
    CS_ITEM_PICKUP       = 0x7012,                      // 클라->서버 바닥 아이템 줍기 요청

    CS_TRADE_REQUEST     = 0x7020,                      // 클라->서버 거래 요청 (대상 이름, 같은 맵만)
    SC_TRADE_REQUEST     = 0x7021,                      // 서버->클라 상대에게 거래 요청 통지
    CS_TRADE_RESPONSE    = 0x7022,                      // 클라->서버 거래 요청 수락/거절
    SC_TRADE_OPEN        = 0x7023,                      // 서버->클라 양측 거래창 열기
    CS_TRADE_ADD_ITEM    = 0x7024,                      // 클라->서버 거래창에 아이템 올리기
    CS_TRADE_REMOVE_ITEM = 0x7025,                      // 클라->서버 거래창에서 아이템 내리기
    CS_TRADE_ACCEPT      = 0x7026,                      // 클라->서버 거래 수락 (payload 없음. 오퍼 변경 시 서버가 양측 수락을 강제 리셋)
    CS_TRADE_CANCEL      = 0x7027,                      // 클라->서버 거래 취소 (payload 없음)
    SC_TRADE_UPDATE      = 0x7028,                      // 서버->클라 거래창 상태 (양측 오퍼 목록 + 양측 수락 여부)
    SC_TRADE_RESULT      = 0x7029                       // 서버->클라 거래 종결 통지 (완료/취소/사유)
};
