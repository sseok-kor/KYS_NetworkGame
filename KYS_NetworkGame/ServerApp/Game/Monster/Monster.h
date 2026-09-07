#pragma once
#include "../GameObject.h"                       // base 완전 정의
#include "../../../GameCommon/GameTypes.h"        // EMoveState/EMoveDirection/EObjectType/EMonsterState/EMonsterType
#include "../../../GameCommon/CommonStructs.h"    // Position (m_patrolAnchor)
#include "../../../GameCommon/GameDefines.h"      // 몬스터 공통 상수 (MONSTER_DEATH_GRACE_SEC 등)
#include "../../../GameCommon/Pathfinding/PathFinder.h"       // PATH_WAYPOINT_MAX (경로 waypoint 버퍼 크기)

namespace KYS
{
    namespace SERVERAPP
    {
        struct MonsterSpawnInfo
        {
            int              mapId = 0;                              // 어느 맵에 spawn (m_maps[mapId])
            Position         pos = { 0, 0 };                         // spawn 좌표 (맵 안 통행 칸 - 호출자[그룹 스폰 재추첨]가 보장)
            EMonsterType     monsterType = EMonsterType::GOBLIN;     // 종류 (스탯 테이블 키 + 전송 데이터 렌더 구분)
            Position         patrolAnchor = { 0, 0 };                // 배회/추격 복귀 기준점 (= spawn pos - 부활 위치 == 배회 기준점 불변식)
        };

        // 종류별 스탯 (대부분 서버 전용. 단 attackRange/moveSpeed는 SC_MONSTER_SPAWN으로 클라에 전송 -
        //   공격범위 표시 + 종류별 속도 위치 예측용). 값은 Data/monsters.yml 에서 부팅 시 적재 (조정 = 재컴파일 대신 파일 한 줄).
        struct MonsterTemplate
        {
            int   hp;
            int   atkPower;
            int   moveSpeed;
            int   aggroRange;
            int   attackRange;
            float attackCooldown;
        };
        const MonsterTemplate& MonsterTemplateFor(EMonsterType type);   // 정의는 MonsterTable.cpp (monsters.yml 적재 표 조회, 단일 인스턴스)

        class Map;   // 전방 선언 - Monster가 자기 맵에 교전 대상 스캔을 위임(back-pointer). Map.h는 Monster.h를 include 안 해 순환 없음.

        // 두 번째 GameObject 파생 클래스 (Player가 첫 번째).
        class Monster : public GameObject
        {
        public:
            explicit Monster(UINT64 id);    // GameObject(id, EObjectType::MONSTER) 생성자에 위임 (id = 전송용 id, m_nextObjectId가 발급)
            ~Monster() override;            // GameObject의 virtual 소멸자 상속
            // 복사/이동 재선언 안 함 - GameObject base의 4줄 =delete 상속 (Player와 동일)

            void Update(float deltaTime) override;                                                  // AI 상태 기계 갱신 + 이동 위치 예측 (매 tick)
            virtual bool SerializeBroadcast(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const override; // SC_MONSTER_MOVE 패킷 생성 (이동 상태/위치)
            virtual bool SerializeSpawn(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const override;     // SC_MONSTER_SPAWN 패킷 생성 (전체 상태)

            // 판단 간격 제한 (m_thinkTimer 누산, 추격 탐지 단계가 호출)
            bool ShouldRethink(float deltaTime);           // deltaTime 누산 -> 간격 도달 시 true 반환 + 리셋

            bool ConsumeAttack(int& outDamage, GameObject*& outVictim);   // 이번 tick 공격했나? 했으면 데미지+공격대상 출력 + 플래그 clear (Map::Update가 호출)
            bool ConsumeVictimDeath();            // 이번 tick 타겟을 죽였나? 소비형 (한 번만 true)
            void MarkDead();                 // hp 0 시 호출: DEAD 전이 + 시신 유지 타이머 리셋
            bool IsDeadExpired() const;      // 시신 유지 시간 만료? (SweepDead가 회수 판정)

            // AI 상태 접근 (데이터 멤버는 private)
            void          SetMap(Map* m);                  // 자기 맵 back-pointer 지정 (SpawnMonster가 호출, 교전 대상 스캔 위임용)
            EMonsterState GetState() const;                // 현재 AI 상태 읽기
            void          SetState(EMonsterState s);       // AI 상태 전이

            // 종류 보존. SpawnMonster가 set, SerializeSpawn/스탯 조회가 read.
            EMonsterType GetMonsterType() const;
            void         SetMonsterType(EMonsterType t);

            // 소속 스폰 그룹 인덱스 (SpawnMonster가 set, 시신 회수가 read - 그 그룹에 부활 예약을 걸기 위한 역참조).
            //   -1 = 미소속 (ctor 기본값 - 풀 재사용 시 ctor 재실행이 리셋).
            int  GetSpawnGroupIdx() const;
            void SetSpawnGroupIdx(int idx);


            // 이동 상태/방향 접근자(SetMoveState/GetMoveState/GetDirection)는 GameObject base에 있어 상속만 함 (여기 재선언하면 이중 선언 = 컴파일 에러).

            // 길찾기 (Phase D) - 소속 채널 stamp + 경로 요청/결과 수신. 실제 추종/요청 시점은 Update FSM.
            void SetChannelId(int channelId);   // SpawnMonster가 stamp (길찾기 요청/결과 회신 대상)
            // 길찾기 결과 도착 - seq/targetId/AI 상태 재검증 후 waypoint 적용/폐기 + pending 해제 (채널 스레드).
            void OnPathResult(UINT32 seq, UINT32 targetId, bool success, const Position* waypoints, int count);

            // 스폰 시 1회 세팅 (MapManager::Spawn(MonsterSpawnInfo)가 호출).
            void            SetPatrolAnchor(const Position& p);   // 배회/추격 복귀 기준점 set (m_patrolPoint도 anchor로 초기화)
            const Position& GetPatrolPoint() const;              // Update가 배회 이동에 read

            // 데미지 기여 기록 - 드랍 소유권 판정 재료 (rAthena 계단식: 기여 상위 3명이 순서대로 우선권).
            static const int MAX_DAMAGE_CREDITS = 8;              // 기여자 추적 상한 (초과 시 최소 기여자 교체)
            void AddDamageCredit(UINT32 charId, int damage);      // 타격마다 누적 (ApplySkillHit가 호출)
            void GetTopDamageCredits(UINT32 outCharIds[3]) const; // 기여 상위 3 char_id (없는 순위 = 0)

        private:
            // private 헬퍼 (Update만 호출 - FSM에서 분리한 자기완결 단위)
            void ApplyAttackTick(GameObject* target, const MonsterTemplate& monsterStats, float deltaTime);   // ATTACK 사거리 안: 쿨다운 누산 + 도달 시 교전 대상에 1회 타격(무적 가드, 사망 전이)
            void RequestPath(const Position& targetPos);   // 벽에 막혔을 때 우회 경로 요청 발행 (pathfinder 스레드로 - 성공 시에만 pending set)
            void ClearPath();                              // 경로/pending 초기화 (CHASE 이탈/포기/사망/직선 복귀 시)
            // 이동 한 걸음은 공유 AdvanceMove(GameCommon WalkableTable)가 처리 - 서버/클라/봇이 같은 수식·같은 벽 정지

            Map*        m_map;           // 자기 맵 back-pointer (소유 안 함, spawn 시 set). 매 틱 교전 대상 근접 스캔을 위임 (Monster::Update -> m_map->ResolveTarget). 몬스터는 맵 불변이라 수명 내 고정.
            UINT32      m_targetId; // 현재 교전 대상 player 전송 id (포인터 아님 = 값이라 dangling 불가). 어그로 범위 안이면 점착 유지(flicker 차단), 이탈/소실 시 0. 초기 0.
            GameObject* m_attackVictim;  // 이번 tick 공격이 때린 대상 (same-tick transient, m_didAttack True일 때만 유효). Map::Update가 ConsumeAttack로 소비.
            EMonsterState    m_state;         // AI 상태 기계 현재 상태
            float            m_thinkTimer;    // 판단 간격 누산 타이머
            // m_moveState/m_direction은 GameObject base 멤버 - Monster에서 재선언 안 함. 위치 예측/직렬화는 base 접근자로 접근.
            Position         m_patrolAnchor;  // 배회 기준점 (추격 한계 복귀, 배회 중심)
            Position         m_patrolPoint;  // 현재 배회 목적지 (anchor 반경 MONSTER_PATROL_RADIUS, Update가 think tick마다 갱신)
            bool             m_returningToAnchor; // 추격 한계 벗어나 복귀 중 플래그 (복귀 중 교전 스캔 skip. clear = anchor 도착 또는 복귀 길이 벽에 막힘[그 자리를 새 집으로])
            float m_attackCooldown;   // 공격 쿨다운 누산(초). ctor init 0.0f
            bool  m_didAttack;        // 이번 tick ATTACK이 데미지 적용했나. ctor init false
            int   m_lastDamage;       // 이번 타격의 원시 판정치(SC_DAMAGE.damage 표시용 - hp 적용은 TakeDamage가 따로 자름). ctor init 0
            bool  m_victimDied;       // 이번 tick 타겟을 죽였나. ctor init false
            float m_deathTimer;              // DEAD 상태 누산 시간(초). ctor init 0.0f
            EMonsterType m_monsterType;      // 종류 (스폰 시 set, 이후 불변). 스탯 테이블 키 + 전송 데이터 종류 필드
            int   m_spawnGroupIdx;           // 소속 스폰 그룹 (스폰 시 set, 이후 불변). 시신 회수가 이 그룹에 부활 예약. ctor init -1

            // 길찾기 (Phase D) - 벽에 막히면 우회 경로 요청, 결과 waypoint 를 순서대로 추종. 풀 재사용 시 ctor 재실행이 리셋.
            int      m_channelId;             // 소속 채널 (요청/결과 회신 대상). 스폰 시 stamp. ctor init -1
            UINT32   m_pathSeq;               // per-몬스터 단조 요청 번호 (늦게 온 옛 결과 폐기). ctor init 0
            bool     m_pathPending;           // 요청 발행 후 결과 대기 중 (중복 요청 가드). ctor init false
            float    m_pathPendingElapsed;    // 요청 후 경과(초) - TTL 초과 시 강제 해제(결과 드랍 자기치유). ctor init 0
            float    m_pathRetryCooldown;     // NOPATH 후 재요청 대기(초). ctor init 0
            float    m_pathFailElapsed;       // 연속 경로 실패 누산(초) - GIVEUP 초과 시 추격 포기. ctor init 0
            Position m_waypoints[PATH_WAYPOINT_MAX];   // 추종할 코너 열 (셀 중심 좌표). 없으면 count 0
            int      m_waypointCount;         // 유효 waypoint 수. ctor init 0
            int      m_waypointIndex;         // 현재 향하는 waypoint 인덱스 (도착 시 ++). ctor init 0

            // 데미지 기여 누적 표 (charId별 총 데미지). 풀 재사용 시 ctor 재실행이 비움.
            struct DamageCredit { UINT32 charId; int total; };
            DamageCredit m_damageCredits[MAX_DAMAGE_CREDITS];
        };
    }
}
