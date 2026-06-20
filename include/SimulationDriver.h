#pragma once

#include "SimulationParticipant.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "Types.h"

#include <future>
#include <memory>
#include <vector>

namespace OrderMatcher {

// ----------------------------------------------------------------------------
// SimulationDriver — orchestrates a set of SimulationParticipants.
//
// Usage:
//   MatchingEngine engine;
//   engine.start();
//   engine.addSymbol(1);
//
//   SimulationDriver driver(engine, 1);
//   driver.addParticipant(std::make_unique<NoiseTrader>(1));
//   driver.addParticipant(std::make_unique<MarketMaker>(2));
//   driver.run(1000);
//
//   const auto& hist = driver.history(); // 1000 snapshots
//
// Each tick:
//   1. Query book state (best bid/ask, depths, last trade price, volume).
//   2. Call onTick() on all participants in registration order.
//   3. Re-query book state and capture a TickSnapshot.
//
// Single-threaded: run() executes synchronously on the caller's thread.
// runAsync() wraps run() in std::async for fire-and-forget use.
// ----------------------------------------------------------------------------
class SimulationDriver {
public:
    // Snapshot captured after each tick.
    struct TickSnapshot {
        uint64_t tick;
        Price    bestBid;
        Price    bestAsk;
        Price    lastTradePrice;
        Quantity bidDepth;
        Quantity askDepth;
        Quantity volume;             // cumulative traded qty in this tick
        int64_t  makerNetPosition;  // from first MarketMaker in participant list (0 if none)
    };

    SimulationDriver(MatchingEngine& engine, SymbolId symbol);

    // Register a participant. The driver injects engine_ and symbol_ before
    // the first call to onTick().
    void addParticipant(std::unique_ptr<SimulationParticipant> p);

    // Like addParticipant but returns a raw (non-owning) pointer to the stored
    // participant so the caller can configure it before the first tick.
    SimulationParticipant* addParticipantAndGet(std::unique_ptr<SimulationParticipant> p);

    // Run numTicks ticks synchronously.
    void run(uint64_t numTicks);

    // Run numTicks ticks on a background thread.  Returns the future so the
    // caller can wait for completion if desired.
    std::future<void> runAsync(uint64_t numTicks);

    // Snapshot history — one entry per tick produced by the last run().
    const std::vector<TickSnapshot>& history() const { return history_; }
    void clearHistory() { history_.clear(); }

private:
    TickSnapshot captureSnapshot(uint64_t tick) const;

    MatchingEngine&                                   engine_;
    SymbolId                                          symbol_;
    std::vector<std::unique_ptr<SimulationParticipant>> participants_;
    std::vector<TickSnapshot>                         history_;
};

} // namespace OrderMatcher
