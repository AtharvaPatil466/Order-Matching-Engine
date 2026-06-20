#include "SimulationDriver.h"
#include "MarketMaker.h"

#include <limits>
#include <stdexcept>

namespace OrderMatcher {

SimulationDriver::SimulationDriver(MatchingEngine& engine, SymbolId symbol)
    : engine_(engine), symbol_(symbol) {}

void SimulationDriver::addParticipant(std::unique_ptr<SimulationParticipant> p) {
    addParticipantAndGet(std::move(p));
}

SimulationParticipant* SimulationDriver::addParticipantAndGet(
        std::unique_ptr<SimulationParticipant> p) {
    if (!p) throw std::invalid_argument("SimulationDriver: null participant");
    // Inject engine and symbol before storing
    p->engine_ = &engine_;
    p->symbol_ = symbol_;
    participants_.push_back(std::move(p));
    return participants_.back().get();
}

void SimulationDriver::run(uint64_t numTicks) {
    history_.reserve(history_.size() + numTicks);

    for (uint64_t tick = 0; tick < numTicks; ++tick) {
        // Query book state before participants act.
        // getBestAsk() returns std::numeric_limits<Price>::max() when there
        // are no asks; normalise to 0 so participants can use a simple > 0 test.
        constexpr Price NO_ASK = std::numeric_limits<Price>::max();
        OrderBook* book = engine_.getOrderBook(symbol_);
        Price    bestBid  = book ? book->getBestBid()  : 0;
        Price    rawAsk   = book ? book->getBestAsk()  : NO_ASK;
        Price    bestAsk  = (rawAsk == NO_ASK) ? 0 : rawAsk;
        Quantity bidDepth = 0;
        Quantity askDepth = 0;

        if (book) {
            // Accumulate top-of-book depth from snapshot (depth=1 is sufficient)
            auto snap = book->getSnapshot(1);
            if (snap.bidCount > 0) bidDepth = snap.bids[0].totalQuantity;
            if (snap.askCount > 0) askDepth = snap.asks[0].totalQuantity;
        }

        // Dispatch tick to all participants
        for (auto& p : participants_) {
            p->onTick(tick, bestBid, bestAsk, bidDepth, askDepth);
        }

        // Capture snapshot after participants have acted
        history_.push_back(captureSnapshot(tick));
    }
}

std::future<void> SimulationDriver::runAsync(uint64_t numTicks) {
    return std::async(std::launch::async, [this, numTicks]() {
        run(numTicks);
    });
}

SimulationDriver::TickSnapshot SimulationDriver::captureSnapshot(uint64_t tick) const {
    TickSnapshot snap{};
    snap.tick = tick;

    OrderBook* book = engine_.getOrderBook(symbol_);
    if (!book) return snap;

    constexpr Price NO_ASK = std::numeric_limits<Price>::max();
    snap.bestBid = book->getBestBid();
    Price rawAsk = book->getBestAsk();
    snap.bestAsk = (rawAsk == NO_ASK) ? 0 : rawAsk;

    // Retrieve last trade price and volume from the trade ring buffer.
    // The ring buffer is ordered oldest→newest; the last element is the most
    // recent trade.
    const auto& trades = book->getTradeRingBuffer();
    Quantity volumeThisTick = 0;
    if (trades.size() > 0) {
        // Accumulate all trades that arrived since the previous snapshot.
        // We use the ring buffer's sequential read (it is a circular buffer;
        // we traverse its size() items newest-to-oldest).
        //
        // For simplicity (and because the buffer's iterator goes newest first
        // when using reverse iteration), we use the public getSnapshot method
        // for last-trade info and compute volume as accumulated quantity for
        // any trade logged in this tick.  Since the buffer has no timestamp
        // per entry in ticks, we approximate by reading the full buffer and
        // reporting the last-trade price.  This is acceptable for simulation.
        auto mdSnap = book->getSnapshot(0);
        snap.lastTradePrice = mdSnap.lastTradePrice;
        snap.volume         = mdSnap.lastTradeQty;  // last trade qty
        (void)volumeThisTick;
    }

    // Depth from snapshot
    auto mdSnap = book->getSnapshot(1);
    if (mdSnap.bidCount > 0) snap.bidDepth = mdSnap.bids[0].totalQuantity;
    if (mdSnap.askCount > 0) snap.askDepth = mdSnap.asks[0].totalQuantity;

    // Find first MarketMaker to get its net position
    for (const auto& p : participants_) {
        const MarketMaker* mm = dynamic_cast<const MarketMaker*>(p.get());
        if (mm) {
            snap.makerNetPosition = mm->inventory();
            break;
        }
    }

    return snap;
}

} // namespace OrderMatcher
