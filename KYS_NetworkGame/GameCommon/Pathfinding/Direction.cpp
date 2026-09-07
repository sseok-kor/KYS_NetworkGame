#include "pch_gamecommon.h"
#include "Direction.h"

// 4방(0..3)은 값 보존(기존 wire 호환), 대각(4..7)은 ±0.7071 정규화.
//   순서: UP_LEFT / UP_RIGHT / DOWN_LEFT / DOWN_RIGHT (EMoveDirection 4..7 과 1:1).
const float MOVE_DIR_DELTA[8][2] =
{
    {  0.0f,    -1.0f    },   // UP
    {  0.0f,     1.0f    },   // DOWN
    { -1.0f,     0.0f    },   // LEFT
    {  1.0f,     0.0f    },   // RIGHT
    { -0.7071f, -0.7071f },   // UP_LEFT
    {  0.7071f, -0.7071f },   // UP_RIGHT
    { -0.7071f,  0.7071f },   // DOWN_LEFT
    {  0.7071f,  0.7071f },   // DOWN_RIGHT
};

EMoveDirection DirectionToward(int dx, int dy)
{
    if (dx == 0 && dy == 0) { return EMoveDirection::DOWN; }   // 방향 없음 - 무해 기본값(STOP 은 호출자 몫)

    const long long adx = (dx < 0) ? -static_cast<long long>(dx) : dx;
    const long long ady = (dy < 0) ? -static_cast<long long>(dy) : dy;

    // 대각 대역: 작은 성분이 큰 성분의 tan(22.5도) ~= 0.4142 배 이상이면 대각선.
    //   4142/10000 정수비로 부동소수 없이 판정 - 좌표 차가 정수라 결과가 결정적.
    const bool diagonal = (adx * 10000 >= ady * 4142) && (ady * 10000 >= adx * 4142);
    const bool right = (dx > 0);
    const bool down  = (dy > 0);

    if (diagonal)
    {
        if (down) { return right ? EMoveDirection::DOWN_RIGHT : EMoveDirection::DOWN_LEFT; }
        return right ? EMoveDirection::UP_RIGHT : EMoveDirection::UP_LEFT;
    }
    if (adx >= ady) { return right ? EMoveDirection::RIGHT : EMoveDirection::LEFT; }
    return down ? EMoveDirection::DOWN : EMoveDirection::UP;
}
