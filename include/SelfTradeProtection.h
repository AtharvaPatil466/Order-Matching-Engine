#pragma once

// SelfTradeProtection — per-participant self-trade prevention.
//
// Roadmap Phase 4, Week 13: Self-Trade Prevention
//
// Implements STP with four modes per participant:
//   CancelResting:      Cancel the resting order, execute nothing.
//   CancelIncoming:     Cancel the incoming order, leave resting.
//   CancelBoth:         Cancel both orders.
//   DecreaseAndCancel:  Decrease resting qty by match amount; cancel if zero.
//
// STP mode is stored per participant. The check is performed in the
// matching loop before executing a trade. If STP triggers, a cancel
// acknowledgment is generated but no trade.
//
// Zero allocation: uses pre-allocated cancel records via the existing
// ObjectPool and EventListener infrastructure.

#include "Types.h"
#include <cstdint>

namespace OrderMatcher {

enum class STPMode : uint8_t {
    None = 0,              // No self-trade prevention
    CancelResting = 1,     // Cancel resting order
    CancelIncoming = 2,    // Cancel incoming order
    CancelBoth = 3,        // Cancel both orders
    DecreaseAndCancel = 4  // Decrease resting qty, cancel if zero
};

struct STPResult {
    enum class Action : uint8_t {
        NoSelfTrade,       // Not a self-trade, proceed normally
        CancelResting,     // Cancel the resting order
        CancelIncoming,    // Cancel the incoming order
        CancelBoth,        // Cancel both orders
        DecreaseResting    // Decrease resting order quantity
    };

    Action action{Action::NoSelfTrade};
    Quantity decreaseAmount{0};  // For DecreaseAndCancel mode
};

class SelfTradeProtection {
public:
    // Check if two orders from the same participant would self-trade.
    // Returns the action to take.
    static STPResult check(ParticipantId incomingParticipant,
                           ParticipantId restingParticipant,
                           STPMode mode,
                           Quantity matchQty) {
        STPResult result;

        // Not a self-trade if different participants
        if (incomingParticipant != restingParticipant) {
            result.action = STPResult::Action::NoSelfTrade;
            return result;
        }

        // Same participant — apply STP mode
        switch (mode) {
        case STPMode::None:
            result.action = STPResult::Action::NoSelfTrade;
            break;
        case STPMode::CancelResting:
            result.action = STPResult::Action::CancelResting;
            break;
        case STPMode::CancelIncoming:
            result.action = STPResult::Action::CancelIncoming;
            break;
        case STPMode::CancelBoth:
            result.action = STPResult::Action::CancelBoth;
            break;
        case STPMode::DecreaseAndCancel:
            result.action = STPResult::Action::DecreaseResting;
            result.decreaseAmount = matchQty;
            break;
        }

        return result;
    }

    // Quick check: is this a self-trade that needs intervention?
    static bool isSelfTrade(ParticipantId incoming,
                            ParticipantId resting,
                            STPMode mode) {
        return incoming == resting && mode != STPMode::None;
    }
};

}  // namespace OrderMatcher
