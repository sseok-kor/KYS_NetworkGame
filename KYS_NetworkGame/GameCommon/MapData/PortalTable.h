#pragma once
#include "CommonStructs.h"   // Position (x, y)

// 공유 포탈 표 - 서버(트리거 검증 + 도착 이동)와 클라(렌더 + 근접 판정)가
//   같은 파일(Data/portals.csv)을 부팅 시 적재하는 단일 진실원. 손미러 표 폐기.
//   서버 권위: 도착 좌표(dst)는 서버만 사용, 클라는 트리거 위치만 렌더.
struct PortalDef
{
    int      srcMapId;       // 이 포탈이 있는 맵 (불일치 = 스푸핑 차단)
    Position trigger;        // 포탈 트리거 중심 좌표
    int      triggerRadius;  // 근접 임계(px) - 플레이어가 이 안이어야 발동 (거리 제곱 비교)
    int      dstMapId;       // 목적지 맵 (클라는 라벨용, 서버는 이동)
    Position dst;            // 도착 좌표 (서버 권위 - 클라는 안 씀)
};

// 부팅 1회: Data/portals.csv 적재. 반환 = 적재한 포탈 수 (파일 없음/0행 = -1, fail-fast).
//   exe 디렉터리에서 위로 몇 단계씩 훑어 같은 공유 파일을 찾는다 (working dir 기준 탐색은 마지막 폴백).
int               LoadPortalTable();

// 적재된 포탈 수. portalId 는 0..count-1 (= 행 순서).
int               PortalTableCount();

// portalId 가 가리키는 포탈. 호출자가 0..count-1 범위를 보장한다 (서버 ResolvePortal / 클라 루프).
const PortalDef&  PortalTableAt(int portalId);
