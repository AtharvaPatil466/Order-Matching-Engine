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

// What to do when a participant's incoming order would cross their own resting
// order. Detection is unconditional (OrderBook::checkSMP); this only selects the
// action. EVERY mode prevents the self-cross — there is no setting that permits
// one. Self-matching is prohibited at essentially every regulated venue, so the
// safe default is to prevent it even when no explicit mode is configured; a mode
// named `None` that permitted wash trades would be an unsafe default regardless
// of what the name suggests, which is why mode 0 is named for what it does.
enum class STPMode : uint8_t {
    // No explicit mode configured; incoming order is cancelled on self-cross
    // (safe default). Renamed from `None`, whose name implied "no prevention"
    // and contradicted both the behaviour and the four independent tests
    // asserting prevention-by-default. STPMode is never serialized or put on
    // the wire, so the rename is source-only.
    DefaultCancelIncoming = 0,
    CancelResting = 1,     // Cancel resting order
    CancelIncoming = 2,    // Cancel incoming order
    CancelBoth = 3,        // Cancel both orders
    DecreaseAndCancel = 4  // Decrease resting qty, cancel if zero
};

struct STPResult {
    enum class Action : uint8_t {
        // Either a genuine non-self-trade (different participants), or a
        // self-cross by a participant with no explicit mode configured. In
        // match() only the second reading is reachable, because checkSMP has
        // already established same-participant: the incoming order is
        // cancelled and reported CancelledBySTP.
        NoSelfTrade,
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
        case STPMode::DefaultCancelIncoming:
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

    // Is this a self-trade that needs intervention? Same participant is the
    // whole test: every mode intervenes, including the unconfigured default,
    // so the mode does not enter into it. Previously this excluded
    // STPMode::None and so answered `false` for the exact case the engine was
    // in fact preventing.
    static bool isSelfTrade(ParticipantId incoming,
                            ParticipantId resting,
                            STPMode /*mode*/) {
        return incoming == resting;
    }
};

}  // namespace OrderMatcher
