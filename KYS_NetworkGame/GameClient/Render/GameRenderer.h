#pragma once

class LocalPlayer;
class GameState;

// 게임 화면 렌더러 - ImGui DrawList 기반 자기중심 격자 뷰.
// 자기 위치를 화면 중앙에 두고 격자(월드 고정)/내 아바타를 그린다.
// 자기중심 격자(월드 고정) + 내 아바타 + 맵 경계 + AOI 박스 + 원격 엔티티(타플레이어/몬스터)/포탈/체력바/말풍선.
class GameRenderer
{
public:
	GameRenderer() = default;
	~GameRenderer() = default;

	GameRenderer(const GameRenderer&) = delete;
	GameRenderer& operator=(const GameRenderer&) = delete;

	// ImGui::NewFrame 이후 호출. 배경 DrawList 에 자기중심 월드를 그린다.
	void Draw(const LocalPlayer& localPlayer, const GameState& gameState);
};
