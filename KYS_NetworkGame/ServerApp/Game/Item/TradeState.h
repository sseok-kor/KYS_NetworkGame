#pragma once
#include "../../../GameServer/Types/Defines.h"   // UINT64 / BYTE
#include "../../../GameCommon/GameDefines.h"     // MAX_TRADE_SLOTS

namespace KYS
{
    namespace SERVERAPP
    {
        // 거래 진행 단계 (플레이어별 서버 내부 상태 - wire 엔 안 실림)
        enum class ETradeState : BYTE
        {
            NONE        = 0,   // 거래 없음
            PENDING_OUT = 1,   // 내가 요청을 보내고 상대 응답 대기
            PENDING_IN  = 2,   // 상대 요청을 받고 내 응답 대기
            OPEN        = 3,   // 거래창 열림 (오퍼 올리기/수락 진행)
        };

        // 거래창 한 칸에 올린 오퍼. 원본 가방 칸 번호를 기억한다 -
        //   커밋 때 어디서 뺄지 + 거래 중 그 칸의 다른 조작을 잠그는 근거.
        struct TradeOfferSlot
        {
            bool   used;
            BYTE   bagSlot;      // 내 가방 어느 칸에서 올렸나
            UINT64 uid;          // 그 칸 실물의 uid (거래창 표시용)
            int    templateId;
            int    quantity;     // 올린 수량 (스택 일부 가능)
        };

        // 플레이어 한 명의 거래 상태 (rAthena sd->deal 대응 - Player 값 멤버, 채널 스레드 전용이라 락 없음).
        //   오퍼가 바뀌면 서버가 양측 accepted 를 강제로 내린다 (WoW식 - 막판 바꿔치기 스캠 방어).
        struct TradeState
        {
            UINT64         partnerSid;   // 상대 세션 (0 = 없음)
            ETradeState    state;
            bool           accepted;     // 내 수락 여부
            TradeOfferSlot offer[MAX_TRADE_SLOTS];

            TradeState() { Reset(); }

            void Reset();                              // 거래 종결/취소 시 전체 초기화
            bool IsTrading() const { return state != ETradeState::NONE; }
            int  OfferedQuantityFrom(int bagSlot) const;   // 이 가방 칸에서 이미 올린 총 수량 (초과 올리기 차단)
        };
    }
}
