// ShutdownCancelRaceTest — H1. Cancels racing gracefulShutdown() must not
// dereference pooled memory the teardown has already freed.
//
// Pre-fix two things collided: submitCancel checked only running_, not
// shuttingDown_, so a cancel was admitted while the sweeps were returning
// orders to orderPool_; and it then called releasePosition(book->getOrder(id)),
// dereferencing an unlocked raw pointer into that pool. Under ASan this is a
// heap-use-after-free; in Release it silently corrupts a participant's
// position.
//
// IMPORTANT — what this test can and cannot prove.
//
// It CANNOT prove absence of the use-after-free, and no sanitizer test can:
//
//   * ASan is structurally blind to it. Orders live in an ObjectPool, whose
//     deallocate() returns the slot to a freelist and never releases memory to
//     the allocator (MemoryPool.h:75). The recycled slot is still a valid
//     allocation, so reading it is a LOGICAL use-after-free, not a heap one.
//     ASan has nothing to poison and reports nothing. Verified: the pre-fix
//     code runs this scenario clean under ASan.
//   * TSan rarely catches it either — the window between getOrder() returning
//     a pointer and releasePosition() dereferencing it is a few instructions,
//     and once the sweep has removed the order getOrder() returns nullptr and
//     releasePosition() early-returns. Verified: pre-fix runs clean under TSan
//     too.
//
// So the correctness argument for H1 is by inspection, and what this test
// pins deterministically is the SHUTDOWN GATE, which is observable:
// pre-fix every cancel was admitted during teardown (accepted=2000,
// refused=0); post-fix they are refused with EngineStopped. The concurrent
// phase remains a crash/corruption smoke test on top of that.

#include "MatchingEngine.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using namespace OrderMatcher;

int main() {
    constexpr SymbolId kSymbol = 1;
    constexpr int kOrders = 2000;
    constexpr int kCancelThreads = 4;

    for (int round = 0; round < 5; ++round) {
        MatchingEngine engine;
        engine.addSymbol(kSymbol);
        engine.getOrderBook(kSymbol)->setCircuitBreakerThreshold(1e9);
        // Position limits ON: this is what makes releasePosition run, and
        // releasePosition is what dereferenced the freed Order.
        engine.setPositionLimit(7, 1'000'000);
        engine.start();

        // DAY, not GTC: gracefulShutdown() runs cancelDayOrders(), which is
        // the sweep that frees these orders back to orderPool_ while the
        // cancel threads below are dereferencing them. GTC orders are never
        // swept, so there would be no teardown to race.
        for (int i = 1; i <= kOrders; ++i) {
            engine.submitOrder(kSymbol, i, 7, Side::Buy, 10000, 1,
                               OrderType::Limit, /*stopPrice=*/0,
                               /*displayQty=*/0, TimeInForce::DAY);
        }

        std::atomic<bool> go{false};
        std::atomic<int>  accepted{0}, refused{0};
        std::vector<std::thread> cancellers;
        for (int t = 0; t < kCancelThreads; ++t) {
            cancellers.emplace_back([&, t] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 1 + t; i <= kOrders; i += kCancelThreads) {
                    auto r = engine.submitCancel(kSymbol, i);
                    (r.isAccepted() ? accepted : refused)
                        .fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        std::thread shutter([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            engine.gracefulShutdown();
        });

        go.store(true, std::memory_order_release);
        for (auto& t : cancellers) t.join();
        shutter.join();

        assert(accepted.load() + refused.load() == kOrders &&
               "cancel accounting lost a request");

        // THE DETERMINISTIC ASSERTION. gracefulShutdown() has returned, so the
        // sweeps have run and the pool is being torn down: every further
        // cancel must be turned away by the shuttingDown_ gate, never admitted
        // into a path that dereferences a recycled Order. Pre-fix submitCancel
        // checked only running_, so these were all admitted.
        for (int i = 1; i <= 32; ++i) {
            auto r = engine.submitCancel(kSymbol, i);
            assert(!r.isAccepted() &&
                   "cancel admitted after gracefulShutdown() returned");
            assert(r.rejectReason == RejectReason::EngineStopped &&
                   "post-shutdown cancel must be refused by the shutdown gate");
        }

        std::printf("round %d: accepted=%d refused=%d; post-shutdown cancels "
                    "all refused with EngineStopped\n",
                    round, accepted.load(), refused.load());
    }

    std::puts("ShutdownCancelRaceTest passed");
    return 0;
}
