// BinaryCodecBenchmark — measures encode/decode rates for the three
// binary order-entry/market-data protocols we ship:
//
//   * OUCH 4.2  EnterOrder (49B)   + OrderAccepted (66B)
//   * ITCH 5.0  AddOrder (36B)
//   * SBE       NewOrderV1 (32B = 8B header + 24B block)
//
// Pure user-space, single-thread, single-message-type microbench.
// No engine, no transport, no network — this isolates the codec
// cost so a reader can compare paradigms apples-to-apples:
//
//   - OUCH/ITCH:   big-endian, ASCII-decimal field slots, hand-coded
//                  fixed offsets.
//   - SBE:         little-endian (matches x86/ARM native byte order),
//                  binary integer fields, no string parsing.
//
// Output is messages/sec and ns/message for each protocol's encode
// and decode paths. Results vary by CPU; treat absolute numbers as
// indicative, ratios as more stable.

#include "ItchProtocol.h"
#include "OuchProtocol.h"
#include "SbeProtocol.h"

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

    // ─── OUCH EnterOrder encode ─────────────────────────────────────────────
    {
        uint8_t buf[OUCH_SIZE_ENTER_ORDER];
        auto r = run("OUCH EnterOrder encode (49B)",
                     OUCH_SIZE_ENTER_ORDER, kIters,
                     [&](uint64_t i) {
            doNotOptimize(i);
            std::memset(buf, ' ', OUCH_SIZE_ENTER_ORDER);
            buf[0] = OUCH_MT_ENTER_ORDER;
            writeAsciiDecimal(buf + 1, 14, i);
            buf[15] = 'B';
            writeU32BE(buf + 16, 100);
            writeAsciiDecimal(buf + 20, 8, 42);
            writeU32BE(buf + 28, 1000);
            writeU32BE(buf + 32, OUCH_TIF_DAY);
            writeAsciiDecimal(buf + 36, 4, 1);
            buf[40] = 'Y'; buf[41] = 'O'; buf[42] = 'N';
            writeU32BE(buf + 43, 0);
            buf[47] = 'N'; buf[48] = 'R';
            clobber(buf);  // force buf to be considered observed
        });
        printRow(r);
    }

    // ─── OUCH EnterOrder decode ─────────────────────────────────────────────
    {
        uint8_t buf[OUCH_SIZE_ENTER_ORDER];
        std::memset(buf, ' ', OUCH_SIZE_ENTER_ORDER);
        buf[0] = OUCH_MT_ENTER_ORDER;
        writeAsciiDecimal(buf + 1, 14, 12345);
        buf[15] = 'B';
        writeU32BE(buf + 16, 100);
        writeAsciiDecimal(buf + 20, 8, 42);
        writeU32BE(buf + 28, 1000);
        writeU32BE(buf + 32, OUCH_TIF_DAY);
        writeAsciiDecimal(buf + 36, 4, 1);
        buf[40] = 'Y'; buf[41] = 'O'; buf[42] = 'N';
        writeU32BE(buf + 43, 0);
        buf[47] = 'N'; buf[48] = 'R';

        OuchEnterOrder o;
        auto r = run("OUCH EnterOrder decode (49B)",
                     OUCH_SIZE_ENTER_ORDER, kIters,
                     [&](uint64_t) {
            decodeEnterOrder(buf, OUCH_SIZE_ENTER_ORDER, o);
            doNotOptimize(o);
        });
        printRow(r);
    }

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

    // ─── SBE NewOrderV1 encode ──────────────────────────────────────────────
    {
        constexpr size_t kSbeSize = SBE_MESSAGE_HEADER_BYTES +
                                     SBE_NEW_ORDER_V1_BLOCK_BYTES;
        uint8_t buf[kSbeSize];
        SbeNewOrderV1 msg;
        msg.orderId = 0; msg.price = 1000; msg.quantity = 100;
        msg.side = 1; msg.orderType = 1;

        auto r = run("SBE NewOrderV1 encode (32B)",
                     kSbeSize, kIters,
                     [&](uint64_t i) {
            msg.orderId = i;
            doNotOptimize(msg);
            encodeSbeNewOrderV1(buf, msg);
            clobber(buf);
        });
        printRow(r);
    }

    // ─── SBE NewOrderV1 decode ──────────────────────────────────────────────
    {
        constexpr size_t kSbeSize = SBE_MESSAGE_HEADER_BYTES +
                                     SBE_NEW_ORDER_V1_BLOCK_BYTES;
        uint8_t buf[kSbeSize];
        SbeNewOrderV1 in;
        in.orderId = 12345; in.price = 1000; in.quantity = 100;
        in.side = 1; in.orderType = 1;
        encodeSbeNewOrderV1(buf, in);

        SbeNewOrderV1 out;
        auto r = run("SBE NewOrderV1 decode (32B)",
                     kSbeSize, kIters,
                     [&](uint64_t) {
            auto h = readSbeMessageHeader(buf);
            doNotOptimize(h);
            readSbeNewOrderV1Block(buf + SBE_MESSAGE_HEADER_BYTES, out);
            doNotOptimize(out);
        });
        printRow(r);
    }

    std::printf("\nNotes:\n");
    std::printf("  * OUCH carries ASCII-decimal fields in the wire — encode/\n");
    std::printf("    decode cost includes snprintf-style integer formatting.\n");
    std::printf("  * SBE uses native-endian binary integers — no string\n");
    std::printf("    conversion. Expected to be the fastest of the three.\n");
    std::printf("  * ITCH is bigger per message than SBE on this comparison\n");
    std::printf("    because it includes more metadata (tracking #, locate,\n");
    std::printf("    side, stock) — but no ASCII conversion overhead beyond\n");
    std::printf("    the stock field.\n\n");

    return 0;
}
