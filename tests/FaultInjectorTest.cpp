// Smoke + statistical tests for FaultInjector. Compiled with
// OB_ENABLE_FAULT_INJECTION; the disabled path is exercised by every other
// release build implicitly (constexpr-false).

#define OB_ENABLE_FAULT_INJECTION 1
#include "FaultInjector.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using OrderMatcher::FaultInjector;

static void test_disabled_by_default() {
    auto& fi = FaultInjector::instance();
    fi.reset();
    for (int i = 0; i < 1000; ++i) {
        assert(!fi.shouldFail("never.armed"));
    }
    assert(fi.activations("never.armed") == 0);
}

static void test_arm_disarm_counts() {
    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(42);
    assert(fi.arm("p", 1.0));  // always fail
    int hits = 0;
    for (int i = 0; i < 100; ++i) {
        if (fi.shouldFail("p")) ++hits;
    }
    assert(hits == 100);
    assert(fi.activations("p") == 100);
    fi.disarm("p");
    for (int i = 0; i < 100; ++i) {
        assert(!fi.shouldFail("p"));
    }
    assert(fi.activations("p") == 100);  // counter preserved
}

static void test_probability_distribution() {
    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(0xC0FFEE);
    fi.arm("flaky", 0.25);
    constexpr int N = 20000;
    int hits = 0;
    for (int i = 0; i < N; ++i) {
        if (fi.shouldFail("flaky")) ++hits;
    }
    double frac = double(hits) / N;
    // 4-sigma envelope around 0.25 for N=20000: stddev ≈ 0.00306.
    assert(std::abs(frac - 0.25) < 0.02 && "probability outside expected band");
}

static void test_seed_determinism() {
    auto& fi = FaultInjector::instance();
    auto run = [&]() {
        fi.reset();
        fi.seed(0xDEADBEEFull);
        fi.arm("d", 0.5);
        std::vector<bool> seq;
        for (int i = 0; i < 200; ++i) seq.push_back(fi.shouldFail("d"));
        return seq;
    };
    auto a = run();
    auto b = run();
    assert(a == b && "same seed must produce same sequence");
}

static void test_thread_safety() {
    auto& fi = FaultInjector::instance();
    fi.reset();
    fi.seed(1);
    fi.arm("t", 0.5);
    constexpr int kThreads = 8;
    constexpr int kIters = 5000;
    std::atomic<int> total{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            int local = 0;
            for (int i = 0; i < kIters; ++i) {
                if (fi.shouldFail("t")) ++local;
            }
            total.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (auto& th : ts) th.join();
    // Counter from injector must match the locally-tallied total.
    assert(fi.activations("t") == static_cast<uint64_t>(total.load()));
}

int main() {
    test_disabled_by_default();
    test_arm_disarm_counts();
    test_probability_distribution();
    test_seed_determinism();
    test_thread_safety();
    std::puts("FaultInjector tests passed");
    return 0;
}
