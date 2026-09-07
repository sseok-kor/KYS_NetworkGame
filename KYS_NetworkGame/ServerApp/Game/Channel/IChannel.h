#pragma once
#include "../GameServer/Types/Defines.h"
#include "../GameServer/Core/Thread/IJob.h"


namespace KYS
{
    namespace SERVERAPP
    {
        class Monster;   // 전방 선언 - FindMonsterForPath 반환(길찾기 결과 재주입용 재조회). 구현은 .cpp가 Monster.h include.

        // 게임 채널의 추상 통로 - 라이브러리가 받은 네트워크 사건(연결/패킷/종료)을 이 인터페이스로 게임측에 넘긴다.
        // 구현은 Channel 하나뿐 (ChannelManager는 IChannel을 보유/위임만).
        class IChannel
        {
        public:
            virtual ~IChannel() = default;

            IChannel(const IChannel&) = delete;
            IChannel& operator=(const IChannel&) = delete;
            IChannel(IChannel&&) = delete;
            IChannel& operator=(IChannel&&) = delete;

            // job을 채널 critical mailbox에 넣는다 (로그인/전투/채팅/포탈/종료 - Worker enqueue, Channel drain).
            //   반환 = 적재 성공 여부. 성공/실패 모두 job 소유는 mailbox 로 넘어간다 - false(큐 포화 DROP)면
            //   mailbox 가 job 을 이미 delete한 것이므로 호출자는 회수(delete)하지 않는다. bool 은 drop 계측용.
            virtual bool EnqueueJob(KYS::GAMESERVER::THREAD::IJob* job) = 0;
            // CS_MOVE job을 movement mailbox에 넣는다 (과부하 시 먼저 shed - dead-reckoning이 메움). lane 분리로 critical을 우선 처리(best-effort 보존, 소진 시 DROP 계측).
            virtual void EnqueueMoveJob(KYS::GAMESERVER::THREAD::IJob* job) = 0;
            // 세션이벤트(enter/leave)를 별도 큐에 넣는다 (절대 드롭 금지)
            virtual void EnqueueSessionEvent(KYS::GAMESERVER::THREAD::IJob* job) = 0;
            // 이 채널의 id 반환 (전부 게임 채널, 0..N-1)
            virtual int  GetChannelId() const = 0;

            // 길찾기 결과 재주입 전용 - mapId/monsterId 로 살아있는(존재+!IsDead) 몬스터 재조회 (없으면 nullptr).
            //   PathResultJob 이 채널 스레드에서 호출 (포인터 결과 금지 - id 로 재조회해 stale 봉인).
            virtual Monster* FindMonsterForPath(int mapId, UINT32 monsterId) = 0;

            // 채널 진입 (로그인 - 캐릭터 입장)
            virtual void OnChannelEnter(UINT64 sid) = 0;
            // 인게임 채널 변경의 도착측 진입 (대상 채널 스레드가 실행 - 새 그리드 등록 + 결과/CharInfo 송신)
            virtual void OnChannelChangeEnter(UINT64 sid) = 0;
            // 채널 이동으로 떠남 (세션은 살아있음)
            virtual void OnChannelLeave(UINT64 sid) = 0;
            // 연결 종료로 떠남 (세션 사망 - 게임 상태 회수 트리거)
            virtual void OnClientLeave(UINT64 sid, EDisconnectReason reason) = 0;
            // 완성 패킷 도착 (형식 파싱/PacketType 분기는 구현이 수행)
            virtual void OnRecv(UINT64 sid, const BYTE* data, int size) = 0;

        protected:
            IChannel() = default;  // 직접 인스턴스화 차단 (추상 통로)
        };
    }
}
