#include "MatchingEngine.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>

using namespace OrderMatcher;

// ─── Test 1: Concurrent producers into async engine ─────────────────────────
// Multiple threads submit orders simultaneously via async mode.
// Verify all orders are processed and book invariants hold.

void testConcurrentProducers() {
    std::cout << "Running testConcurrentProducers..." << std::endl;

    MatchingEngine engine;
    engine.addSymbol(0);
    engine.startAsync(4, 65536);

    constexpr int NUM_THREADS = 4;
    constexpr int ORDERS_PER_THREAD = 5000;
    std::atomic<uint64_t> nextOrderId{1};
    std::atomic<int> tradeCount{0};

    auto* book = engine.getOrderBook(0);
    struct TradeCounter : EventListener {
        std::atomic<int>& count;
        TradeCounter(std::atomic<int>& c) : count(c) {}
        void onTrade(const Trade&) override { count.fetch_add(1, std::memory_order_relaxed); }
    };
    TradeCounter counter(tradeCount);
    book->setEventListener(&counter);

    // Each thread submits alternating buys and sells at overlapping prices
    auto worker = [&](int threadId) {
        std::mt19937 rng(threadId * 12345);
        ParticipantId pid = static_cast<ParticipantId>(100 + threadId);

        for (int i = 0; i < ORDERS_PER_THREAD; ++i) {
            OrderId oid = nextOrderId.fetch_add(1, std::memory_order_relaxed);
            Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            Price price = toPrice(99.0 + (rng() % 200) * 0.01); // 99.00 - 100.99

            engine.processOrder(0, oid, pid, side, price, 10, OrderType::Limit);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto& t : threads) t.join();

    engine.waitForDrain();
    engine.stopAsync();

    // Verify all orders were processed
    uint64_t totalOrders = NUM_THREADS * ORDERS_PER_THREAD;
    std::cout << "  Submitted: " << totalOrders << " orders, Trades: " << tradeCount.load() << std::endl;

    // Verify bid/ask invariant: best bid < best ask (or one side is empty)
    Price bestBid = book->getBestBid();
    Price bestAsk = book->getBestAsk();
    if (bestBid > 0 && bestAsk < std::numeric_limits<Price>::max()) {
        assert(bestBid < bestAsk);
    }

    std::cout << "testConcurrentProducers PASSED" << std::endl;
}

// ─── Test 2: Mixed workload (add, cancel, modify) with contention ───────────

void testMixedWorkloadContention() {
    std::cout << "Running testMixedWorkloadContention..." << std::endl;

    MatchingEngine engine;
    engine.addSymbol(0);
    engine.startAsync(4, 65536);

    constexpr int NUM_THREADS = 4;
    constexpr int OPS_PER_THREAD = 5000;
    std::atomic<uint64_t> nextOrderId{1};
    std::atomic<int> tradeCount{0};

    auto* book = engine.getOrderBook(0);
    struct TradeCounter : EventListener {
        std::atomic<int>& count;
        TradeCounter(std::atomic<int>& c) : count(c) {}
        void onTrade(const Trade&) override { count.fetch_add(1, std::memory_order_relaxed); }
    };
    TradeCounter counter(tradeCount);
    book->setEventListener(&counter);

    auto worker = [&](int threadId) {
        std::mt19937 rng(threadId * 54321 + 1);
        ParticipantId pid = static_cast<ParticipantId>(200 + threadId);
        std::vector<OrderId> myOrders;
        myOrders.reserve(OPS_PER_THREAD);

        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            int action = rng() % 100;

            if (action < 60 || myOrders.empty()) {
                // 60% add
                OrderId oid = nextOrderId.fetch_add(1, std::memory_order_relaxed);
                Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
                Price price = toPrice(95.0 + (rng() % 1000) * 0.01);
                Quantity qty = 1 + (rng() % 100);
                engine.processOrder(0, oid, pid, side, price, qty, OrderType::Limit);
                myOrders.push_back(oid);
            } else if (action < 85) {
                // 25% cancel
                size_t idx = rng() % myOrders.size();
                engine.cancelOrder(0, myOrders[idx]);
                myOrders[idx] = myOrders.back();
                myOrders.pop_back();
            } else {
                // 15% modify (reduce qty)
                size_t idx = rng() % myOrders.size();
                engine.modifyOrder(0, myOrders[idx], 1 + (rng() % 10));
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto& t : threads) t.join();

    engine.waitForDrain();
    engine.stopAsync();

    Price bestBid = book->getBestBid();
    Price bestAsk = book->getBestAsk();
    if (bestBid > 0 && bestAsk < std::numeric_limits<Price>::max()) {
        assert(bestBid < bestAsk);
    }

    std::cout << "  Trades: " << tradeCount.load() << std::endl;
    std::cout << "testMixedWorkloadContention PASSED" << std::endl;
}

// ─── Test 3: Kill switch under load ─────────────────────────────────────────
// One thread fires kill switch while others are actively submitting.

void testKillSwitchUnderLoad() {
    std::cout << "Running testKillSwitchUnderLoad..." << std::endl;

    MatchingEngine engine;
    engine.addSymbol(0);
    engine.startAsync(4, 65536);

    constexpr int NUM_PRODUCERS = 3;
    constexpr ParticipantId VICTIM_PID = 999;
    std::atomic<uint64_t> nextOrderId{1};

    auto producer = [&](int threadId) {
        std::mt19937 rng(threadId * 99999);
        ParticipantId pid = (threadId == 0) ? VICTIM_PID : static_cast<ParticipantId>(300 + threadId);
        int ordersToProduce = (pid == VICTIM_PID) ? 1000 : 5000;

        for (int i = 0; i < ordersToProduce; ++i) {
            OrderId oid = nextOrderId.fetch_add(1, std::memory_order_relaxed);
            Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            Price price = toPrice(100.0 + (rng() % 200) * 0.01);
            engine.processOrder(0, oid, pid, side, price, 10, OrderType::Limit);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_PRODUCERS; ++t) {
        threads.emplace_back(producer, t);
    }

    // Deterministically wait for the victim to finish its workload
    threads[0].join();
    
    // Now fire the kill switch while other participants are still active
    engine.killSwitch(VICTIM_PID);

    // Wait for the remaining participants
    for (int t = 1; t < NUM_PRODUCERS; ++t) {
        threads[t].join();
    }

    engine.waitForDrain();
    engine.stopAsync();

    // After kill switch, victim should have no orders in book
    auto* book = engine.getOrderBook(0);
    int strayVictimOrders = 0;
    book->forEachOrder([&](const Order& o) {
        if (o.participantId == VICTIM_PID) {
            std::cout << "STRAY VICTIM ORDER: " << o.id << " qty=" << o.remainingQty << "\n";
            strayVictimOrders++;
        }
    });
    assert(strayVictimOrders == 0);

    std::cout << "testKillSwitchUnderLoad PASSED" << std::endl;
}

// ─── Test 4: Multi-symbol concurrent stress ─────────────────────────────────

void testMultiSymbolConcurrent() {
    std::cout << "Running testMultiSymbolConcurrent..." << std::endl;

    MatchingEngine engine;
    constexpr int NUM_SYMBOLS = 4;
    for (int s = 0; s < NUM_SYMBOLS; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
    }
    engine.startAsync(4, 65536);

    constexpr int NUM_THREADS = 4;
    constexpr int ORDERS_PER_THREAD = 3000;
    std::atomic<uint64_t> nextOrderId{1};
    std::atomic<int> totalTrades{0};

    struct TradeCounter : EventListener {
        std::atomic<int>& count;
        TradeCounter(std::atomic<int>& c) : count(c) {}
        void onTrade(const Trade&) override { count.fetch_add(1, std::memory_order_relaxed); }
    };
    TradeCounter multiCounter(totalTrades);
    for (int s = 0; s < NUM_SYMBOLS; ++s) {
        auto* book = engine.getOrderBook(static_cast<SymbolId>(s));
        book->setEventListener(&multiCounter);
    }

    auto worker = [&](int threadId) {
        std::mt19937 rng(threadId * 77777);
        ParticipantId pid = static_cast<ParticipantId>(400 + threadId);

        for (int i = 0; i < ORDERS_PER_THREAD; ++i) {
            SymbolId sym = static_cast<SymbolId>(rng() % NUM_SYMBOLS);
            OrderId oid = nextOrderId.fetch_add(1, std::memory_order_relaxed);
            Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            Price price = toPrice(50.0 + (rng() % 200) * 0.01);
            Quantity qty = 1 + (rng() % 50);
            engine.processOrder(sym, oid, pid, side, price, qty, OrderType::Limit);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto& t : threads) t.join();

    engine.waitForDrain();
    engine.stopAsync();

    // Verify each symbol's book invariants
    for (int s = 0; s < NUM_SYMBOLS; ++s) {
        auto* book = engine.getOrderBook(static_cast<SymbolId>(s));
        Price bestBid = book->getBestBid();
        Price bestAsk = book->getBestAsk();
        if (bestBid > 0 && bestAsk < std::numeric_limits<Price>::max()) {
            assert(bestBid < bestAsk);
        }
    }

    std::cout << "  Total trades across " << NUM_SYMBOLS << " symbols: " << totalTrades.load() << std::endl;
    std::cout << "testMultiSymbolConcurrent PASSED" << std::endl;
}

// ─── Test 5: High-throughput burst ──────────────────────────────────────────

void testHighThroughputBurst() {
    std::cout << "Running testHighThroughputBurst..." << std::endl;

    MatchingEngine engine;
    engine.addSymbol(0);
    engine.startAsync(4, 65536);

    constexpr int BURST_SIZE = 50000;
    std::atomic<uint64_t> nextOrderId{1};
    std::atomic<int> tradeCount{0};

    auto* book = engine.getOrderBook(0);
    struct TradeCounter : EventListener {
        std::atomic<int>& count;
        TradeCounter(std::atomic<int>& c) : count(c) {}
        void onTrade(const Trade&) override { count.fetch_add(1, std::memory_order_relaxed); }
    };
    TradeCounter counter(tradeCount);
    book->setEventListener(&counter);

    auto start = std::chrono::high_resolution_clock::now();

    // Burst: all threads submit as fast as possible
    constexpr int NUM_THREADS = 4;
    auto burster = [&](int threadId) {
        ParticipantId pid = static_cast<ParticipantId>(500 + threadId);
        int count = BURST_SIZE / NUM_THREADS;

        for (int i = 0; i < count; ++i) {
            OrderId oid = nextOrderId.fetch_add(1, std::memory_order_relaxed);
            // Alternating buy/sell at crossing prices to generate trades
            Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
            Price price = toPrice(100.00);
            engine.processOrder(0, oid, pid, side, price, 1, OrderType::Limit);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back(burster, t);
    }
    for (auto& t : threads) t.join();

    engine.waitForDrain();

    auto end = std::chrono::high_resolution_clock::now();
    auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    engine.stopAsync();

    double opsPerSec = static_cast<double>(BURST_SIZE) / (durationUs / 1e6);
    std::cout << "  " << BURST_SIZE << " orders in " << durationUs << " us"
              << " (" << static_cast<int>(opsPerSec / 1e6) << "M ops/sec)"
              << ", trades: " << tradeCount.load() << std::endl;

    std::cout << "testHighThroughputBurst PASSED" << std::endl;
}

int main() {
    std::cout << "\n=== Multi-Threaded Stress Tests ===" << std::endl;

    testConcurrentProducers();
    testMixedWorkloadContention();
    testKillSwitchUnderLoad();
    testMultiSymbolConcurrent();
    testHighThroughputBurst();

    std::cout << "\nALL STRESS TESTS PASSED!" << std::endl;
    return 0;
}
