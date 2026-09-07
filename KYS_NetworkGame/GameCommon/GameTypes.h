#pragma once

using PlayerId = UINT32;   // 플레이어 식별 번호 (로그인마다 발급, wire에 실림)

// GameObject가 플레이어/몬스터/바닥 아이템 중 무엇인지 구분 (spawn 패킷에 1바이트로 실림)
enum class EObjectType : BYTE
{
    PLAYER      = 0,
    MONSTER     = 1,
    GROUND_ITEM = 2,   // 바닥에 떨어진 아이템 (관측 전용 - 시야 broadcast 대상, 수신/저장/타겟 필터엔 안 잡힘)
};

// 이동 중인지 멈췄는지
enum class EMoveState : BYTE
{
    STOP  = 0,
    START = 1,
};

// 8방향 (상하좌우 4방 + 대각 4방). 위치 예측 표 MOVE_DIR_DELTA(Direction.h) 의 인덱스이자 wire 에 실리는 값.
//   4방(0..3)은 값 보존(기존 wire 호환), 대각(4..7)은 Phase D 8방 전환에서 추가.
enum class EMoveDirection : BYTE
{
    UP         = 0,
    DOWN       = 1,
    LEFT       = 2,
    RIGHT      = 3,
    UP_LEFT    = 4,
    UP_RIGHT   = 5,
    DOWN_LEFT  = 6,
    DOWN_RIGHT = 7,
    COUNT      = 8,   // 방향 개수 (수신 범위 검사용 - wire 엔 안 실림)
};

// 방향별 단위 이동 벡터(MOVE_DIR_DELTA)와 8방 양자화(DirectionToward)는 짝 .cpp 를 갖는 Direction.h 로 옮겼다
//   (8방 대각 정규화 0.7071 이 float 라 단일 정의로 3앱 비트 일치를 보장하기 위함).

// 몬스터 AI 상태 (Monster::Update의 분기 기준). wire가 아닌 서버 내부 상태지만 이동 enum들과 묶어 여기 둠.
enum class EMonsterState : BYTE
{
    IDLE   = 0,   // 가만히 있음
    PATROL = 1,   // 집 주변을 어슬렁댐
    CHASE  = 2,   // 적을 발견해 추격
    ATTACK = 3,   // 사거리 안이라 공격
    DEAD   = 4,   // 죽음, 시신이 잠깐 남았다 사라짐
};

// 몬스터 종류 (클라는 종류로 그림을 고르고, 서버는 종류로 능력치와 AI를 찾음).
//   개체 번호(m_id)와 별개인 "종류" - 같은 종류 몬스터가 여럿일 수 있음. 플레이어/몬스터 구분(EObjectType)과도 별개.
enum class EMonsterType : BYTE
{
    GOBLIN = 0,
    ORC    = 1,
    OGRE   = 2,
    SLIME  = 3,
    FENRIR = 4,   // 보스
    WOLF   = 5,   // 9맵 확장 - 중급 야수
    BEAR   = 6,   // 9맵 확장 - 상급 야수
    TROLL  = 7,   // 9맵 확장 - 상급 잡몹
    GOLEM  = 8,   // 9맵 확장 - 최상급 보스급
    COUNT  = 9,   // 종류 개수 (능력치 표 크기 확인용, wire엔 안 실림)
};
