#pragma once
#include <vector>

namespace KYS
{
    namespace SERVERAPP
    {

        class GameObject;

        // 격자(grid)의 한 칸(cell) - 이 칸에 들어 있는 객체들을 담는다 (근처 객체 수집이 인접 칸들을 모아 읽음).
        class Cell
        {
        public:
            Cell() = default;
            ~Cell() = default;
            Cell(const Cell&) = delete;
            Cell& operator=(const Cell&) = delete;
            Cell(Cell&&) = delete;
            Cell& operator=(Cell&&) = delete;

            void Insert(GameObject* obj);                                // 이 칸에 객체 추가
            void Erase(GameObject* obj);                                 // 이 칸에서 객체 제거 (순서 무관)
            const std::vector<GameObject*>& GetObjects() const;          // 이 칸의 객체 목록 (근처 객체 수집이 읽음)

        private:
            std::vector<GameObject*> m_objects;   // 이 칸에 든 객체들 (근처 수집 대상)
        };

    }
}
