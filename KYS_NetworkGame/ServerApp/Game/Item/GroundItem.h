#pragma once
#include "../GameObject.h"                       // base 완전 정의

namespace KYS
{
    namespace SERVERAPP
    {
        // 세 번째 GameObject 파생 클래스 (Player, Monster 다음).
        //   바닥에 떨어진 아이템 - 몬스터/플레이어와 같은 맵 엔티티로 격자와 AOI spawn/despawn 에 참여한다.
        //   비영속: DB 에 저장하지 않고 만료(스윕)나 서버 재시작 때 사라진다. 영속 uid 는 주울 때 발급.
        class GroundItem : public GameObject
        {
        public:
            explicit GroundItem(UINT64 id);   // id = 맵 개체 번호 (m_nextObjectId 공유 발급 - 줍기 요청의 키)
            ~GroundItem() override;
            // 복사/이동 재선언 안 함 - GameObject base 의 4줄 =delete 상속 (Player/Monster 와 동일)

            void Update(float) override;                                                            // 이동/AI 없음 - 만료는 MapManager 스윕이 판정
            virtual bool SerializeSpawn(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const override;     // SC_GROUND_ITEM_SPAWN (시야 진입자에게) - SerializeBroadcast 는 base no-op 상속(이동 없음)

            // 스폰 직후 1회 세팅 - 내용물 + 드랍 시각 + 데미지 기여 1~3순위 (없는 순위는 0).
            void InitDrop(int templateId, int quantity, UINT64 spawnTimeMs, const UINT32 ownerCharIds[3]);

            // 계단식 소유권 판정 (rAthena): 드랍 후 3초는 1순위만, 5초까지 1~2순위, 7초까지 1~3순위, 이후 자유.
            //   기여자가 아예 없으면(전투 없이 생긴 드랍) 처음부터 자유.
            bool CanBePickedBy(UINT32 charId, UINT64 nowMs) const;

            bool IsExpired(UINT64 nowMs) const;   // 만료? (스윕이 회수 판정 - 드랍 후 GROUND_ITEM_EXPIRE_MS 경과)

            int    GetTemplateId() const { return m_templateId; }
            int    GetQuantity() const { return m_quantity; }
            UINT64 GetSpawnTimeMs() const { return m_spawnTimeMs; }

        private:
            int    m_templateId;       // 아이템 종류 (Data/items.csv 참조)
            int    m_quantity;         // 수량 (주우면 이 전량이 인벤으로)
            UINT64 m_spawnTimeMs;      // 드랍 시각 (벽시계 ms) - 만료와 소유권 계단의 공통 기준
            UINT32 m_ownerCharIds[3];  // 데미지 기여 1~3순위 char_id (0 = 해당 순위 없음)
        };
    }
}
