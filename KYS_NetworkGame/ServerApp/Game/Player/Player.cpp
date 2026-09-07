#include "pch_serverapp.h"
#include "Player.h"
#include "../GameCommon/GameDefines.h"
#include "../GameCommon/Protocol/GamePackets.h"
#include "../GameCommon/Protocol/PacketType.h"
#include "../GameCommon/Protocol/CPacket.h"
#include "../GameCommon/MapData/MapTable.h"       // MapTableAt/MapTableCount (부활 좌표 - maps.csv 적재 표)
#include "../GameCommon/MapData/WalkableTable.h"  // AdvanceMove (이동 한 걸음 - 서버/클라/봇 공유 수식 + 벽 정지)



namespace KYS
{
    namespace SERVERAPP
    {
        // 빈 플레이어로 초기화 - 받은 sid 를 GameObject base 에 넘기고 나머지 상태는 비워 둔다.
        //   id : 세션 핸들(sid). GameObject::m_id 로 확정되어 조회 키가 됨. PlayerId 는 spawn 때 따로 발급
        Player::Player(UINT64 id)
            : GameObject(id)        // sid 를 base ctor 로 전달 (m_id = sid 확정)
            , m_playerId(0)         // 0 = 아직 미발급 (실제 발급은 Channel 의 spawn)
            , m_lastProcessedSeq(0) // 아직 처리한 입력 없음
            , m_mp(0)
            , m_skillCooldown(0.0f)
            , m_respawnTimer(0.0f)
            , m_invulnUntil(0.0f)
            , m_justRespawned(false)
            , m_lastChatMs(0)
            , m_lastActionMs{}      // 행동별 직전 시각 전부 0 (포탈/채널변경/채널목록 쿨다운 리셋)
            , m_dbDirty(0)
            , m_charId(0)              // 선택 입장 시 DbResultJob 이 세팅 (0 = 미선택)
            , m_gameSession(nullptr)   // 풀 재사용 시 stale 역포인터 봉인 (ObjectPool placement-new가 ctor 재실행)
        {
            m_name[0] = L'\0';
        }

        Player::~Player()
        {
            // 다형 정리는 GameObject::~GameObject가 담당 (virtual). Player 고유 자원 없음.
        }

        // 맵 번호로 부활 좌표를 찾는다 (범위 밖이면 맵 0 좌표로 폴백).
        //   좌표는 Data/maps.csv 의 respawnX/respawnY 에서 부팅 시 적재 (하드코딩 표 폐기) -
        //   몬스터 spawn(맵 중앙부)에서 먼 구석이라 부활 직후 즉시 피격이 없다.
        //   mapId : 부활할 맵 번호
        // Player::Update 한 곳만 쓰므로 파일 안에서만 보이는 static (헤더 노출 불요).
        static Position RespawnPointFor(int mapId)
        {
            if (mapId < 0 || mapId >= MapTableCount()) { return MapTableAt(0).respawn; }
            return MapTableAt(mapId).respawn;
        }

        // 매 tick 호출 - 사망/리스폰, 무적/쿨다운 카운트다운, 이동 중이면 다음 위치 예측까지 처리한다.
        //   deltaTime : 지난 tick 부터 흐른 시간(초). 위치 예측/타이머 감소의 시간 단위
        void Player::Update(float deltaTime)
        {
            if (IsDead())                                  // hp 0 이하 = 사망 상태
            {
                m_respawnTimer += deltaTime;
                if (m_respawnTimer >= PLAYER_RESPAWN_DELAY_SEC)
                {
                    SetHp(GetMaxHp());                      // 부활 (hp 최대로)
                    SetPos(RespawnPointFor(GetMapId()));   // 고정 리스폰 지점으로 (셀 이동/가시성 갱신은 RelocateInGrid 가 처리)
                    m_invulnUntil = PLAYER_INVULN_SEC;   // 부활 직후 잠깐 무적
                    m_respawnTimer = 0.0f;
                    m_justRespawned = true;                // Map::Update 가 SC_RESPAWN 통지를 준비
                }
                return;                                    // 사망 중엔 위치 예측/이동 정지 (dirty 표시 안 함)
            }

            if (m_invulnUntil > 0.0f) { m_invulnUntil -= deltaTime; }      // 무적 카운트다운
            if (m_skillCooldown > 0.0f) { m_skillCooldown -= deltaTime; }  // 스킬 쿨다운 카운트다운 (ConsumeForSkill 시전 간격 제한)

            if (GetMoveState() != EMoveState::START)
            {
                return;   // 정지 상태면 위치 예측 안 함 (위치 그대로라 dirty 도 안 됨)
            }

            // dead-reckoning 으로 다음 위치 예측 - 전진/경계 클램프/벽 정지를 공유 AdvanceMove 한 곳이 처리.
            //   클라(자기 예측/원격 외삽)/봇도 같은 함수를 써서 서버와 같은 비트 결과 + 같은 곳에서 멈춘다
            //   (벽에 막히면 위치 불변 - 클라도 같이 멈추므로 추가 통지 불요).
            SetPos(AdvanceMove(GetMapId(), GetPos(), GetDirection(), MAX_MOVE_SPEED, deltaTime));
            // 이동 broadcast는 OnMove(CS_MOVE 수신) 시에만 한다 (event-driven - 몬스터 상태변경 트리거와 같은 원칙).
            //   매 tick 재broadcast는 잉여: 관찰자가 dead-reckon으로 보고 사이를 외삽해 메우므로,
            //   클라 보고(~167ms)를 받을 때 relay하면 그 보고가 재anchor(keyframe) 역할까지 겸한다.
        }

        // 현재 이동 상태를 broadcast 패킷으로 직렬화 - Channel 이 위치 바뀐 객체마다 호출.
        //   pkt : 직렬화 결과를 담을 패킷 버퍼 (호출자 소유)
        bool Player::SerializeBroadcast(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
        {
            Position pos = GetPos();   // GameObject const 접근자 (권위 위치)
            SC_MOVE_BROADCAST body{ m_playerId, GetMoveState(), GetDirection(), pos.x, pos.y, m_lastProcessedSeq};
            pkt.Begin(static_cast<USHORT>(PacketType::SC_MOVE_BROADCAST));   // 헤더 4B 예약 + 패킷 종류 태깅
            body.Serialize(pkt);                                            // playerId, moveState, direction, x, y, lastSeq 순서로 기록 (byte order 변환은 << 안에서)
            return pkt.End();                                                      // 맨 앞(offset 0)에 전체 길이 채워 넣음
        }

        // 초기 상태를 spawn 패킷으로 직렬화 - 새로 시야에 들어온 관찰자에게 보낼 첫 상태 (SerializeBroadcast 와 같은 패턴).
        //   pkt : 직렬화 결과를 담을 패킷 버퍼
        bool Player::SerializeSpawn(KYS::GAMECOMMON::PROTOCOL::CPacket& pkt) const
        {
            pkt.Begin(static_cast<USHORT>(PacketType::SC_SPAWN));   // 헤더 4B 예약 + 패킷 종류 태깅
            SC_SPAWN s;
            s.playerId = GetPlayerId();          // 현재 값 읽어 담음 (wire 경계)
            s.moveState = GetMoveState();
            s.direction = GetDirection();
            s.x = GetPos().x;
            s.y = GetPos().y;
            s.hp = GetHp();
            s.maxHp = GetMaxHp();
            ::wcscpy_s(s.name, GetName());       // 캐릭터 이름 동봉 - 관찰 클라가 스폰 즉시 이름표 표시
            s.Serialize(pkt);                      // 위 필드들을 버퍼에 기록
            return pkt.End();                             // 맨 앞에 전체 길이 채워 넣음
        }

        // 서버가 가진 권위 위치를 그대로 돌려준다 - Channel 이 클라가 보고한 위치와 맞는지 대조할 때 호출.
        //   상태를 바꾸지 않는 읽기 전용 (SetPos/SetDirty 안 함). 위치 예측 수식은 Update 에만 둔다.
        //   한 tick 오차는 허용 오차(MOVE_SYNC_TOLERANCE)가 흡수하므로 현재 위치만 반환해도 충분.
        Position Player::PosSync() const
        {
            return GetPos();
        }

        // 마지막으로 처리한 클라 입력 seq 기록 - broadcast 에 함께 실려 mover 가 자기 위치를 맞춰보는 기준이 된다.
        //   seq : 방금 처리한 클라 입력의 순번
        void Player::SetLastSeq(UINT32 seq)
        {
            m_lastProcessedSeq = seq;
        }

        // 마지막으로 처리한 클라 입력 seq.
        UINT32 Player::GetLastSeq() const
        {
            return m_lastProcessedSeq;
        }

        // 시야 집합에 객체 추가 (그 객체가 spawn 으로 보이기 시작할 때).
        void Player::AddVisible(GameObject* obj)
        {
            m_visibleObjects.insert(obj);
        }

        // 시야 집합에서 객체 제거 (despawn 으로 안 보이게 될 때).
        void Player::RemoveVisible(GameObject* obj)
        {
            m_visibleObjects.erase(obj);
        }

        // 그 객체가 지금 시야 안에 있는지.
        bool Player::IsVisible(GameObject* obj) const
        {
            return m_visibleObjects.find(obj) != m_visibleObjects.end();
        }

        // 시야 집합 전체 비움 (맵 전환 시).
        void Player::ClearVisible()
        {
            m_visibleObjects.clear();
        }

        // 시야 집합을 읽기 전용으로 넘김 (맵 전환 시 순회용).
        const std::unordered_set<GameObject*>& Player::GetVisibleObjects() const
        {
            return m_visibleObjects;
        }

        // 스킬 시전 가능 여부 판정 + 가능하면 쿨다운 소비 - 서버가 시전 간격을 강제해 변조 클라의 난사를 막는다.
        //   skillId : 시전할 스킬 번호 (0 = 평타, MP 소비 없음)
        bool Player::ConsumeForSkill(UINT32 skillId)
        {
            // 공용 쿨다운으로 시전 간격 제한. 서버가 빈도를 강제해, 변조 클라가 CS_SKILL 을 난사해
            //   SC_DAMAGE 를 여러 대상에 폭증시키는 과부하 공격을 막는다.
            //   m_skillCooldown 은 Player::Update 가 매 tick deltaTime 만큼 줄인다 (무적 카운트다운과 같은 방식).
            if (m_skillCooldown > 0.0f)
            {
                return false;   // 아직 쿨다운 중 -> 시전 무효 (조용히 무시)
            }
            m_skillCooldown = SKILL_COOLDOWN_SEC;   // 쿨다운 리셋 (GameDefines.h 시작값 0.5f)

            // skillId 0 = 평타 -> MP 소비 없음. 0 보다 크면 스킬 (지금은 MP 검사 최소, 나중에 m_mp 차감 자리).
            (void)skillId;
            return true;
        }

        // 리스폰 타이머 누산 시작 - 사망으로 전환되는 순간 호출.
        void Player::StartRespawn()
        {
            m_respawnTimer = 0.0f;   // 사망 순간부터 0 에서 누산 시작 (Update 가 키움)
        }

        // 지금 무적 상태인지 - 데미지 적용 직전 가드.
        bool Player::IsInvulnerable() const
        {
            return m_invulnUntil > 0.0f;
        }

        // 이번 tick 에 부활했는지 확인하고 플래그를 내린다 - Map::Update 가 SC_RESPAWN 을 한 번만 보내도록.
        bool Player::ConsumeRespawn()
        {
            if (!m_justRespawned) { return false; }
            m_justRespawned = false;   // 한 번 소비하면 내림 (tick 당 SC_RESPAWN 한 번만)
            return true;
        }

        // 변경된 저장 카테고리 비트를 켠다 - 나중에 그 부분만 골라 저장.
        //   category : 켤 카테고리 비트
        void Player::MarkDbDirty(UINT32 category)
        {
            m_dbDirty |= category;   // 카테고리 비트 누적 (OR)
        }

        // 저장 완료 후 dirty 비트를 모두 끈다.
        void Player::ClearDbDirty()
        {
            m_dbDirty = 0;           // 저장 후 전체 0
        }

        // 변경된 저장 카테고리 비트 조회 - 저장 루틴이 어디를 쓸지 판단.
        UINT32 Player::GetDbDirty() const
        {
            return m_dbDirty;
        }

        // wire 에 싣는 플레이어 ID 반환 (SC_MOVE_BROADCAST 등).
        PlayerId Player::GetPlayerId() const
        {
            return m_playerId;
        }

        // 플레이어 ID 설정 (spawn 때 Channel 이 발급).
        void Player::SetPlayerId(PlayerId id)
        {
            m_playerId = id;
        }

        // 조회 키로 쓰는 sid 반환 (GameObject::m_id, wire 엔 안 실음).
        UINT64 Player::GetSid() const
        {
            return GetId();   // GameObject::m_id = sid (조회 키). wire엔 안 실음
        }

        // 마나 조회 (DB 저장/로드 대상).
        int Player::GetMp() const
        {
            return m_mp;
        }

        // 마나 설정 (로드 시 적용, 소비 시 갱신).
        void Player::SetMp(int mp)
        {
            m_mp = mp;
        }

        // 캐릭터 이름 반환.
        const wchar_t* Player::GetName() const
        {
            return m_name;
        }

        // 캐릭터 이름 설정 (버퍼 크기 안에서 복사).
        void Player::SetName(const wchar_t* name)
        {
            wcscpy_s(m_name, WHISPER_NAME_MAX, name);
        }

        // 직전 채팅 시각(ms) 반환 - 채팅 도배 간격 제한 판정용.
        UINT64 Player::GetLastChatMs() const
        {
            return m_lastChatMs;
        }

        // 직전 채팅 시각(ms) 갱신.
        void Player::SetLastChatMs(UINT64 ms)
        {
            m_lastChatMs = ms;
        }

        // 행동 쿨다운 게이트 - 마지막 이 행동 이후 minIntervalMs 지났으면 true(+시각 갱신), 아니면 false(도배 드롭).
        //   채널 스레드 단독 소유라 락 불요. spawn 마다 ctor 가 m_lastActionMs 를 0 으로 리셋(풀 재사용 placement-new).
        //   [!] nowMs 는 반드시 프로세스 전역 단조 시계(::GetTickCount64())를 넘긴다 - m_lastActionMs 는 채널 이관 후에도 살아남으므로
        //       채널별 타이머(채널마다 epoch 상이)를 쓰면 이관 후 cross-epoch 비교로 UINT64 언더플로(쿨다운 우회)/오드롭이 난다.
        bool Player::TryAct(EPlayerAction action, UINT64 nowMs, UINT64 minIntervalMs)
        {
            UINT64& last = m_lastActionMs[static_cast<int>(action)];
            if (nowMs - last < minIntervalMs)
            {
                return false;   // 아직 쿨다운 - 도배 드롭
            }
            last = nowMs;
            return true;
        }

        // 최종 공격력 - 맨손 기준값에 장착 무기 공격력을 더한다. 전투 계산(CombatFormula)이 매 타격마다 읽음.
        int Player::GetAttackPower() const
        {
            return PLAYER_BASE_ATTACK_POWER + m_inventory.TotalAtkPower();
        }

        // 최종 방어력 - 장착 방어구 합. 받는 피해에서 감산 (최소 피해 보장은 전투 계산 쪽 책임).
        int Player::GetDefPower() const
        {
            return m_inventory.TotalDefPower();
        }

    }
}
