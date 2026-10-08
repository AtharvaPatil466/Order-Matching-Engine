// E2ELatencyRaceTest — read the engine's latency histograms while the
// workers are writing them.
//
// Roadmap 1.7-G3 (BENCH-8, LF-6). Each async worker records its end-to-end
// latency into its own LatencyTracker; getAggregateE2ELatency() merges them
// from whatever thread asks — the admin /metrics endpoint, the benchmarks'
// sampling loops. The buckets were plain integers, so every such read was a
// data race with the worker writing them (TSan reports it). The fields are
// now relaxed atomics written by their single owner, and a read is a
// race-free snapshot.
//
// Run under TSan; the plain build checks the snapshot stays coherent.

#include "MatchingEngine.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSymbols[] = {1, 2};
constexpr uint64_t kOrders = 200'000;
constexpr int kMinReads = 50;

void fail(const char* what, uint64_t a, uint64_t b) {
    std::fprintf(stderr, "%s (%llu vs %llu)\n", what, static_cast<unsigned long long>(a),
                 static_cast<unsigned long long>(b));
    std::abort();
}

}  // namespace

int main() {
    MatchingEngine engine;
    for (SymbolId s : kSymbols) engine.addSymbol(s);
    engine.startAsync(2);

    std::atomic<bool> done{false};
    int reads = 0;
    std::thread reader([&] {
        uint64_t lastCount = 0;
        while (!done.load(std::memory_order_acquire) || reads < kMinReads) {
            const LatencyTracker snap = engine.getAggregateE2ELatency();
            if (snap.getCount() < lastCount) fail("count went backwards", snap.getCount(), lastCount);
            if (snap.getP50() > snap.getP99()) fail("p50 above p99", snap.getP50(), snap.getP99());
            lastCount = snap.getCount();
            ++reads;
        }
    });

    // Crossing flow on both books: buys and sells at one price, so the books
    // stay small and every order is matched or rests briefly.
    for (uint64_t n = 0; n < kOrders; ++n) {
        const SymbolId sym = kSymbols[n % std::size(kSymbols)];
        const Side side = (n / 2) % 2 ? Side::Sell : Side::Buy;
        while (!engine.submitOrder(sym, n + 1, 7, side, 100, 1, OrderType::Limit).isAccepted()) {
            std::this_thread::yield();   // queue backpressure: retry
        }
    }
    engine.waitForDrain();
    done.store(true, std::memory_order_release);
    reader.join();

    const LatencyTracker total = engine.getAggregateE2ELatency();
    engine.stopAsync();
    if (total.getCount() != kOrders) fail("not every order was timed", total.getCount(), kOrders);
    std::printf("E2ELatencyRaceTest passed (%d concurrent reads)\n", reads);
    return 0;
}
