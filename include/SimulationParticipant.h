#pragma once

#include "Types.h"

// Forward declarations — participants need these but we do not include the
// heavy matching-engine or order-book headers here.
namespace OrderMatcher {
class MatchingEngine;
class SimulationDriver;
} // namespace OrderMatcher

namespace OrderMatcher {

// ----------------------------------------------------------------------------
// SimulationParticipant — abstract base for every simulated market actor.
//
// Lifecycle:
//   1. Construct the concrete participant with whatever parameters it needs.
//   2. Pass a std::unique_ptr<SimulationParticipant> to
//      SimulationDriver::addParticipant().
//   3. SimulationDriver sets engine_ and symbol_ before the first tick.
//   4. Every tick the driver calls onTick() with the current book state.
//
// Derived classes must implement:
//   - onTick()  — react to market conditions (submit/cancel orders, etc.)
//   - id()      — return a stable, unique participant identifier
// ----------------------------------------------------------------------------
class SimulationParticipant {
public:
    virtual ~SimulationParticipant() = default;

    // Called once per tick with the current best-bid/ask and depth.
    // bestBid == 0 or bestAsk == 0 means that side has no resting orders.
    virtual void onTick(uint64_t tickNum,
                        Price    bestBid,
                        Price    bestAsk,
                        Quantity bidDepth,
                        Quantity askDepth) = 0;

    // Returns the participant's unique identifier (set at construction).
    virtual ParticipantId id() const = 0;

protected:
    // Injected by SimulationDriver::addParticipant() before the first tick.
    MatchingEngine* engine_ = nullptr;
    SymbolId        symbol_ = 0;

    friend class SimulationDriver;
};

} // namespace OrderMatcher
