#include "pch_serverapp.h"
#include "TradeState.h"

namespace KYS
{
    namespace SERVERAPP
    {
        void TradeState::Reset()
        {
            partnerSid = 0;
            state = ETradeState::NONE;
            accepted = false;
            for (int i = 0; i < MAX_TRADE_SLOTS; ++i)
            {
                offer[i].used = false;
                offer[i].bagSlot = 0;
                offer[i].uid = 0;
                offer[i].templateId = 0;
                offer[i].quantity = 0;
            }
        }

        int TradeState::OfferedQuantityFrom(int bagSlot) const
        {
            int total = 0;
            for (int i = 0; i < MAX_TRADE_SLOTS; ++i)
            {
                if (offer[i].used && offer[i].bagSlot == bagSlot) { total += offer[i].quantity; }
            }
            return total;
        }
    }
}
