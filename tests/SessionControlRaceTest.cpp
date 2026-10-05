// SessionControlRaceTest — a scheduler thread drives the session while the
// async workers take orders. Built to run under ThreadSanitizer.
//
// Roadmap 1.7-G1 (THR-1). Session control used to mutate worker-owned books,
// and the OCO/trade buffers the worker drains, from the caller's thread. The
// transitions and uncrosses now ride the workers' own queues; this pins that
// the whole session path — every phase SessionScheduler drives, including the
// close's DAY sweep — leaves each book's state, its listener and its engine
// observer (OCO executions, trade buffer) to the one worker that owns it.
//
// The per-book listener counts with PLAIN integers on purpose: the engine's
// contract is that a book's events arrive on its owning worker, so a second
// thread delivering any event to it is a data race TSan reports here.

#include "EventListener.h"
#include "MatchingEngine.h"
#include "SessionScheduler.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSymbols[] = {1, 2, 3, 4};
constexpr int kSessions = 30;
constexpr int kProducers = 2;
constexpr OrderId kIdsPerProducer = 1'000'000;

struct CountingListener : EventListener {
    uint64_t updates = 0;   // plain on purpose — see the header
    uint64_t trades = 0;
    void onOrderUpdate(const OrderUpdate&) override { ++updates; }
    void onTrade(const Trade&) override { ++trades; }
};

void produce(MatchingEngine& engine, int producer, const std::atomic<bool>& stop) {
    OrderId id = static_cast<OrderId>(producer + 1) * kIdsPerProducer;
    for (uint64_t n = 0; !stop.load(std::memory_order_acquire); ++n) {
        const SymbolId sym = kSymbols[n % std::size(kSymbols)];
        const Side side = (n & 1) ? Side::Sell : Side::Buy;
        const Price px = 100 + static_cast<Price>(n % 5);      // overlapping: crosses
        const TimeInForce tif = (n % 3 == 0) ? TimeInForce::DAY : TimeInForce::GTC;
        const OrderId a = ++id;
        engine.submitOrder(sym, a, 10 + producer, side, px, 10, OrderType::Limit,
                           0, 0, tif);
        if (n % 7 == 0) {
            const OrderId b = ++id;
            engine.submitOrder(sym, b, 10 + producer, side, px + 3, 10,
                               OrderType::Limit, 0, 0, TimeInForce::GTC);
            engine.registerOco(sym, a, b);
        }
    }
}

void driveSessions(MatchingEngine& engine) {
    const std::vector<SymbolId> universe(std::begin(kSymbols), std::end(kSymbols));
    std::atomic<uint64_t> now{0};
    SessionSchedule sched;
    SessionScheduler scheduler(engine, universe, sched);
    scheduler.setClock([&] { return now.load(std::memory_order_relaxed); });

    const uint64_t phases[] = {sched.preOpenMs, sched.openMs,
                               sched.closeAuctionMs, sched.closeMs};
    for (int s = 0; s < kSessions; ++s) {
        for (uint64_t t : phases) {
            now.store(t, std::memory_order_relaxed);
            scheduler.tick();
            std::this_thread::yield();
        }
        // A volatility pause and its reopening, then a fresh day. The batch
        // returns only once every book has made the flip, so the reopening
        // must find all of them still paused.
        engine.setTradingStateBatch(universe, TradingState::VolatilityAuction);
        const size_t reopened = engine.resumeVolatilityAuctions();
        if (reopened != universe.size()) {
            std::fprintf(stderr, "session %d: reopened %zu of %zu paused books\n",
                         s, reopened, universe.size());
            std::abort();
        }
        engine.uncrossBatch(universe);
        scheduler.resetSession();
    }
}

}  // namespace

int main() {
    MatchingEngine engine;
    std::array<CountingListener, std::size(kSymbols)> listeners;
    for (size_t i = 0; i < std::size(kSymbols); ++i) {
        engine.addSymbol(kSymbols[i]);
        engine.getOrderBook(kSymbols[i])->setEventListener(&listeners[i]);
    }
    engine.enableAuditTrail(true);   // turns on the engine observer's trade buffer
    engine.startAsync(2);

    std::atomic<bool> stop{false};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back(produce, std::ref(engine), p, std::cref(stop));
    }
    driveSessions(engine);
    stop.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();

    engine.waitForDrain();
    uint64_t updates = 0;
    for (size_t i = 0; i < std::size(kSymbols); ++i) {
        const OrderBook* book = engine.getOrderBook(kSymbols[i]);
        assert(book->validateIntegrity() && "a book was corrupted under session control");
        updates += listeners[i].updates;
    }
    engine.stopAsync();
    assert(updates > 0 && "no order flow reached the books");
    std::printf("SessionControlRaceTest passed (%llu order updates)\n",
                static_cast<unsigned long long>(updates));
    return 0;
}
