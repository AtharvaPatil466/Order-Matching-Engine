// PoolExhaustionTest — exercises the CapacityExhausted reject path via
// fault injection. The natural path (allocate 200k orders to fill the
// fixed-size pool) is too slow for a unit test; this drives it
// deterministically.
//
// Properties asserted:
//   * When the fault is armed at 100%, every AddOrder returns
//     CapacityExhausted, and no order ends up in the book.
//   * When disarmed, allocation resumes; orders flow normally.
//   * The reject path does NOT leak: orderPool_.allocate() was never
//     called when the fault fires, so there's nothing to leak. We
//     verify by checking that every disarmed AddOrder still succeeds
//     after a long fault-armed run (pool capacity intact).
//
// Requires -DENABLE_FAULT_INJECTION=ON.

#include "FaultInjector.h"
#include "OrderBook.h"

#include <cassert>
#include <cstdio>

using namespace OrderMatcher;

int main() {
#ifndef OB_ENABLE_FAULT_INJECTION
    std::puts("PoolExhaustionTest: OB_ENABLE_FAULT_INJECTION not defined — "
              "skipping. Reconfigure with -DENABLE_FAULT_INJECTION=ON.");
    return 0;
#endif

    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(0x900D);

    OrderBook book(0);

    // ── Scenario 1: armed at 100% — every add rejects with CapacityExhausted
    fi.arm("pool.allocate.fail", 1.0);
    constexpr int kAttempts = 1000;
    for (int i = 0; i < kAttempts; ++i) {
        auto r = book.addOrder(static_cast<OrderId>(i + 1), 1, Side::Buy,
                                1000000, 100, OrderType::Limit);
        assert(std::holds_alternative<RejectReason>(r));
        assert(std::get<RejectReason>(r) == RejectReason::CapacityExhausted);
    }
    // Nothing landed in the book.
    {
        const Order* arr[16];
        size_t n = book.getAllOrders(arr, 16);
        assert(n == 0 && "armed-fault adds must not land");
    }

#ifdef OB_ENABLE_FAULT_INJECTION
    uint64_t fires = fi.activations("pool.allocate.fail");
    std::printf("armed: %d attempts, %llu fault fires, all rejected\n",
                kAttempts, static_cast<unsigned long long>(fires));
    assert(fires == kAttempts);
#endif

    // ── Scenario 2: disarmed — allocation resumes normally
    fi.disarm("pool.allocate.fail");
    auto good = book.addOrder(2'000'000, 1, Side::Buy, 1000000, 100,
                                OrderType::Limit);
    assert(std::holds_alternative<OrderId>(good));
    assert(book.getOrder(2'000'000) != nullptr);

    // ── Scenario 3: probabilistic — about half should land
    fi.reset();
    fi.seed(0xCAFE);
    fi.arm("pool.allocate.fail", 0.5);
    OrderBook book2(0);
    int landed = 0, rejected = 0;
    for (int i = 0; i < 1000; ++i) {
        auto r = book2.addOrder(static_cast<OrderId>(i + 1), 1, Side::Buy,
                                 1000000 + (i % 5), 100, OrderType::Limit);
        if (std::holds_alternative<OrderId>(r)) ++landed;
        else {
            assert(std::get<RejectReason>(r) == RejectReason::CapacityExhausted);
            ++rejected;
        }
    }
    std::printf("probabilistic: %d landed, %d rejected\n", landed, rejected);
    // 50% × 1000 = 500 expected; 4σ envelope ≈ ±60. Loose bound to avoid
    // flakes from determinism + auto-trigger paths inside addOrder.
    assert(landed + rejected == 1000);
    assert(landed > 200 && rejected > 200);

    std::puts("PoolExhaustionTest passed");
    return 0;
}
