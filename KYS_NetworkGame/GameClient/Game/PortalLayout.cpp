#include "pch_gameclient.h"
#include "PortalLayout.h"
#include "MapData/PortalTable.h"   // 공유 PortalDef / PortalTableCount / PortalTableAt (GameCommon SSOT)

// 포탈 데이터는 GameCommon 공유 SSOT(PortalTable) - 클라는 서버와 같은 portals.csv 를 읽는다.
//   여기엔 클라 고유의 "내가 어느 포탈 반경 안인가" 판정만 남긴다 (렌더는 GameRenderer 가 직접 조회).
int FindPortalNear(int mapId, int x, int y)
{
	const int count = PortalTableCount();
	for (int i = 0; i < count; ++i)
	{
		const PortalDef& portal = PortalTableAt(i);
		if (portal.srcMapId != mapId)
			continue;
		const int dx = x - portal.trigger.x;
		const int dy = y - portal.trigger.y;
		if (dx * dx + dy * dy <= portal.triggerRadius * portal.triggerRadius)   // 거리 제곱 <= 반경 제곱
			return i;
	}
	return -1;
}
