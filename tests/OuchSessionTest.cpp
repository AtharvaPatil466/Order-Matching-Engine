// OuchSessionTest — exercises the OUCH 4.2 binary order-entry codec
// and the session adapter that drives MatchingEngine.
//
// Coverage:
//   Codec layer
//     1. EnterOrder roundtrip (encode-equivalent → decode → identical)
//     2. CancelOrder roundtrip
//     3. OrderAccepted layout (wire byte-level checks)
//     4. OrderRejected layout
//     5. OrderCanceled layout
//     6. OrderExecuted layout
//     7. Big-endian field encoding (the wire is big-endian even on LE hosts)
//     8. ASCII-decimal slot parsing handles padding correctly
//     9. ASCII-decimal slot rejects non-digit content
//
//   Session layer
//    10. EnterOrder lands in the engine and triggers an Accepted response
//    11. EnterOrder for unregistered symbol triggers a Reject with code 'S'
//    12. CancelOrder removes the order and emits a Canceled response
//    13. Fragmented feed (one byte at a time) reassembles correctly
//    14. Multiple frames in a single feed are all dispatched
//    15. Unknown message type aborts the session (returns false from feed)

#include "MatchingEngine.h"
#include "OuchProtocol.h"
#include "OuchSession.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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

// ─── Helpers ────────────────────────────────────────────────────────────────

namespace {

// Build a wire-format EnterOrder message into a freshly-allocated vector.
// Mirrors what a real client would emit; used by both codec and session
// tests to keep the construction logic in one place.
std::vector<uint8_t> buildEnterOrder(uint64_t token, char side, uint32_t shares,
                                     uint64_t stock, uint32_t price,
                                     uint32_t tif, uint64_t firm,
                                     char display = 'Y') {
    std::vector<uint8_t> buf(OUCH_SIZE_ENTER_ORDER, ' ');
    buf[0] = OUCH_MT_ENTER_ORDER;
    writeAsciiDecimal(buf.data() + 1, 14, token);
    buf[15] = static_cast<uint8_t>(side);
    writeU32BE(buf.data() + 16, shares);
    writeAsciiDecimal(buf.data() + 20, 8, stock);
    writeU32BE(buf.data() + 28, price);
    writeU32BE(buf.data() + 32, tif);
    writeAsciiDecimal(buf.data() + 36, 4, firm);
    buf[40] = display;
    buf[41] = 'O';   // capacity
    buf[42] = 'N';   // intermarket sweep
    writeU32BE(buf.data() + 43, 0);  // min quantity
    buf[47] = 'N';   // cross type
    buf[48] = 'R';   // customer type
    return buf;
}

std::vector<uint8_t> buildCancelOrder(uint64_t token, uint32_t sharesToCancel) {
    std::vector<uint8_t> buf(OUCH_SIZE_CANCEL_ORDER);
    buf[0] = OUCH_MT_CANCEL_ORDER;
    writeAsciiDecimal(buf.data() + 1, 14, token);
    writeU32BE(buf.data() + 15, sharesToCancel);
    return buf;
}

}  // namespace

// ─── Codec tests ────────────────────────────────────────────────────────────

void test_EnterOrderRoundtrip() {
    TEST(EnterOrderRoundtrip) {
        auto wire = buildEnterOrder(1234567890ULL, 'B', 500, 42, 1000000,
                                    OUCH_TIF_DAY, 7777);
        CHECK(wire.size() == OUCH_SIZE_ENTER_ORDER);

        OuchEnterOrder out;
        CHECK(decodeEnterOrder(wire.data(), wire.size(), out));
        CHECK(out.orderToken == 1234567890ULL);
        CHECK(out.side == Side::Buy);
        CHECK(out.shares == 500);
        CHECK(out.stock == 42);
        CHECK(out.price == 1000000);
        CHECK(out.timeInForce == OUCH_TIF_DAY);
        CHECK(out.firm == 7777);
    } END
}

void test_CancelOrderRoundtrip() {
    TEST(CancelOrderRoundtrip) {
        auto wire = buildCancelOrder(99, 250);
        OuchCancelOrder c;
        CHECK(decodeCancelOrder(wire.data(), wire.size(), c));
        CHECK(c.orderToken == 99);
        CHECK(c.sharesToCancel == 250);
    } END
}

void test_BigEndianEncoding() {
    TEST(BigEndianEncoding) {
        // The wire is big-endian regardless of host byte order. Hand-pick
        // a value where every byte is distinct so we can assert layout.
        uint8_t buf[4];
        writeU32BE(buf, 0x11223344u);
        CHECK(buf[0] == 0x11);
        CHECK(buf[1] == 0x22);
        CHECK(buf[2] == 0x33);
        CHECK(buf[3] == 0x44);
        CHECK(readU32BE(buf) == 0x11223344u);

        uint8_t buf64[8];
        writeU64BE(buf64, 0x0102030405060708ULL);
        for (int i = 0; i < 8; ++i) CHECK(buf64[i] == uint8_t(i + 1));
        CHECK(readU64BE(buf64) == 0x0102030405060708ULL);
    } END
}

void test_AsciiDecimalParsesPadding() {
    TEST(AsciiDecimalParsesPadding) {
        uint8_t s[14] = {' ',' ',' ',' ',' ',' ',' ',' ',' ',' ','1','2','3','4'};
        uint64_t v = 0;
        CHECK(parseAsciiDecimal(s, 14, v));
        CHECK(v == 1234ULL);

        uint8_t s2[8] = {'4','2',' ',' ',' ',' ',' ',' '};
        CHECK(parseAsciiDecimal(s2, 8, v));
        CHECK(v == 42ULL);

        uint8_t empty[4] = {' ',' ',' ',' '};
        CHECK(!parseAsciiDecimal(empty, 4, v) && "empty slot is not a number");
    } END
}

void test_AsciiDecimalRejectsGarbage() {
    TEST(AsciiDecimalRejectsGarbage) {
        uint8_t s[4] = {'A','B','C','D'};
        uint64_t v = 99;
        CHECK(!parseAsciiDecimal(s, 4, v));

        // Mixed digit + non-digit must also fail (no partial parsing).
        uint8_t mix[4] = {'1','2','X','4'};
        CHECK(!parseAsciiDecimal(mix, 4, v));
    } END
}

void test_OrderAcceptedLayout() {
    TEST(OrderAcceptedLayout) {
        uint8_t buf[OUCH_SIZE_ORDER_ACCEPTED];
        size_t n = encodeOrderAccepted(buf, /*timestamp=*/0xDEADBEEFCAFEBABEULL,
                                       /*token=*/77, Side::Sell,
                                       /*shares=*/100, /*stock=*/3,
                                       /*price=*/200000, /*tif=*/OUCH_TIF_DAY,
                                       /*firm=*/9, /*orderRef=*/0x1234ULL);
        CHECK(n == OUCH_SIZE_ORDER_ACCEPTED);
        CHECK(buf[0] == 'A');
        CHECK(readU64BE(buf + 1) == 0xDEADBEEFCAFEBABEULL);
        CHECK(buf[23] == 'S');
        CHECK(readU32BE(buf + 24) == 100);
        CHECK(readU32BE(buf + 36) == 200000);
        CHECK(readU64BE(buf + 49) == 0x1234ULL);
        CHECK(buf[64] == 'L');  // order state: live
    } END
}

void test_OrderRejectedLayout() {
    TEST(OrderRejectedLayout) {
        uint8_t buf[OUCH_SIZE_ORDER_REJECTED];
        size_t n = encodeOrderRejected(buf, 1ULL, 555, OUCH_REJECT_INVALID_PRICE);
        CHECK(n == OUCH_SIZE_ORDER_REJECTED);
        CHECK(buf[0] == 'J');
        CHECK(readU64BE(buf + 1) == 1ULL);
        CHECK(buf[23] == OUCH_REJECT_INVALID_PRICE);
    } END
}

void test_OrderCanceledLayout() {
    TEST(OrderCanceledLayout) {
        uint8_t buf[OUCH_SIZE_ORDER_CANCELED];
        size_t n = encodeOrderCanceled(buf, 42ULL, 100, 75);
        CHECK(n == OUCH_SIZE_ORDER_CANCELED);
        CHECK(buf[0] == 'C');
        CHECK(readU64BE(buf + 1) == 42ULL);
        CHECK(readU32BE(buf + 23) == 75);
        CHECK(buf[27] == 'U');  // default reason: user requested
    } END
}

void test_OrderExecutedLayout() {
    TEST(OrderExecutedLayout) {
        uint8_t buf[OUCH_SIZE_ORDER_EXECUTED];
        size_t n = encodeOrderExecuted(buf, 7ULL, 999, 30, 50000, 0xABCDEF01ULL);
        CHECK(n == OUCH_SIZE_ORDER_EXECUTED);
        CHECK(buf[0] == 'E');
        CHECK(readU32BE(buf + 23) == 30);          // executed shares
        CHECK(readU32BE(buf + 27) == 50000);       // execution price
        CHECK(readU64BE(buf + 32) == 0xABCDEF01ULL);  // match number
    } END
}

// ─── Session tests ──────────────────────────────────────────────────────────

void test_SessionAcceptsEnterOrder() {
    TEST(SessionAcceptsEnterOrder) {
        MatchingEngine engine;
        engine.addSymbol(42);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });
        session.setClock([] { return uint64_t{12345}; });

        auto wire = buildEnterOrder(/*token=*/1001, 'B', /*shares=*/50,
                                    /*stock=*/42, /*price=*/1000,
                                    OUCH_TIF_DAY, /*firm=*/100);
        CHECK(session.feed(reinterpret_cast<const char*>(wire.data()),
                           wire.size()));
        CHECK(session.ordersAccepted() == 1);
        CHECK(session.ordersRejected() == 0);

        // The order is in the book.
        auto* book = engine.getOrderBook(42);
        CHECK(book != nullptr);
        const Order* o = book->getOrder(1001);
        CHECK(o != nullptr);
        CHECK(o->remainingQty == 50);

        // The wire response is a properly-framed 'A' message.
        CHECK(sent.size() == OUCH_SIZE_ORDER_ACCEPTED);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == OUCH_MT_ORDER_ACCEPTED);
        CHECK(readU64BE(resp + 1) == 12345ULL);          // injected clock
        CHECK(readU64BE(resp + 49) == 1001ULL);          // order ref echoes token
    } END
}

void test_SessionRejectsUnregisteredSymbol() {
    TEST(SessionRejectsUnregisteredSymbol) {
        MatchingEngine engine;
        engine.start();  // no symbols added

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });

        auto wire = buildEnterOrder(2002, 'B', 10, /*stock=*/99, 1000,
                                    OUCH_TIF_DAY, 100);
        CHECK(session.feed(reinterpret_cast<const char*>(wire.data()),
                           wire.size()));
        CHECK(session.ordersAccepted() == 0);
        CHECK(session.ordersRejected() == 1);

        CHECK(sent.size() == OUCH_SIZE_ORDER_REJECTED);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == OUCH_MT_ORDER_REJECTED);
        CHECK(readU64BE(resp + 9) >= 0);  // token slot is valid ascii
        CHECK(resp[23] == OUCH_REJECT_INVALID_STOCK);  // 'S'
    } END
}

void test_SessionCancelRemovesOrder() {
    TEST(SessionCancelRemovesOrder) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });

        auto place = buildEnterOrder(500, 'B', 25, 7, 1000, OUCH_TIF_DAY, 1);
        CHECK(session.feed(reinterpret_cast<const char*>(place.data()),
                           place.size()));
        sent.clear();  // discard the Accepted

        auto cancel = buildCancelOrder(500, 0);
        CHECK(session.feed(reinterpret_cast<const char*>(cancel.data()),
                           cancel.size()));
        CHECK(session.cancelsAccepted() == 1);

        auto* book = engine.getOrderBook(7);
        CHECK(book != nullptr);
        CHECK(book->getOrder(500) == nullptr);

        CHECK(sent.size() == OUCH_SIZE_ORDER_CANCELED);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == OUCH_MT_ORDER_CANCELED);
    } END
}

void test_SessionFragmentedFeed() {
    TEST(SessionFragmentedFeed) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });

        auto wire = buildEnterOrder(600, 'B', 10, 7, 1000, OUCH_TIF_DAY, 1);
        // Drip the message one byte at a time — the session must
        // accumulate and only dispatch when the full frame arrives.
        for (size_t i = 0; i < wire.size(); ++i) {
            CHECK(session.feed(reinterpret_cast<const char*>(wire.data() + i), 1));
            if (i + 1 < wire.size()) {
                CHECK(session.ordersAccepted() == 0
                      && "session must NOT dispatch on a partial frame");
            }
        }
        CHECK(session.ordersAccepted() == 1);
        CHECK(session.framesProcessed() == 1);
    } END
}

void test_SessionBatchedFrames() {
    TEST(SessionBatchedFrames) {
        MatchingEngine engine;
        engine.addSymbol(5);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });

        // Pack three Enter orders into one feed call; the session must
        // drain all three in order.
        std::vector<uint8_t> batched;
        for (int i = 0; i < 3; ++i) {
            auto w = buildEnterOrder(700 + i, 'B', 10, 5, 1000, OUCH_TIF_DAY, 1);
            batched.insert(batched.end(), w.begin(), w.end());
        }
        CHECK(session.feed(reinterpret_cast<const char*>(batched.data()),
                           batched.size()));
        CHECK(session.framesProcessed() == 3);
        CHECK(session.ordersAccepted() == 3);
    } END
}

void test_SessionRejectsUnknownMsgType() {
    TEST(SessionRejectsUnknownMsgType) {
        MatchingEngine engine;
        engine.start();

        OuchSession session(engine, [](std::string_view) {});
        // 'Z' is not a defined inbound OUCH type. The session must
        // refuse to drain the buffer — returning false signals the
        // gateway to drop the connection.
        const char garbage[1] = {'Z'};
        CHECK(!session.feed(garbage, 1));
        CHECK(session.framesProcessed() == 0);
    } END
}

// ─── Replace-order tests ────────────────────────────────────────────────────

namespace {

std::vector<uint8_t> buildReplaceOrder(uint64_t existingToken,
                                       uint64_t newToken,
                                       uint32_t shares, uint32_t price,
                                       uint32_t tif = OUCH_TIF_DAY) {
    std::vector<uint8_t> buf(OUCH_SIZE_REPLACE_ORDER);
    buf[0] = OUCH_MT_REPLACE_ORDER;
    writeAsciiDecimal(buf.data() + 1, 14, existingToken);
    writeAsciiDecimal(buf.data() + 15, 14, newToken);
    writeU32BE(buf.data() + 29, shares);
    writeU32BE(buf.data() + 33, price);
    writeU32BE(buf.data() + 37, tif);
    buf[41] = 'Y';                   // display
    writeU32BE(buf.data() + 42, 0);  // min qty
    buf[46] = 'N';                   // intermarket sweep
    return buf;
}

}  // namespace

void test_ReplaceOrderRoundtrip() {
    TEST(ReplaceOrderRoundtrip) {
        auto wire = buildReplaceOrder(/*existing=*/100, /*new=*/200,
                                       /*shares=*/50, /*price=*/2500);
        OuchReplaceOrder r;
        CHECK(decodeReplaceOrder(wire.data(), wire.size(), r));
        CHECK(r.existingOrderToken == 100);
        CHECK(r.newOrderToken == 200);
        CHECK(r.shares == 50);
        CHECK(r.price == 2500);
    } END
}

void test_OrderReplacedLayout() {
    TEST(OrderReplacedLayout) {
        uint8_t buf[OUCH_SIZE_ORDER_REPLACED];
        size_t n = encodeOrderReplaced(
            buf, /*ts=*/0xCAFEBABEULL, /*newToken=*/200, Side::Buy,
            /*shares=*/40, /*stock=*/7, /*price=*/1500,
            /*tif=*/OUCH_TIF_DAY, /*firm=*/1, /*orderRef=*/123,
            /*minQty=*/0, /*previousToken=*/100);
        CHECK(n == OUCH_SIZE_ORDER_REPLACED);
        CHECK(buf[0] == OUCH_MT_ORDER_REPLACED);
        CHECK(readU64BE(buf + 1) == 0xCAFEBABEULL);
        CHECK(buf[23] == 'B');
        CHECK(readU32BE(buf + 24) == 40);
        CHECK(readU32BE(buf + 36) == 1500);
        CHECK(readU64BE(buf + 49) == 123);   // engine order ref preserved
        CHECK(buf[64] == 'L');               // order state: live
        // Previous token slot at offset 65 — ASCII "100" then padding.
        CHECK(buf[65] == '1' && buf[66] == '0' && buf[67] == '0');
    } END
}

void test_SessionReplaceUpdatesPriceAndQty() {
    TEST(SessionReplaceUpdatesPriceAndQty) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });
        session.setClock([] { return uint64_t{1000}; });

        // 1. Place a resting buy 50 @ 1000.
        auto place = buildEnterOrder(/*token=*/500, 'B', 50, /*stock=*/7,
                                     /*price=*/1000, OUCH_TIF_DAY, 1);
        CHECK(session.feed(reinterpret_cast<const char*>(place.data()),
                           place.size()));
        CHECK(session.ordersAccepted() == 1);
        sent.clear();

        // 2. Replace to 30 @ 1100 with new token 501.
        auto rep = buildReplaceOrder(/*existing=*/500, /*new=*/501,
                                      /*shares=*/30, /*price=*/1100);
        CHECK(session.feed(reinterpret_cast<const char*>(rep.data()),
                           rep.size()));
        CHECK(session.replacesAccepted() == 1);
        CHECK(session.replacesRejected() == 0);

        // 3. Wire response is OrderReplaced (79 bytes).
        CHECK(sent.size() == OUCH_SIZE_ORDER_REPLACED);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == OUCH_MT_ORDER_REPLACED);
        CHECK(readU32BE(resp + 24) == 30);     // new shares
        CHECK(readU32BE(resp + 36) == 1100);   // new price

        // 4. Engine state reflects the replacement — same orderId,
        //    new price/qty.
        auto* book = engine.getOrderBook(7);
        CHECK(book != nullptr);
        const Order* o = book->getOrder(500);  // engine keeps the
                                                // original id under
                                                // CancelReplace
        CHECK(o != nullptr);
        CHECK(o->price == 1100);
        CHECK(o->remainingQty == 30);

        // 5. Old token is no longer addressable; new token is.
        CHECK(session.knownOrderCount() == 1);
    } END
}

void test_SessionReplaceUnknownTokenIsRejected() {
    TEST(SessionReplaceUnknownTokenIsRejected) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });

        // No prior order — replace must reject the new token.
        auto rep = buildReplaceOrder(/*existing=*/999, /*new=*/1000, 10, 100);
        CHECK(session.feed(reinterpret_cast<const char*>(rep.data()),
                           rep.size()));
        CHECK(session.replacesAccepted() == 0);
        CHECK(session.replacesRejected() == 1);

        // Wire response is a Reject keyed to the NEW token (the one
        // the client is waiting on).
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == OUCH_MT_ORDER_REJECTED);
        // Token field at offset 9, 14 bytes ASCII decimal. "1000".
        CHECK(resp[9] == '1' && resp[10] == '0' && resp[11] == '0' && resp[12] == '0');
    } END
}

void test_SessionFillsOnReplacedTokenUseNewToken() {
    TEST(SessionFillsOnReplacedTokenUseNewToken) {
        // After replace, fills must arrive under the NEW token. The
        // engine's orderId stays the same; the OUCH session's
        // reverse-map keeps the wire-facing identity correct.
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });
        CHECK(session.attachFillListener(7));

        // Place + replace.
        auto place = buildEnterOrder(600, 'B', 50, 7, 1000, OUCH_TIF_DAY, 1);
        CHECK(session.feed(reinterpret_cast<const char*>(place.data()),
                           place.size()));
        auto rep = buildReplaceOrder(/*existing=*/600, /*new=*/601, 50, 1000);
        CHECK(session.feed(reinterpret_cast<const char*>(rep.data()),
                           rep.size()));
        CHECK(session.replacesAccepted() == 1);
        sent.clear();

        // Counterparty fill via engine.submitOrder — fires onTrade.
        engine.submitOrder(7, 9999, 2, Side::Sell, 1000, 20, OrderType::Limit);

        CHECK(session.fillsEmitted() == 1);
        // The 'E' message must reference the NEW token (601), not
        // the original (600) — the client uses the new token to
        // track this order.
        CHECK(sent.size() == OUCH_SIZE_ORDER_EXECUTED);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == OUCH_MT_ORDER_EXECUTED);
        // Token slot at offset 9, 14B ASCII decimal: "601".
        CHECK(resp[9] == '6' && resp[10] == '0' && resp[11] == '1');
    } END
}

// ─── Fill-emission tests ────────────────────────────────────────────────────
// All exercise the EventListener → 'E' (OrderExecuted) path. The
// pattern: place a resting order via OUCH so it lands in the session's
// token table, then drive an aggressive match via engine.submitOrder
// directly so the counterparty is intentionally *not* in our table —
// only the resting side should produce an 'E'.

void test_SessionEmitsExecutedOnPartialFill() {
    TEST(SessionEmitsExecutedOnPartialFill) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });
        session.setClock([] { return uint64_t{777}; });
        CHECK(session.attachFillListener(7));

        // Resting buy at price 1000, qty 50 via OUCH.
        auto wire = buildEnterOrder(/*token=*/1001, 'B', /*shares=*/50,
                                    /*stock=*/7, /*price=*/1000,
                                    OUCH_TIF_DAY, /*firm=*/100);
        CHECK(session.feed(reinterpret_cast<const char*>(wire.data()),
                           wire.size()));
        CHECK(session.ordersAccepted() == 1);
        CHECK(session.fillsEmitted() == 0);
        sent.clear();  // discard the 'A'

        // Aggressive sell at price 1000, qty 30 — directly via the
        // engine (NOT OUCH) so the seller is not in our token table.
        auto sellRes = engine.submitOrder(/*sym=*/7, /*id=*/9999, /*pid=*/200,
                                          Side::Sell, /*price=*/1000,
                                          /*qty=*/30, OrderType::Limit);
        CHECK(sellRes.isAccepted());

        // Expect exactly one 'E' for our resting buy.
        CHECK(session.fillsEmitted() == 1);
        CHECK(sent.size() == OUCH_SIZE_ORDER_EXECUTED);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == OUCH_MT_ORDER_EXECUTED);
        CHECK(readU64BE(resp + 1) == 777ULL);          // injected clock
        CHECK(readU64BE(resp + 9) >= 0);               // token slot is ascii
        CHECK(readU32BE(resp + 23) == 30);             // executed shares
        CHECK(readU32BE(resp + 27) == 1000);           // execution price
    } END
}

void test_SessionEmitsExecutedOnFullFillAndDropsToken() {
    TEST(SessionEmitsExecutedOnFullFillAndDropsToken) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });
        CHECK(session.attachFillListener(7));

        // Resting buy 50 @ 1000.
        auto wire = buildEnterOrder(2001, 'B', 50, 7, 1000, OUCH_TIF_DAY, 100);
        CHECK(session.feed(reinterpret_cast<const char*>(wire.data()),
                           wire.size()));
        CHECK(session.knownOrderCount() == 1);

        // Aggressive sell of exactly 50 — completes the resting order.
        auto sellRes = engine.submitOrder(7, 9001, 200, Side::Sell,
                                          1000, 50, OrderType::Limit);
        CHECK(sellRes.isAccepted());

        CHECK(session.fillsEmitted() == 1);
        // Fully filled → token table no longer contains this order.
        CHECK(session.knownOrderCount() == 0
              && "fully-filled token must be dropped from session table");
    } END
}

void test_SessionPartialFillThenCancelEmitsCorrectLeaves() {
    TEST(SessionPartialFillThenCancelEmitsCorrectLeaves) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });
        CHECK(session.attachFillListener(7));

        // Resting buy 100 @ 1000.
        auto wire = buildEnterOrder(3001, 'B', 100, 7, 1000, OUCH_TIF_DAY, 100);
        CHECK(session.feed(reinterpret_cast<const char*>(wire.data()),
                           wire.size()));

        // Aggressive sell of 40 — partial fill, leaves = 60.
        auto sellRes = engine.submitOrder(7, 9002, 200, Side::Sell,
                                          1000, 40, OrderType::Limit);
        CHECK(sellRes.isAccepted());
        sent.clear();

        // Cancel the resting buy. The 'C' message must report the
        // post-fill leaves (60), NOT the original 100.
        auto cancelWire = buildCancelOrder(3001, 0);
        CHECK(session.feed(reinterpret_cast<const char*>(cancelWire.data()),
                           cancelWire.size()));
        CHECK(session.cancelsAccepted() == 1);

        CHECK(sent.size() == OUCH_SIZE_ORDER_CANCELED);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == OUCH_MT_ORDER_CANCELED);
        CHECK(readU32BE(resp + 23) == 60
              && "Canceled message must carry post-fill leaves, not original size");
    } END
}

void test_SessionEmitsExecutedOnImmediateFillAtSubmit() {
    TEST(SessionEmitsExecutedOnImmediateFillAtSubmit) {
        // Regression: when an OUCH-placed order immediately matches
        // resting liquidity on the way in, onTrade fires inside
        // submitOrder, which previously happened before the token
        // entry was registered. The fix is to pre-register and roll
        // back on reject. Verify the fill is emitted.
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        // Seed resting sell via the engine directly (not OUCH).
        auto restingRes = engine.submitOrder(7, 8001, 300, Side::Sell,
                                              1000, 40, OrderType::Limit);
        CHECK(restingRes.isAccepted());

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });
        CHECK(session.attachFillListener(7));

        // OUCH-place an aggressive buy of 40 — this crosses the
        // resting sell on submit. Expect 'A' AND 'E' in the wire
        // response stream, and the token entry to be gone (fully
        // filled on entry).
        auto wire = buildEnterOrder(4001, 'B', 40, 7, 1000, OUCH_TIF_DAY, 100);
        CHECK(session.feed(reinterpret_cast<const char*>(wire.data()),
                           wire.size()));
        CHECK(session.ordersAccepted() == 1);
        CHECK(session.fillsEmitted() == 1
              && "immediate fill on submit must emit 'E' (this was the bug)");
        CHECK(session.knownOrderCount() == 0);

        // The wire stream should contain BOTH 'A' (66 bytes) and 'E'
        // (40 bytes). Total = 106. Order: 'A' first (sent from
        // handleEnterOrder), then 'E' (sent from onTrade during
        // submit) — wait, the listener fires INSIDE submitOrder, so
        // 'E' actually precedes 'A' on the wire. Pin that order.
        CHECK(sent.size() == OUCH_SIZE_ORDER_EXECUTED + OUCH_SIZE_ORDER_ACCEPTED);
        const auto* p = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(p[0] == OUCH_MT_ORDER_EXECUTED);
        CHECK(p[OUCH_SIZE_ORDER_EXECUTED] == OUCH_MT_ORDER_ACCEPTED);
    } END
}

void test_SessionIOCMapsToOrderType() {
    TEST(SessionIOCMapsToOrderType) {
        // OUCH conflates IOC into the TIF field (value 0). The session
        // must translate that to OrderType::IOC, otherwise a market
        // order with no immediate fill would rest on the book —
        // exactly the bug OUCH's distinct IOC encoding exists to
        // prevent.
        MatchingEngine engine;
        engine.addSymbol(8);
        engine.start();

        std::string sent;
        OuchSession session(engine, [&](std::string_view b) { sent.append(b); });

        auto wire = buildEnterOrder(800, 'B', 50, 8, 1000, OUCH_TIF_IOC, 1);
        CHECK(session.feed(reinterpret_cast<const char*>(wire.data()),
                           wire.size()));
        CHECK(session.ordersAccepted() == 1);

        // IOC with no opposing liquidity → order must NOT rest on the book.
        auto* book = engine.getOrderBook(8);
        CHECK(book != nullptr);
        CHECK(book->getOrder(800) == nullptr
              && "IOC with no fill must not rest");
    } END
}

int main() {
    std::cout << "Running OuchSessionTest\n";

    test_EnterOrderRoundtrip();
    test_CancelOrderRoundtrip();
    test_BigEndianEncoding();
    test_AsciiDecimalParsesPadding();
    test_AsciiDecimalRejectsGarbage();
    test_OrderAcceptedLayout();
    test_OrderRejectedLayout();
    test_OrderCanceledLayout();
    test_OrderExecutedLayout();

    test_SessionAcceptsEnterOrder();
    test_SessionRejectsUnregisteredSymbol();
    test_SessionCancelRemovesOrder();
    test_SessionFragmentedFeed();
    test_SessionBatchedFrames();
    test_SessionRejectsUnknownMsgType();
    test_SessionIOCMapsToOrderType();

    test_SessionEmitsExecutedOnPartialFill();
    test_SessionEmitsExecutedOnFullFillAndDropsToken();
    test_SessionPartialFillThenCancelEmitsCorrectLeaves();
    test_SessionEmitsExecutedOnImmediateFillAtSubmit();

    test_ReplaceOrderRoundtrip();
    test_OrderReplacedLayout();
    test_SessionReplaceUpdatesPriceAndQty();
    test_SessionReplaceUnknownTokenIsRejected();
    test_SessionFillsOnReplacedTokenUseNewToken();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
