// ItchByteLayoutTest — exact wire-offset coverage for the ITCH 5.0
// encoders that previously had only a type-code/size mapping check
// (ProtocolVersioningTest) but no field-by-field byte-layout assertion.
//
// ITCH has no version field on the wire: a message-type byte maps to a
// single fixed layout forever, so any drift in a field's offset is a
// silent, backward-incompatible feed break. These tests pin the layout
// of three previously-uncovered messages by encoding a frame with a
// distinct sentinel in every field, then reading each field back at its
// SPEC offset and asserting the value survives. Because the read-back
// offsets are hard-coded, a field encoded at the wrong offset fails the
// round-trip — the offset IS under test, not just the value.
//
// Coverage:
//   1. 'C' OrderExecutedWithPrice (36 bytes) — every field offset
//   2. 'X' OrderCancel            (23 bytes) — every field offset
//   3. 'P' Trade                  (44 bytes) — every field offset
//
// These are COVERAGE additions: the encoders are believed correct, so
// the tests are expected to pass on the first build. A failure here is a
// real layout bug in the corresponding encoder.

#include "ItchProtocol.h"
#include "OuchProtocol.h"  // readU16BE/readU32BE/readU64BE, parseAsciiDecimal

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace OrderMatcher;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                        \
    std::cout << "  " << #name << "... " << std::flush;                   \
    try
#define END                                                               \
    catch (const std::exception& e) {                                     \
        std::cout << "FAIL: " << e.what() << "\n";                        \
        ++tests_failed;                                                   \
        return;                                                           \
    }                                                                     \
    std::cout << "ok\n";                                                  \
    ++tests_passed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            throw std::runtime_error("CHECK failed: " #cond);             \
        }                                                                 \
    } while (0)

// Helper: read the 8-byte ASCII-decimal stock slot back to an integer.
static uint64_t readStock(const uint8_t* p) {
    uint64_t v = 0;
    if (!parseAsciiDecimal(p, 8, v)) {
        throw std::runtime_error("stock slot not ASCII-decimal");
    }
    return v;
}

// ─── 'C' OrderExecutedWithPrice (36 bytes) ───────────────────────────────────
// Spec layout (from encodeOrderExecutedWithPrice):
//   0      MessageType ('C')
//   1-2    StockLocate      (BE u16)
//   3-4    TrackingNumber   (BE u16)
//   5-10   Timestamp        (BE u48)
//   11-18  OrderRefNumber   (BE u64)
//   19-22  ExecutedShares   (BE u32)
//   23-30  MatchNumber      (BE u64)
//   31     Printable        ('Y'/'N')
//   32-35  ExecutionPrice   (BE u32)
void test_OrderExecutedWithPriceLayout() {
    TEST(OrderExecutedWithPriceLayout) {
        const uint16_t kLocate    = 0x1122;
        const uint16_t kTracking  = 0x3344;
        const uint64_t kTimestamp = 0x102030405060ULL;  // exactly 48 bits
        const uint64_t kOrderRef  = 0xA1A2A3A4A5A6A7A8ULL;
        const uint32_t kExecShares = 0xDEADBEEFU;
        const uint64_t kMatch     = 0xB1B2B3B4B5B6B7B8ULL;
        const uint32_t kExecPrice = 0x0BADF00DU;

        uint8_t out[ITCH_SIZE_ORDER_EXECUTED_PX];
        std::memset(out, 0xCC, sizeof(out));  // poison: any unwritten byte shows
        size_t n = encodeOrderExecutedWithPrice(
            out, kLocate, kTracking, kTimestamp, kOrderRef,
            static_cast<Quantity>(kExecShares), kMatch,
            /*printable=*/true, static_cast<Price>(kExecPrice));

        CHECK(n == ITCH_SIZE_ORDER_EXECUTED_PX);
        CHECK(n == 36);

        // Every field, read back at its SPEC offset.
        CHECK(out[0] == ITCH_MT_ORDER_EXECUTED_PX);   // 0: 'C'
        CHECK(readU16BE(out + 1)  == kLocate);        // 1-2
        CHECK(readU16BE(out + 3)  == kTracking);      // 3-4
        CHECK(readU48BE(out + 5)  == kTimestamp);     // 5-10
        CHECK(readU64BE(out + 11) == kOrderRef);      // 11-18
        CHECK(readU32BE(out + 19) == kExecShares);    // 19-22
        CHECK(readU64BE(out + 23) == kMatch);         // 23-30
        CHECK(out[31] == 'Y');                        // 31: printable flag
        CHECK(readU32BE(out + 32) == kExecPrice);     // 32-35

        // The printable flag toggles independently at offset 31 and
        // nowhere else.
        uint8_t out2[ITCH_SIZE_ORDER_EXECUTED_PX];
        encodeOrderExecutedWithPrice(out2, kLocate, kTracking, kTimestamp,
                                     kOrderRef, static_cast<Quantity>(kExecShares),
                                     kMatch, /*printable=*/false,
                                     static_cast<Price>(kExecPrice));
        CHECK(out2[31] == 'N');
        out2[31] = out[31];  // normalize the one differing byte
        CHECK(std::memcmp(out, out2, sizeof(out)) == 0
              && "printable flag must affect only offset 31");
    } END
}

// ─── 'X' OrderCancel (23 bytes) ──────────────────────────────────────────────
// Spec layout (from encodeOrderCancel):
//   0      MessageType ('X')
//   1-2    StockLocate      (BE u16)
//   3-4    TrackingNumber   (BE u16)
//   5-10   Timestamp        (BE u48)
//   11-18  OrderRefNumber   (BE u64)
//   19-22  CanceledShares   (BE u32)
void test_OrderCancelLayout() {
    TEST(OrderCancelLayout) {
        const uint16_t kLocate    = 0x4455;
        const uint16_t kTracking  = 0x6677;
        const uint64_t kTimestamp = 0xAABBCCDDEEFFULL;  // exactly 48 bits set
        const uint64_t kOrderRef  = 0xC1C2C3C4C5C6C7C8ULL;
        const uint32_t kCanceled  = 0x12345678U;

        uint8_t out[ITCH_SIZE_ORDER_CANCEL];
        std::memset(out, 0xCC, sizeof(out));
        size_t n = encodeOrderCancel(out, kLocate, kTracking, kTimestamp,
                                     kOrderRef, static_cast<Quantity>(kCanceled));

        CHECK(n == ITCH_SIZE_ORDER_CANCEL);
        CHECK(n == 23);

        CHECK(out[0] == ITCH_MT_ORDER_CANCEL);     // 0: 'X'
        CHECK(readU16BE(out + 1)  == kLocate);     // 1-2
        CHECK(readU16BE(out + 3)  == kTracking);   // 3-4
        CHECK(readU48BE(out + 5)  == kTimestamp);  // 5-10
        CHECK(readU64BE(out + 11) == kOrderRef);   // 11-18
        CHECK(readU32BE(out + 19) == kCanceled);   // 19-22
    } END
}

// ─── 'P' Trade (44 bytes) ────────────────────────────────────────────────────
// Spec layout (from encodeTrade):
//   0      MessageType ('P')
//   1-2    StockLocate      (BE u16)
//   3-4    TrackingNumber   (BE u16)
//   5-10   Timestamp        (BE u48)
//   11-18  OrderRefNumber   (BE u64)
//   19     BuySellIndicator ('B'/'S')
//   20-23  Shares           (BE u32)
//   24-31  Stock            (8B ASCII decimal)
//   32-35  Price            (BE u32)
//   36-43  MatchNumber      (BE u64)
void test_TradeLayout() {
    TEST(TradeLayout) {
        const uint16_t kLocate    = 0x0708;
        const uint16_t kTracking  = 0x090A;
        const uint64_t kTimestamp = 0x010203040506ULL;
        const uint64_t kOrderRef  = 0xD1D2D3D4D5D6D7D8ULL;
        const uint32_t kShares    = 0x00BC614EU;       // 12345678 decimal
        const uint64_t kStock     = 87654321ULL;       // fits 8-digit ASCII slot
        const uint32_t kPrice     = 0x0001E240U;       // 123456 decimal
        const uint64_t kMatch     = 0xE1E2E3E4E5E6E7E8ULL;

        uint8_t out[ITCH_SIZE_TRADE];
        std::memset(out, 0xCC, sizeof(out));
        size_t n = encodeTrade(out, kLocate, kTracking, kTimestamp, kOrderRef,
                               Side::Sell, static_cast<Quantity>(kShares),
                               static_cast<SymbolId>(kStock),
                               static_cast<Price>(kPrice), kMatch);

        CHECK(n == ITCH_SIZE_TRADE);
        CHECK(n == 44);

        CHECK(out[0] == ITCH_MT_TRADE);            // 0: 'P'
        CHECK(readU16BE(out + 1)  == kLocate);     // 1-2
        CHECK(readU16BE(out + 3)  == kTracking);   // 3-4
        CHECK(readU48BE(out + 5)  == kTimestamp);  // 5-10
        CHECK(readU64BE(out + 11) == kOrderRef);   // 11-18
        CHECK(out[19] == 'S');                     // 19: Sell side
        CHECK(readU32BE(out + 20) == kShares);     // 20-23
        CHECK(readStock(out + 24) == kStock);      // 24-31 (ASCII decimal)
        CHECK(readU32BE(out + 32) == kPrice);      // 32-35
        CHECK(readU64BE(out + 36) == kMatch);      // 36-43

        // The buy/sell indicator at offset 19 flips with the side and
        // touches nothing else.
        uint8_t buyOut[ITCH_SIZE_TRADE];
        encodeTrade(buyOut, kLocate, kTracking, kTimestamp, kOrderRef,
                    Side::Buy, static_cast<Quantity>(kShares),
                    static_cast<SymbolId>(kStock),
                    static_cast<Price>(kPrice), kMatch);
        CHECK(buyOut[19] == 'B');
        buyOut[19] = out[19];  // normalize the one differing byte
        CHECK(std::memcmp(out, buyOut, sizeof(out)) == 0
              && "buy/sell indicator must affect only offset 19");
    } END
}

int main() {
    std::cout << "Running ItchByteLayoutTest\n";

    test_OrderExecutedWithPriceLayout();
    test_OrderCancelLayout();
    test_TradeLayout();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
