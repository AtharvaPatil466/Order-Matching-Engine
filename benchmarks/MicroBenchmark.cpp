#include <benchmark/benchmark.h>
#include "OrderBook.h"
#include "Types.h"
#include "Utils.h"

using namespace OrderMatcher;

// Benchmark Adding Orders (No Match) - passive orders far from spread
static void BM_AddOrder_NoMatch(benchmark::State& state) {
    OrderBook book;
    OrderId id = 1;
    ParticipantId participant = 1;

    for (auto _ : state) {
        book.addOrder(id, participant, Side::Buy, 1000000 - (id % 1000), 10, OrderType::Limit);
        id++;
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark Matching Logic - aggressive orders that cross the spread
static void BM_MatchOrder(benchmark::State& state) {
    OrderBook book;
    OrderId id = 1;
    ParticipantId participant = 1;

    // Pre-seed book with enough sell liquidity
    for (int i = 0; i < 100000; ++i) {
        book.addOrder(id++, participant, Side::Sell, 1000000 + i, 10, OrderType::Limit);
    }

    ParticipantId buyer = 2;
    for (auto _ : state) {
        book.addOrder(id++, buyer, Side::Buy, 2000000, 10, OrderType::Limit);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark Cancel Orders
static void BM_CancelOrder(benchmark::State& state) {
    OrderBook book;
    OrderId id = 1;
    ParticipantId participant = 1;

    // Pre-populate
    size_t count = 100000;
    for (size_t i = 0; i < count; ++i) {
        book.addOrder(id++, participant, Side::Buy, 900000 + (i % 10000), 10, OrderType::Limit);
    }

    OrderId cancelId = 1;
    for (auto _ : state) {
        book.cancelOrder(cancelId++);
        if (cancelId > static_cast<OrderId>(count)) {
            state.PauseTiming();
            cancelId = id;
            for (size_t i = 0; i < count; ++i) {
                book.addOrder(id++, participant, Side::Buy, 900000 + (i % 10000), 10, OrderType::Limit);
            }
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark Market Data Snapshot
static void BM_GetSnapshot(benchmark::State& state) {
    OrderBook book;
    OrderId id = 1;
    ParticipantId participant = 1;

    // Build a realistic book
    for (int i = 0; i < 1000; ++i) {
        book.addOrder(id++, participant, Side::Buy, 990000 - i * 10, 100, OrderType::Limit);
        book.addOrder(id++, participant, Side::Sell, 1010000 + i * 10, 100, OrderType::Limit);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(book.getSnapshot(10));
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_AddOrder_NoMatch);
BENCHMARK(BM_MatchOrder);
BENCHMARK(BM_CancelOrder);
BENCHMARK(BM_GetSnapshot);

BENCHMARK_MAIN();
