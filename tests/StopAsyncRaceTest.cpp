// StopAsyncRaceTest — stopAsync() while producers are still submitting.
//
// Roadmap 1.7-G2 (LF-5, THR-7). stopAsync() cleared the request queues with
// nothing to stop a producer that had already passed the running check from
// pushing into one: the push landed in freed memory (ASan: heap-use-after-
// free), or in a queue whose worker had already left, so the order was acked
// and then never applied. The fix is an admission gate in front of the
// queues: stopAsync closes it and waits for every producer already inside to
// finish before the workers are told to stop.
//
// Each order the engine acks must reach its book (the book's listener sees
// an update for it). Run under ASan for the use-after-free, under TSan for
// the unsynchronised teardown, and plainly for the dropped acks.

#include "EventListener.h"
#include "MatchingEngine.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr int kRounds = 200;
constexpr int kProducers = 3;
constexpr SymbolId kSymbols[] = {1, 2};
constexpr uint64_t kAcceptedBeforeStop = 200;
constexpr OrderId kIdsPerProducer = 1'000'000;

// One per book, so one worker writes each; read only after stopAsync joined.
struct SeenListener : EventListener {
    std::unordered_set<OrderId> seen;
    void onOrderUpdate(const OrderUpdate& u) override { seen.insert(u.orderId); }
};

struct Producer {
    std::vector<std::pair<SymbolId, OrderId>> acked;
};

void produce(MatchingEngine& engine, int index, Producer& out,
             std::atomic<uint64_t>& acceptedTotal) {
    OrderId id = static_cast<OrderId>(index + 1) * kIdsPerProducer;
    for (uint64_t n = 0;; ++n) {
        const SymbolId sym = kSymbols[n % std::size(kSymbols)];
        ++id;
        // Bids far below any ask: every acked order rests, nothing trades.
        const SubmitResult r = engine.submitOrder(sym, id, 7, Side::Buy,
                                                  100 + static_cast<Price>(n % 50), 1,
                                                  OrderType::Limit);
        if (r.isAccepted()) {
            out.acked.emplace_back(sym, id);
            acceptedTotal.fetch_add(1, std::memory_order_relaxed);
        } else if (r.rejectReason == RejectReason::EngineStopped) {
            return;
        }
    }
}

bool runRound(int round) {
    MatchingEngine engine;
    SeenListener listeners[std::size(kSymbols)];
    for (size_t i = 0; i < std::size(kSymbols); ++i) {
        engine.addSymbol(kSymbols[i]);
        engine.getOrderBook(kSymbols[i])->setEventListener(&listeners[i]);
    }
    engine.startAsync(2, 1024);

    std::atomic<uint64_t> accepted{0};
    std::vector<Producer> producers(kProducers);
    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back(produce, std::ref(engine), p, std::ref(producers[p]),
                             std::ref(accepted));
    }
    while (accepted.load(std::memory_order_relaxed) < kAcceptedBeforeStop) {
        std::this_thread::yield();
    }
    engine.stopAsync();
    for (auto& t : threads) t.join();

    for (const Producer& p : producers) {
        for (const auto& [sym, id] : p.acked) {
            const size_t book = sym == kSymbols[0] ? 0 : 1;
            if (!listeners[book].seen.count(id)) {
                std::fprintf(stderr, "round %d: order %llu was acked and never applied\n",
                             round, static_cast<unsigned long long>(id));
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main() {
    for (int round = 0; round < kRounds; ++round) {
        if (!runRound(round)) std::abort();
    }
    std::printf("StopAsyncRaceTest passed (%d rounds)\n", kRounds);
    return 0;
}
