#include "pch_serverapp.h"
#include "Cell.h"
#include <algorithm>   // std::find / std::swap (swap-and-pop 용)

namespace KYS
{
    namespace SERVERAPP
    {

        // 이 칸에 객체를 추가한다.
        void Cell::Insert(GameObject* obj)
        {
            m_objects.push_back(obj);
        }

        // 이 칸에서 객체를 찾아 제거한다 - 마지막 원소와 바꿔치고 뒤를 잘라냄 (순서 비보존이라 드물게 일어나는 제거에 적합).
        void Cell::Erase(GameObject* obj)
        {
            std::vector<GameObject*>::iterator it = std::find(m_objects.begin(), m_objects.end(), obj);   // O(n) - 칸당 객체 수가 작아 무해
            if (it != m_objects.end())
            {
                std::swap(*it, m_objects.back());   // 마지막 원소를 빈 자리로 옮김 (근처 수집 순회는 순서 무관)
                m_objects.pop_back();
            }
        }

        // 이 칸의 객체 목록을 돌려준다 (근처 객체 수집이 읽음).
        const std::vector<GameObject*>& Cell::GetObjects() const
        {
            return m_objects;
        }

    }
}
