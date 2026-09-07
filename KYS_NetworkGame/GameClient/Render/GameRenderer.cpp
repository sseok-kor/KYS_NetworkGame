#include "pch_gameclient.h"
#include "GameRenderer.h"
#include "../Game/LocalPlayer.h"
#include "../Game/GameState.h"
#include "MapData/PortalTable.h"     // 공유 포탈 표 (GameCommon SSOT) - PortalTableCount / PortalTableAt
#include "ItemData/ItemTable.h"       // FindItemDef (바닥 드랍 이름 표시 - 서버와 같은 Data/items.csv)
#include "GameDefines.h"     // VIEW_RANGE / CELL_SIZE / WALK_CELL_SIZE
#include "MapData/MapTable.h"        // MapWidthFor/MapHeightFor (맵 경계 렌더 - 맵마다 크기 다름)
#include "MapData/WalkableTable.h"   // IsWalkable/IsWalkableTableLoaded (벽 칸 렌더 + 강등 모드 경고)

#pragma warning(push, 0)
#include "imgui.h"
#pragma warning(pop)

// 월드 px -> 화면 px 배율 (자기중심). VIEW_RANGE(400) 가 화면에 여유롭게 들어오도록.
static const float kWorldToScreen = 0.8f;

// 몬스터 종류별 표시 스타일 - EMonsterType 값을 인덱스로 직접 조회(0..8 연속).
// 타플레이어 주황과 겹치지 않게, 종류끼리도 구분되게 색을 고르고 머리글자로 식별한다.
struct MonsterStyle
{
	ImU32       color;
	const char* glyph;
};
static const MonsterStyle kMonsterStyles[9] =
{
	{ IM_COL32(110, 200,  90, 255), "G" },   // GOBLIN - 연두(작은 잡몹)
	{ IM_COL32( 60, 160,  80, 255), "O" },   // ORC    - 초록
	{ IM_COL32(160, 100,  60, 255), "R" },   // OGRE    - 갈색
	{ IM_COL32( 90, 200, 220, 255), "S" },   // SLIME  - 하늘(점액)
	{ IM_COL32(235,  60,  60, 255), "F" },   // FENRIR - 빨강(보스)
	{ IM_COL32(170, 170, 180, 255), "W" },   // WOLF   - 회색(야수)
	{ IM_COL32(120,  80,  50, 255), "B" },   // BEAR   - 짙은 갈색
	{ IM_COL32(150, 120, 200, 255), "T" },   // TROLL  - 보라
	{ IM_COL32(215, 100, 230, 255), "L" },   // GOLEM  - 자홍(보스급)
};

// 엔티티 머리 위 체력바 - 빨강 바탕 + 남은 비율만큼 초록. (cx,cy)=글리프 중심.
static void DrawHpBar(ImDrawList* dl, float cx, float cy, int hp, int maxHp)
{
	if (maxHp <= 0)
		return;
	float ratio = static_cast<float>(hp) / static_cast<float>(maxHp);
	if (ratio < 0.0f) ratio = 0.0f; else if (ratio > 1.0f) ratio = 1.0f;
	const float w = 26.0f;
	const float h = 4.0f;
	const float x0 = cx - w * 0.5f;
	const float y0 = cy - 17.0f;   // 글리프 바로 위
	dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(70, 20, 20, 200));            // 빈 체력(빨강 바탕)
	dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w * ratio, y0 + h), IM_COL32(70, 200, 70, 230));   // 남은 체력(초록)
	dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(15, 15, 15, 220));                  // 테두리
}

// 서버 wchar_t 문자열 -> ImGui 표시용 UTF-8 char (스택 버퍼, 동적할당 0).
static const char* ToUtf8(const wchar_t* src, char* dst, int dstCap)
{
	const int n = ::WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstCap, nullptr, nullptr);
	if (n <= 0 && dstCap > 0)
		dst[0] = '\0';
	return dst;
}

// 이동 방향 부채꼴 - 원 중심에서 방향 쪽으로 60도 채운 부채꼴. 자기/타플레이어/몬스터 공통 모양(흰색), 정지해도 마지막 방향 유지.
static void DrawDirectionFan(ImDrawList* dl, ImVec2 center, float radius, EMoveDirection dir, ImU32 color)
{
	// 방향 인덱스 -> 화면 각(rad). 화면 Y는 아래로 증가: 오른쪽=0, 아래=+PI/2, 왼쪽=PI, 위=-PI/2. 대각은 45도 사이각.
	static const float kPi = 3.14159265358979f;   // imgui IM_PI는 imgui_internal.h 전용이라 자체 정의
	static const float kDirAngle[8] =
	{
		-kPi * 0.5f,    // UP
		 kPi * 0.5f,    // DOWN
		 kPi,           // LEFT
		 0.0f,          // RIGHT
		-kPi * 0.75f,   // UP_LEFT
		-kPi * 0.25f,   // UP_RIGHT
		 kPi * 0.75f,   // DOWN_LEFT
		 kPi * 0.25f,   // DOWN_RIGHT
	};
	const int d = static_cast<int>(dir);
	if (d < 0 || d >= static_cast<int>(EMoveDirection::COUNT))
		return;   // 손상 프레임 방어 (서버는 0..7만)
	const float base = kDirAngle[d];
	const float half = kPi / 6.0f;   // 좌우 30도씩 = 총 60도
	dl->PathClear();
	dl->PathLineTo(center);
	dl->PathArcTo(center, radius, base - half, base + half);   // a_min < a_max (CW) - imgui 요구
	dl->PathFillConvex(color);
}

// 공격 범위 원 - 반투명 채움 + 선명한 외곽선(업계 telegraph 표준). 색으로 공격 주체 구분(자기/타/몬스터).
static void DrawAttackRange(ImDrawList* dl, ImVec2 center, float radiusScreen, ImU32 fill, ImU32 outline)
{
	dl->AddCircleFilled(center, radiusScreen, fill, 48);
	dl->AddCircle(center, radiusScreen, outline, 48, 2.0f);
}

void GameRenderer::Draw(const LocalPlayer& localPlayer, const GameState& gameState)
{
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	const float cx = disp.x * 0.5f;
	const float cy = disp.y * 0.5f;

	const int selfX = localPlayer.GetX();
	const int selfY = localPlayer.GetY();

	const ImU32 gridColor   = IM_COL32(55, 58, 68, 255);
	const ImU32 borderColor = IM_COL32(120, 90, 60, 255);
	const ImU32 aoiColor    = IM_COL32(70, 130, 190, 160);
	const ImU32 selfColor   = IM_COL32(90, 220, 130, 255);
	const ImU32 textColor   = IM_COL32(230, 230, 230, 255);
	const ImU32 otherColor  = IM_COL32(230, 170, 60, 255);    // 타플레이어 (주황)

	// 보이는 월드 범위(자기중심).
	// 방향 부채꼴(흰색, 전 객체 공통) + 공격범위 색(주체별 구분 - 실무 telegraph 관례: 적=빨강, 아군=초록, 자신=파랑).
	const ImU32 fanColor       = IM_COL32(255, 255, 255, 235);   // 이동방향 부채꼴
	const ImU32 selfAtkFill    = IM_COL32( 70, 130, 240,  55);   // 내 공격범위 (파랑)
	const ImU32 selfAtkLine    = IM_COL32( 90, 150, 255, 210);
	const ImU32 otherAtkFill   = IM_COL32( 70, 220, 120,  50);   // 타플레이어 공격범위 (초록)
	const ImU32 otherAtkLine   = IM_COL32( 90, 240, 140, 210);
	const ImU32 monsterAtkFill = IM_COL32(235,  70,  70,  50);   // 몬스터 공격범위 (빨강, 위험)
	const ImU32 monsterAtkLine = IM_COL32(255,  90,  90, 215);

	const float halfWorldX = cx / kWorldToScreen;
	const float halfWorldY = cy / kWorldToScreen;

	// (1) CELL_SIZE 격자선 - 월드 좌표에 고정(이동 시 격자가 흐른다 = 이동 시각화).
	const int leftW  = selfX - static_cast<int>(halfWorldX);
	const int rightW = selfX + static_cast<int>(halfWorldX);
	const int topW   = selfY - static_cast<int>(halfWorldY);
	const int botW   = selfY + static_cast<int>(halfWorldY);

	for (int wx = (leftW / CELL_SIZE) * CELL_SIZE; wx <= rightW; wx += CELL_SIZE)
	{
		const float sx = cx + static_cast<float>(wx - selfX) * kWorldToScreen;
		dl->AddLine(ImVec2(sx, 0.0f), ImVec2(sx, disp.y), gridColor);
	}
	for (int wy = (topW / CELL_SIZE) * CELL_SIZE; wy <= botW; wy += CELL_SIZE)
	{
		const float sy = cy + static_cast<float>(wy - selfY) * kWorldToScreen;
		dl->AddLine(ImVec2(0.0f, sy), ImVec2(disp.x, sy), gridColor);
	}

	// (2) 맵 경계 [0,w]x[0,h] - 내가 맵 어디쯤인지 보이게 (맵마다 크기 다름 - 미적재면 4000 폴백).
	const int curMapId = localPlayer.GetMapId();
	const int mapW = MapWidthFor(curMapId);
	const int mapH = MapHeightFor(curMapId);
	{
		const float bx0 = cx + static_cast<float>(0 - selfX) * kWorldToScreen;
		const float by0 = cy + static_cast<float>(0 - selfY) * kWorldToScreen;
		const float bx1 = cx + static_cast<float>(mapW - selfX) * kWorldToScreen;
		const float by1 = cy + static_cast<float>(mapH - selfY) * kWorldToScreen;
		dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), borderColor, 0.0f, 0, 2.0f);
	}

	// (2b) 벽 칸 렌더 - 화면에 걸치는 통행 격자 칸 중 벽('0')을 회색으로 채운다 (지형의 유일한 가시화).
	//   지형 미적재(강등 모드)면 IsWalkable 이 전부 통행이라 아무것도 안 그려지고, 대신 경고 라벨을 띄운다.
	if (IsWalkableTableLoaded())
	{
		const ImU32 wallFill = IM_COL32(120, 120, 130, 170);   // 벽 (회색 채움)
		int wallLeft = (leftW / WALK_CELL_SIZE) * WALK_CELL_SIZE;
		if (wallLeft < 0) { wallLeft = 0; }
		int wallTop = (topW / WALK_CELL_SIZE) * WALK_CELL_SIZE;
		if (wallTop < 0) { wallTop = 0; }
		for (int wy = wallTop; wy <= botW && wy < mapH; wy += WALK_CELL_SIZE)
		{
			for (int wx = wallLeft; wx <= rightW && wx < mapW; wx += WALK_CELL_SIZE)
			{
				if (IsWalkable(curMapId, wx, wy))
					continue;
				const float sx0 = cx + static_cast<float>(wx - selfX) * kWorldToScreen;
				const float sy0 = cy + static_cast<float>(wy - selfY) * kWorldToScreen;
				const float sx1 = cx + static_cast<float>(wx + WALK_CELL_SIZE - selfX) * kWorldToScreen;
				const float sy1 = cy + static_cast<float>(wy + WALK_CELL_SIZE - selfY) * kWorldToScreen;
				dl->AddRectFilled(ImVec2(sx0, sy0), ImVec2(sx1, sy1), wallFill);
			}
		}
	}
	else
	{
		// 강등 모드 표시 - 벽 데이터를 못 읽어 예측이 벽을 모른다 (벽 근처에서 서버 되당김 발생이 정상 동작).
		dl->AddText(ImVec2(10.0f, 30.0f), IM_COL32(255, 120, 120, 255),
			"[WARN] walkable data not loaded - terrain fallback (rubber-band near walls)");
	}

	// (3) AOI 시야 박스(자기중심 +-VIEW_RANGE) - 서버가 spawn/despawn 시키는 범위.
	{
		const float r = static_cast<float>(VIEW_RANGE) * kWorldToScreen;
		dl->AddRect(ImVec2(cx - r, cy - r), ImVec2(cx + r, cy + r), aoiColor);
	}

	// (4) 현재 맵의 포탈 마커 + 내 아바타 (배치 후에만 - 실제 mapId/위치를 알아야 함).
	if (localPlayer.IsPlaced())
	{
		const int curMap = localPlayer.GetMapId();
		const ImU32 portalColor = IM_COL32(190, 90, 220, 255);
		const ImU32 portalRing  = IM_COL32(150, 70, 200, 110);
		const int markerCount = PortalTableCount();
		for (int i = 0; i < markerCount; ++i)
		{
			const PortalDef& portal = PortalTableAt(i);
			if (portal.srcMapId != curMap)
				continue;
			const float px = cx + static_cast<float>(portal.trigger.x - selfX) * kWorldToScreen;
			const float py = cy + static_cast<float>(portal.trigger.y - selfY) * kWorldToScreen;
			const float pr = static_cast<float>(portal.triggerRadius) * kWorldToScreen;
			dl->AddCircle(ImVec2(px, py), pr, portalRing, 0, 2.0f);     // 발동 반경(이 안에 들어가면 자동 이동)
			dl->AddCircleFilled(ImVec2(px, py), 9.0f, portalColor);     // 포탈 중심 마커
			char label[32];
			::sprintf_s(label, "PORTAL ->map%d", portal.dstMapId);
			dl->AddText(ImVec2(px + 11.0f, py - 9.0f), portalColor, label);
		}

		// 바닥 드랍(SC_GROUND_ITEM_SPAWN) - 작은 노란 마름모 + 이름/수량 라벨. 근처면 'Z'로 줍기.
		const ImU32 itemColor = IM_COL32(240, 210, 70, 255);
		const std::unordered_map<unsigned int, GroundItemView>& groundItems = gameState.GroundItems();
		for (std::unordered_map<unsigned int, GroundItemView>::const_iterator gi = groundItems.begin(); gi != groundItems.end(); ++gi)
		{
			const GroundItemView& g = gi->second;
			const float gx = cx + static_cast<float>(g.x - selfX) * kWorldToScreen;
			const float gy = cy + static_cast<float>(g.y - selfY) * kWorldToScreen;
			dl->AddCircleFilled(ImVec2(gx, gy), 5.0f, itemColor);
			dl->AddCircle(ImVec2(gx, gy), 7.0f, IM_COL32(120, 100, 30, 200), 0, 1.5f);
			char label[ITEM_NAME_MAX * 3 + 16];
			const ItemDef* def = FindItemDef(g.templateId);
			if (def != nullptr)
			{
				char name[ITEM_NAME_MAX * 3];
				ToUtf8(def->name, name, sizeof(name));
				if (g.quantity > 1)
					::sprintf_s(label, "%s x%d", name, g.quantity);
				else
					::sprintf_s(label, "%s", name);
			}
			else
			{
				::sprintf_s(label, "item#%d", g.templateId);   // 표에 없는 종류 방어(관대 로드)
			}
			dl->AddText(ImVec2(gx + 9.0f, gy - 8.0f), itemColor, label);
		}

		// 원격 객체(타플레이어/몬스터) - 렌더 위치 = 권위 위치(외삽) + renderErr(수렴 스무딩).
		const std::unordered_map<unsigned int, RemoteEntity>& entities = gameState.Entities();
		for (std::unordered_map<unsigned int, RemoteEntity>::const_iterator it = entities.begin(); it != entities.end(); ++it)
		{
			const RemoteEntity& e = it->second;
			// 렌더 위치 = 권위 위치 + renderErr(수렴 스무딩). 하드 스냅을 부드럽게 흡수해 잦은 broadcast 떨림을 없앤다.
			//   글리프/hp바/이름/공격범위원/말풍선/데미지가 모두 이 ex/ey 파생이라 함께 부드러워진다.
			const float ex = cx + (static_cast<float>(e.x) + e.renderErrX - static_cast<float>(selfX)) * kWorldToScreen;
			const float ey = cy + (static_cast<float>(e.y) + e.renderErrY - static_cast<float>(selfY)) * kWorldToScreen;
				// 공격 중이면 공격범위 원(원 아래, 반투명). 플레이어=SKILL_HIT_RANGE, 몬스터=서버가 준 attackRange.
				if (e.attackFlashTimer > 0.0f)
				{
					const bool atkIsMonster = (e.objectType != EObjectType::PLAYER);
					const int atkRangePx = atkIsMonster ? e.attackRange : SKILL_HIT_RANGE;
					const float atkRs = static_cast<float>(atkRangePx) * kWorldToScreen;
					if (atkIsMonster)
						DrawAttackRange(dl, ImVec2(ex, ey), atkRs, monsterAtkFill, monsterAtkLine);
					else
						DrawAttackRange(dl, ImVec2(ex, ey), atkRs, otherAtkFill, otherAtkLine);
				}
			// 시신(SC_DEATH~SC_DESPAWN 1초) - 회색 반투명으로 그려 막타 데미지 숫자가 붙을 자리를 남긴다.
			const ImU32 corpseColor = IM_COL32(130, 130, 130, 140);
			if (e.objectType == EObjectType::PLAYER)
			{
				dl->AddCircleFilled(ImVec2(ex, ey), 6.0f, e.dead ? corpseColor : otherColor);
				char lbl[CHAT_SENDER_MAX * 3];   // UTF-8 한글 3바이트/자
				if (e.name[0] != L'\0')
					ToUtf8(e.name, lbl, sizeof(lbl));   // 스폰에 동봉된 캐릭터 이름
				else
					::sprintf_s(lbl, "P%u", e.id);       // 이름 미학습(몬스터 스폰 등) - 임시 id 라벨
				dl->AddText(ImVec2(ex + 8.0f, ey - 8.0f), textColor, lbl);
			}
			else
			{
				// 몬스터 - 종류(EMonsterType)별 색/문자. 보스(서버가 monsters.yml isBoss 로 판정해 전달)는 더 크게.
				int mt = static_cast<int>(e.monsterType);
				if (mt < 0 || mt >= 9)
					mt = 0;   // 서버가 보증하지만 OOB 방어
				const MonsterStyle& style = kMonsterStyles[mt];
				const bool isBoss = e.isBoss;   // 서버 전달 값 (종류 하드코딩 판정 대체 - 새 보스 추가 = 데이터 편집만)
				const float radius = isBoss ? 10.0f : 6.0f;
				dl->AddCircleFilled(ImVec2(ex, ey), radius, e.dead ? corpseColor : style.color);
				dl->AddText(ImVec2(ex - 4.0f, ey - 8.0f), textColor, style.glyph);   // 머리글자로 종류 식별
			}

				// 이동 방향 부채꼴(원 위, 흰색). 플레이어=반지름6, 보스=10에 맞춤. 시신은 생략.
				if (!e.dead)
					DrawDirectionFan(dl, ImVec2(ex, ey),
						(e.objectType == EObjectType::PLAYER) ? 6.0f
							: (e.isBoss ? 10.0f : 6.0f),
						e.direction, fanColor);
				// 체력바 + 플로팅 데미지 (플레이어/몬스터 공통)
				DrawHpBar(dl, ex, ey, e.hp, e.maxHp);
				if (e.damagePopupTimer > 0.0f)
				{
					char dmg[16];
					::sprintf_s(dmg, "-%d", e.lastDamage);
					dl->AddText(ImVec2(ex - 6.0f, ey - 31.0f), IM_COL32(255, 220, 80, 255), dmg);   // 노랑 데미지 숫자
				}

				// 머리 위 채팅 말풍선 (최근 발화 4초간)
				if (e.chatBubbleTimer > 0.0f)
				{
					char bubble[CHAT_MSG_MAX * 3];   // UTF-8 한글 3바이트/자 -> 원본 wchar x3
					ToUtf8(e.chatBubble, bubble, sizeof(bubble));
					const ImVec2 bsz = ImGui::CalcTextSize(bubble);
					const ImVec2 bp(ex - bsz.x * 0.5f, ey - 42.0f);
					dl->AddRectFilled(ImVec2(bp.x - 3.0f, bp.y - 1.0f), ImVec2(bp.x + bsz.x + 3.0f, bp.y + bsz.y + 1.0f), IM_COL32(0, 0, 0, 150), 3.0f);
					dl->AddText(bp, IM_COL32(255, 255, 255, 255), bubble);
				}
		}

		// 내 아바타(화면 중앙) + 라벨.
		if (localPlayer.GetAttackFlashTimer() > 0.0f)
				DrawAttackRange(dl, ImVec2(cx, cy), static_cast<float>(SKILL_HIT_RANGE) * kWorldToScreen, selfAtkFill, selfAtkLine);   // 내 공격범위 (파랑)
			dl->AddCircleFilled(ImVec2(cx, cy), 7.0f, selfColor);
			DrawDirectionFan(dl, ImVec2(cx, cy), 7.0f, localPlayer.GetDir(), fanColor);   // 내 이동방향
		char selfLabel[WHISPER_NAME_MAX * 3 + 4];
		if (localPlayer.GetName()[0] != L'\0')
		{
			char nameUtf8[WHISPER_NAME_MAX * 3];
			ToUtf8(localPlayer.GetName(), nameUtf8, sizeof(nameUtf8));
			::sprintf_s(selfLabel, "@ %s", nameUtf8);   // 내 이름표(캐릭터 이름) - 이 렌더는 캐릭터 선택 후에만 돌아서 로그인 id 임시 라벨은 이미 덮여 있다
		}
		else
			::strcpy_s(selfLabel, "@ me");
		dl->AddText(ImVec2(cx + 9.0f, cy - 9.0f), textColor, selfLabel);
			DrawHpBar(dl, cx, cy, localPlayer.GetHp(), localPlayer.GetMaxHp());   // 내 체력바
		// 내 피격 데미지 숫자 (빨강 - 받은 피해 경고. 원격 엔티티의 노랑과 구분)
		if (localPlayer.GetDamagePopupTimer() > 0.0f)
		{
			char selfDmg[16];
			::sprintf_s(selfDmg, "-%d", localPlayer.GetLastDamage());
			dl->AddText(ImVec2(cx - 6.0f, cy - 31.0f), IM_COL32(255, 95, 95, 255), selfDmg);
		}
		// 내 채팅 말풍선 (자기 발화 - LocalPlayer 보유)
		if (localPlayer.GetChatBubbleTimer() > 0.0f)
		{
			char selfBubble[CHAT_MSG_MAX * 3];   // UTF-8 한글 3바이트/자 -> 원본 wchar x3
			ToUtf8(localPlayer.GetChatBubble(), selfBubble, sizeof(selfBubble));
			const ImVec2 sbsz = ImGui::CalcTextSize(selfBubble);
			const ImVec2 sbp(cx - sbsz.x * 0.5f, cy - 42.0f);
			dl->AddRectFilled(ImVec2(sbp.x - 3.0f, sbp.y - 1.0f), ImVec2(sbp.x + sbsz.x + 3.0f, sbp.y + sbsz.y + 1.0f), IM_COL32(0, 0, 0, 150), 3.0f);
			dl->AddText(sbp, IM_COL32(255, 255, 255, 255), selfBubble);
		}
	}
}
