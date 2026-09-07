#pragma once

// 클라 전용 근접 판정 - (mapId, x, y) 가 어느 포탈 트리거 반경 안인가? -> portalId(인덱스), 없으면 -1.
//   포탈 데이터 자체는 GameCommon 공유 표(PortalTable)에서 적재한다 (서버와 같은 Data/portals.csv).
//   서버 ResolvePortal 과 동일한 거리 제곱 비교 - 같은 파일을 읽으므로 결과가 일치한다.
int FindPortalNear(int mapId, int x, int y);
