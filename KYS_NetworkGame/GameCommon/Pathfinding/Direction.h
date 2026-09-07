#pragma once
#include "../GameServer/Types/Defines.h"   // BYTE (GameTypes.h 가 쓰지만 스스로 include 안 함 - includer 제공 관례·WalkableTable.h 와 동일)
#include "GameTypes.h"                      // EMoveDirection (LUT/양자화가 방향 enum 을 인덱스로 씀)

// 8방향 이동의 공용 방향 계층 - 위치 예측 단위 벡터 표 + 목표 방향 8방 양자화.
//   서버(몬스터 조향)·클라(자기 예측)·봇(조향)이 같은 표/같은 양자화를 써야
//   예측이 서버와 같은 곳에서 멈추고(되당김 0) 방향 선택이 일관된다.
//   (Phase C 까지는 4방 int 표가 GameTypes.h 에 있었으나, 8방 전환에서 대각 정규화(0.7071)가
//    필요해지며 짝 .cpp 를 갖는 이 모듈로 옮겼다 - 단일 정의로 3앱 비트 일치를 구조로 보장.)

// 방향별 단위 이동 벡터 (dx, dy). 인덱스 = static_cast<int>(EMoveDirection) (0..7).
//   대각(4..7)은 sqrt(2)/2 ~= 0.7071 로 정규화해 대각 이동이 직교 이동보다 빠르지 않게 한다.
//   위치 예측이 int(delta * speed * dt) 절단 산술이라 세 앱이 "같은 비트 결과"를 내야 desync 가 없다 -
//   단일 정의(이 .cpp)로 링크돼 리터럴/수식이 갈라지지 않는다. 원소는 반드시 이 표를 거쳐 곱한다
//   (LUT 를 직접 복사해 다른 수식으로 곱하면 절단치가 갈려 되당김이 난다).
extern const float MOVE_DIR_DELTA[8][2];

// 목표까지의 (dx, dy) 를 가장 가까운 8방향으로 고른다 (22.5도 경계로 양자화).
//   (0,0) 은 방향이 없어 DOWN 을 돌려주되, 호출자가 STOP 을 별도로 판단하는 것을 전제한다.
//   서버 몬스터 조향·봇 조향(SteerToward)이 공유 - 예전 지배축 4방 선택을 대각 포함 8방으로 확장.
EMoveDirection DirectionToward(int dx, int dy);
