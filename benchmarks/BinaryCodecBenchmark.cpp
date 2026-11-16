// BinaryCodecBenchmark — measures the encode rate of the ITCH 5.0 AddOrder
// message (36B): big-endian, ASCII-decimal stock slot, hand-coded fixed offsets.
//
// Pure user-space, single-thread, single-message-type microbench. No engine,
// no transport, no network — this isolates the codec cost. Results vary by CPU;
// treat absolute numbers as indicative.

#include "ItchProtocol.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace OrderMatcher;
using Clock = std::chrono::steady_clock;

namespace {

// Inline-asm barrier that prevents the compiler from optimizing
// away the use of `v`. Identical in shape to Google Benchmark's
// `DoNotOptimize` and Folly's `doNotOptimizeAway` — for an isolated
// microbench like this one we replicate the trick inline rather
// than pull in a dependency.
template <typename T>
inline void doNotOptimize(T const& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

template <typename T>
inline void clobber(T& v) {
    asm volatile("" : "+r,m"(v) : : "memory");
}

struct Result {
    const char* name;
    size_t      bytes;
    uint64_t    iterations;
    double      seconds;

    double opsPerSec() const { return iterations / seconds; }
    double nsPerOp()   const { return (seconds * 1e9) / iterations; }
    double gbPerSec()  const { return (iterations * bytes) / seconds / 1e9; }
};

template <typename F>
Result run(const char* name, size_t bytesPerOp, uint64_t iters, F&& fn) {
    // Warm up — a few iterations to stabilize the cache / branch
    // predictor before measuring.
    for (uint64_t i = 0; i < iters / 10; ++i) fn(i);

    auto t0 = Clock::now();
    for (uint64_t i = 0; i < iters; ++i) fn(i);
    auto t1 = Clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    return {name, bytesPerOp, iters, secs};
}

void printHeader() {
    std::printf("%-36s %12s %12s %10s %8s\n",
                "Benchmark", "Iters", "ns/op",
                "M ops/s", "GB/s");
    std::printf("%-36s %12s %12s %10s %8s\n",
                "----------", "-----", "-----",
                "-------", "----");
}

void printRow(const Result& r) {
    std::printf("%-36s %12llu %12.1f %10.2f %8.2f\n",
                r.name,
                static_cast<unsigned long long>(r.iterations),
                r.nsPerOp(),
                r.opsPerSec() / 1e6,
                r.gbPerSec());
}

}  // namespace

int main() {
    constexpr uint64_t kIters = 2'000'000;

    std::printf("\n=== Binary Codec Benchmark ===\n");
    std::printf("Single-thread microbench, no engine / no network.\n");
    std::printf("%llu iterations per benchmark.\n\n",
                static_cast<unsigned long long>(kIters));
    printHeader();

    // ─── ITCH AddOrder encode ───────────────────────────────────────────────
    {
        uint8_t buf[ITCH_SIZE_ADD_ORDER];
        auto r = run("ITCH AddOrder encode (36B)",
                     ITCH_SIZE_ADD_ORDER, kIters,
                     [&](uint64_t i) {
            doNotOptimize(i);
            encodeAddOrder(buf, /*locate=*/42, /*track=*/0,
                           /*ts=*/0x000000000001ULL + i,
                           /*ref=*/i, Side::Buy, 100, 42, 1000);
            clobber(buf);
        });
        printRow(r);
    }

    return 0;
}
