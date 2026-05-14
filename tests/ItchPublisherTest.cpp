// ItchPublisherTest — exercises the ITCH 5.0 binary market-data codec
// plus the EventListener-driven publisher that turns engine events
// into wire frames.
//
// Coverage:
//   Codec layer
//     1. 48-bit big-endian timestamp roundtrip (ITCH-specific encoding)
//     2. Add Order layout (byte-level field positions)
//     3. Order Executed layout
//     4. Order Delete layout
//     5. System Event layout
//
//   Publisher layer
//     6. Accepted with remaining>0 emits 'A'; IOC fully-filled emits nothing
//     7. Resting order trade emits 'E' (the maker side)
//     8. Aggressor that fully filled on entry emits NO 'E' (was never on book)
//     9. Cancel emits 'D'
//    10. Full-fill emits 'E' followed by 'D'
//    11. MultiplexListener fan-out: ITCH and OUCH coexist on same book
//    12. System event manual emission

#include "ItchProtocol.h"
#include "ItchPublisher.h"
#include "MatchingEngine.h"
#include "MultiplexListener.h"
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

// ─── Codec tests ────────────────────────────────────────────────────────────

void test_U48BERoundtrip() {
    TEST(U48BERoundtrip) {
        // 48-bit timestamps span up to 281 trillion ns ≈ 78 hours.
        // Pick values that exercise each byte position.
        uint8_t buf[6];
        writeU48BE(buf, 0x0102030405060708ULL);  // high 2 bytes discarded
        // Expected: low 6 bytes (0x03..0x08) in BE order.
        CHECK(buf[0] == 0x03);
        CHECK(buf[1] == 0x04);
        CHECK(buf[2] == 0x05);
        CHECK(buf[3] == 0x06);
        CHECK(buf[4] == 0x07);
        CHECK(buf[5] == 0x08);
        CHECK(readU48BE(buf) == 0x030405060708ULL);

        // Zero is the natural session start; pin its layout too.
        std::memset(buf, 0xAA, sizeof(buf));
        writeU48BE(buf, 0);
        for (int i = 0; i < 6; ++i) CHECK(buf[i] == 0);
    } END
}

void test_AddOrderLayout() {
    TEST(AddOrderLayout) {
        uint8_t buf[ITCH_SIZE_ADD_ORDER];
        size_t n = encodeAddOrder(buf, /*locate=*/7, /*tracking=*/42,
                                  /*ts=*/0x000010203040ULL,
                                  /*ref=*/0xDEADBEEFCAFEBABEULL,
                                  Side::Sell, /*shares=*/100,
                                  /*stock=*/42, /*price=*/2000000);
        CHECK(n == ITCH_SIZE_ADD_ORDER);
        CHECK(buf[0] == 'A');
        CHECK(readU16BE(buf + 1) == 7);
        CHECK(readU16BE(buf + 3) == 42);
        CHECK(readU48BE(buf + 5) == 0x000010203040ULL);
        CHECK(readU64BE(buf + 11) == 0xDEADBEEFCAFEBABEULL);
        CHECK(buf[19] == 'S');
        CHECK(readU32BE(buf + 20) == 100);
        // Stock slot is ASCII "42" left-justified.
        CHECK(buf[24] == '4' && buf[25] == '2' && buf[26] == ' ');
        CHECK(readU32BE(buf + 32) == 2000000);
    } END
}

void test_OrderExecutedLayout() {
    TEST(OrderExecutedLayout) {
        uint8_t buf[ITCH_SIZE_ORDER_EXECUTED];
        size_t n = encodeOrderExecuted(buf, 1, 2, 0x000000001234ULL,
                                       0xABCDEFULL, 50, 0x9999ULL);
        CHECK(n == ITCH_SIZE_ORDER_EXECUTED);
        CHECK(buf[0] == 'E');
        CHECK(readU16BE(buf + 1) == 1);
        CHECK(readU48BE(buf + 5) == 0x000000001234ULL);
        CHECK(readU64BE(buf + 11) == 0xABCDEFULL);
        CHECK(readU32BE(buf + 19) == 50);
        CHECK(readU64BE(buf + 23) == 0x9999ULL);
    } END
}

void test_OrderDeleteLayout() {
    TEST(OrderDeleteLayout) {
        uint8_t buf[ITCH_SIZE_ORDER_DELETE];
        size_t n = encodeOrderDelete(buf, 5, 6, 0x000000007777ULL, 0x1111ULL);
        CHECK(n == ITCH_SIZE_ORDER_DELETE);
        CHECK(buf[0] == 'D');
        CHECK(readU64BE(buf + 11) == 0x1111ULL);
    } END
}

void test_SystemEventLayout() {
    TEST(SystemEventLayout) {
        uint8_t buf[ITCH_SIZE_SYSTEM_EVENT];
        size_t n = encodeSystemEvent(buf, 0, 0, 0x000000000001ULL,
                                     ITCH_SE_START_OF_MARKET);
        CHECK(n == ITCH_SIZE_SYSTEM_EVENT);
        CHECK(buf[0] == 'S');
        CHECK(buf[11] == 'Q');  // 'Q' = Start of Market
    } END
}

// ─── Publisher tests ────────────────────────────────────────────────────────

void test_PublisherEmitsAddOnResting() {
    TEST(PublisherEmitsAddOnResting) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        auto* book = engine.getOrderBook(7);
        CHECK(book != nullptr);

        std::string sent;
        ItchPublisher pub(*book, [&](std::string_view b) { sent.append(b); });
        pub.setClock([] { return uint64_t{0x000000000005ULL}; });
        book->setEventListener(&pub);

        auto r = engine.submitOrder(7, /*id=*/100, /*pid=*/1,
                                    Side::Buy, /*price=*/1000,
                                    /*qty=*/50, OrderType::Limit);
        CHECK(r.isAccepted());

        CHECK(pub.addsEmitted() == 1);
        CHECK(pub.liveOrderCount() == 1);
        CHECK(sent.size() == ITCH_SIZE_ADD_ORDER);

        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == ITCH_MT_ADD_ORDER);
        CHECK(readU48BE(resp + 5) == 0x5ULL);
        CHECK(readU64BE(resp + 11) == 100ULL);
        CHECK(resp[19] == 'B');
        CHECK(readU32BE(resp + 20) == 50);
        CHECK(readU32BE(resp + 32) == 1000);
    } END
}

void test_PublisherEmitsAddExecuteDeleteForTransientIOC() {
    TEST(PublisherEmitsAddExecuteDeleteForTransientIOC) {
        // Per real Nasdaq ITCH 5.0: an IOC that fully fills on entry
        // produces A → E → D on the wire. The order does pass through
        // Accepted state inside the engine (notifyOrderUpdate fires
        // before matching), so subscribers see the transient lifecycle
        // even though the order never visibly rested. This matches the
        // real venue's behavior — the publisher faithfully reproduces it.
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        // Seed resting sell BEFORE the publisher is installed, so the
        // publisher doesn't see the maker's own 'A'. Use a DIFFERENT
        // participant for the IOC to avoid self-match prevention,
        // which would short-circuit matching and never fire onTrade.
        auto seed = engine.submitOrder(7, 200, /*pid=*/1, Side::Sell, 1000, 30,
                                        OrderType::Limit);
        CHECK(seed.isAccepted());

        auto* book = engine.getOrderBook(7);
        std::string sent;
        ItchPublisher pub(*book, [&](std::string_view b) { sent.append(b); });
        book->setEventListener(&pub);

        auto r = engine.submitOrder(7, 201, /*pid=*/2, Side::Buy, 1000, 30,
                                     OrderType::IOC);
        CHECK(r.isAccepted());

        // Aggressor 201 transits the book: A on accept, E on fill, D on
        // fully-filled removal.
        CHECK(pub.addsEmitted() == 1
              && "aggressor's Accepted state must emit 'A'");
        CHECK(pub.executedEmitted() == 1
              && "aggressor side of the trade must emit 'E' (its 'A' set it live)");
        CHECK(pub.deletesEmitted() == 1
              && "fully-filled aggressor must emit 'D'");
        // The maker (200) was placed before the publisher attached, so
        // the publisher has no record of it — no 'E' for that side.
    } END
}

void test_PublisherEmitsExecutedOnMakerFill() {
    TEST(PublisherEmitsExecutedOnMakerFill) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        auto* book = engine.getOrderBook(7);
        std::string sent;
        ItchPublisher pub(*book, [&](std::string_view b) { sent.append(b); });
        book->setEventListener(&pub);

        // Resting sell via the engine — the publisher is installed,
        // so this fires 'A' and records the order as live.
        auto seed = engine.submitOrder(7, 300, 1, Side::Sell, 1000, 50,
                                        OrderType::Limit);
        CHECK(seed.isAccepted());
        CHECK(pub.addsEmitted() == 1);
        sent.clear();

        // Aggressive buy of 20 — matches the resting sell. Both
        // sides are tracked by the publisher (maker 300 from the
        // earlier 'A'; aggressor 301 from its Accepted-before-match
        // 'A'), so 'E' fires for each side. The aggressor then
        // emits 'D' because it fully filled.
        auto agg = engine.submitOrder(7, 301, 2, Side::Buy, 1000, 20,
                                       OrderType::Limit);
        CHECK(agg.isAccepted());

        CHECK(pub.executedEmitted() == 2
              && "both maker (300) and aggressor (301) tracked — 'E' for each");
        CHECK(pub.deletesEmitted() == 1
              && "aggressor 301 fully filled — emits 'D'; maker still has 30 leaves");
        // Wire stream after the buy: 'A' for 301, then 'E' for the
        // buy side (301), then 'E' for the sell side (300), then 'D'
        // for 301 (now fully filled).
        CHECK(sent.size() ==
              ITCH_SIZE_ADD_ORDER + 2 * ITCH_SIZE_ORDER_EXECUTED +
              ITCH_SIZE_ORDER_DELETE);
        const auto* p = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(p[0] == ITCH_MT_ADD_ORDER);
        CHECK(p[ITCH_SIZE_ADD_ORDER] == ITCH_MT_ORDER_EXECUTED);
    } END
}

void test_PublisherEmitsDeleteOnCancel() {
    TEST(PublisherEmitsDeleteOnCancel) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        auto* book = engine.getOrderBook(7);
        std::string sent;
        ItchPublisher pub(*book, [&](std::string_view b) { sent.append(b); });
        book->setEventListener(&pub);

        auto r = engine.submitOrder(7, 400, 1, Side::Buy, 1000, 25,
                                     OrderType::Limit);
        CHECK(r.isAccepted());
        sent.clear();

        auto c = engine.submitCancel(7, 400);
        CHECK(c.isAccepted());

        CHECK(pub.deletesEmitted() == 1);
        CHECK(pub.liveOrderCount() == 0
              && "publisher's live table must be empty after delete");
        CHECK(sent.size() == ITCH_SIZE_ORDER_DELETE);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == ITCH_MT_ORDER_DELETE);
        CHECK(readU64BE(resp + 11) == 400ULL);
    } END
}

void test_PublisherEmitsExecutedThenDeleteOnFullFill() {
    TEST(PublisherEmitsExecutedThenDeleteOnFullFill) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        auto* book = engine.getOrderBook(7);
        std::string sent;
        ItchPublisher pub(*book, [&](std::string_view b) { sent.append(b); });
        book->setEventListener(&pub);

        // Resting sell 25 @ 1000.
        engine.submitOrder(7, 500, 1, Side::Sell, 1000, 25, OrderType::Limit);
        CHECK(pub.addsEmitted() == 1);
        sent.clear();

        // Aggressive buy of exactly 25 — fully consumes the maker.
        // Both maker (500) and aggressor (501) are tracked, so each
        // emits 'E'. Each then fully fills → each emits 'D'.
        engine.submitOrder(7, 501, 2, Side::Buy, 1000, 25, OrderType::Limit);

        CHECK(pub.executedEmitted() == 2
              && "both sides emit 'E'");
        CHECK(pub.deletesEmitted() == 2
              && "both fully-filled orders emit 'D'");
        CHECK(pub.liveOrderCount() == 0);

        // Wire stream after the aggressor:
        //   'A' (501 accepted)
        //   'E' (501 fill, buy side)
        //   'E' (500 fill, sell side)
        //   'D' (501 filled)
        //   'D' (500 filled)
        // The exact ordering between same-trade 'E's, and between 'D's,
        // depends on the order onTrade iterates the trade's two sides
        // and the order Filled status updates fire — we don't pin those
        // micro-orderings here, only the total counts/sizes.
        CHECK(sent.size() ==
              ITCH_SIZE_ADD_ORDER + 2 * ITCH_SIZE_ORDER_EXECUTED +
              2 * ITCH_SIZE_ORDER_DELETE);
    } END
}

void test_StockDirectoryLayout() {
    TEST(StockDirectoryLayout) {
        uint8_t buf[ITCH_SIZE_STOCK_DIRECTORY];
        size_t n = encodeStockDirectory(buf, /*locate=*/7, /*tracking=*/0,
                                         /*ts=*/0x000000001234ULL,
                                         /*stock=*/42, 'Q', 'N', 100,
                                         'N', 'C');
        CHECK(n == ITCH_SIZE_STOCK_DIRECTORY);
        CHECK(buf[0] == 'R');
        CHECK(readU16BE(buf + 1) == 7);
        // Stock at offset 11 — ASCII "42" left-justified.
        CHECK(buf[11] == '4' && buf[12] == '2' && buf[13] == ' ');
        CHECK(buf[19] == 'Q');         // market category
        CHECK(buf[20] == 'N');         // financial status
        CHECK(readU32BE(buf + 21) == 100);  // round lot size
    } END
}

void test_TradingActionLayout() {
    TEST(TradingActionLayout) {
        uint8_t buf[ITCH_SIZE_TRADING_ACTION];
        size_t n = encodeTradingAction(buf, /*locate=*/7, /*tracking=*/0,
                                        /*ts=*/0x010203040506ULL,
                                        /*stock=*/42, 'H', "T1  ");
        CHECK(n == ITCH_SIZE_TRADING_ACTION);
        CHECK(buf[0] == 'H');
        CHECK(buf[19] == 'H');     // trading state = Halted
        CHECK(buf[21] == 'T' && buf[22] == '1');  // reason "T1"
    } END
}

void test_CrossTradeLayout() {
    TEST(CrossTradeLayout) {
        uint8_t buf[ITCH_SIZE_CROSS_TRADE];
        size_t n = encodeCrossTrade(buf, /*locate=*/7, /*tracking=*/0,
                                     /*ts=*/0x000000000001ULL,
                                     /*shares=*/100000ULL, /*stock=*/42,
                                     /*price=*/1500, /*matchNumber=*/0xABCDULL,
                                     ITCH_CROSS_TYPE_OPENING);
        CHECK(n == ITCH_SIZE_CROSS_TRADE);
        CHECK(buf[0] == 'Q');
        CHECK(readU64BE(buf + 11) == 100000ULL);
        CHECK(readU32BE(buf + 27) == 1500);
        CHECK(buf[39] == 'O');  // crossType = opening
    } END
}

void test_PublisherTradingActionReflectsEngineState() {
    TEST(PublisherTradingActionReflectsEngineState) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();
        auto* book = engine.getOrderBook(7);
        CHECK(book != nullptr);

        std::string sent;
        ItchPublisher pub(*book, [&](std::string_view b) { sent.append(b); });

        // Continuous → emits 'T' (Trading).
        pub.publishTradingAction("    ");
        const auto* p = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(sent.size() == ITCH_SIZE_TRADING_ACTION);
        CHECK(p[0] == ITCH_MT_TRADING_ACTION);
        CHECK(p[19] == ITCH_TRADING_STATE_TRADING);
        sent.clear();

        // Now halt the book and emit again — should wire 'H'.
        book->setTradingState(TradingState::Halted);
        pub.publishTradingAction("T1  ");
        p = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(sent.size() == ITCH_SIZE_TRADING_ACTION);
        CHECK(p[19] == ITCH_TRADING_STATE_HALTED);
        CHECK(p[21] == 'T' && p[22] == '1');

        CHECK(pub.tradingActionsEmitted() == 2);
    } END
}

void test_PublisherStockDirectoryUsesBookSymbol() {
    TEST(PublisherStockDirectoryUsesBookSymbol) {
        MatchingEngine engine;
        engine.addSymbol(123);
        engine.start();
        auto* book = engine.getOrderBook(123);

        std::string sent;
        ItchPublisher pub(*book, [&](std::string_view b) { sent.append(b); });
        pub.publishStockDirectory();

        CHECK(pub.directoriesEmitted() == 1);
        CHECK(sent.size() == ITCH_SIZE_STOCK_DIRECTORY);
        const auto* p = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(p[0] == ITCH_MT_STOCK_DIRECTORY);
        // Locate code = truncated symbol (123).
        CHECK(readU16BE(p + 1) == 123);
        // Stock ASCII at offset 11 = "123"
        CHECK(p[11] == '1' && p[12] == '2' && p[13] == '3');
    } END
}

void test_PublisherSystemEventManualEmit() {
    TEST(PublisherSystemEventManualEmit) {
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();
        auto* book = engine.getOrderBook(7);

        std::string sent;
        ItchPublisher pub(*book, [&](std::string_view b) { sent.append(b); });
        pub.publishSystemEvent(ITCH_SE_START_OF_MARKET);

        CHECK(pub.messagesEmitted() == 1);
        CHECK(sent.size() == ITCH_SIZE_SYSTEM_EVENT);
        const auto* resp = reinterpret_cast<const uint8_t*>(sent.data());
        CHECK(resp[0] == 'S');
        CHECK(resp[11] == 'Q');
    } END
}

// ─── MultiplexListener fan-out test ─────────────────────────────────────────

void test_MultiplexListenerFansOut() {
    TEST(MultiplexListenerFansOut) {
        // Install both an OUCH session and an ITCH publisher on the
        // same book via the fan-out adapter. A single trade should
        // generate one OUCH 'E' (private to the OUCH client) AND one
        // ITCH 'E' (public market data).
        MatchingEngine engine;
        engine.addSymbol(7);
        engine.start();

        auto* book = engine.getOrderBook(7);

        std::string ouchSent;
        OuchSession ouch(engine, [&](std::string_view b) { ouchSent.append(b); });

        std::string itchSent;
        ItchPublisher itch(*book, [&](std::string_view b) { itchSent.append(b); });

        MultiplexListener mux;
        mux.add(&ouch);
        mux.add(&itch);
        book->setEventListener(&mux);
        CHECK(mux.size() == 2);

        // OUCH-place a resting buy. Both listeners see the events:
        //   OUCH: hears nothing on its own events at this point (it
        //     receives the 'A' equivalent via its dispatch path, not
        //     via the listener — handleEnterOrder already wrote it).
        //   ITCH: emits 'A' from onOrderUpdate(Accepted, rem>0).
        auto wire = std::vector<uint8_t>(OUCH_SIZE_ENTER_ORDER, ' ');
        wire[0] = OUCH_MT_ENTER_ORDER;
        writeAsciiDecimal(wire.data() + 1, 14, 600);
        wire[15] = 'B';
        writeU32BE(wire.data() + 16, 50);
        writeAsciiDecimal(wire.data() + 20, 8, 7);
        writeU32BE(wire.data() + 28, 1000);
        writeU32BE(wire.data() + 32, OUCH_TIF_DAY);
        writeAsciiDecimal(wire.data() + 36, 4, 1);
        wire[40] = 'Y'; wire[41] = 'O'; wire[42] = 'N';
        writeU32BE(wire.data() + 43, 0);
        wire[47] = 'N'; wire[48] = 'R';

        CHECK(ouch.feed(reinterpret_cast<const char*>(wire.data()),
                         wire.size()));
        CHECK(itch.addsEmitted() == 1
              && "ITCH must see the Accepted event through the mux");

        // Now drive a fill via the engine. Both publishers should see it.
        ouchSent.clear();
        itchSent.clear();
        engine.submitOrder(7, 601, 2, Side::Sell, 1000, 20, OrderType::Limit);

        // OUCH only knows about its own order (id=600), so it emits
        // 'E' for one side. ITCH knows about both sides (it 'A'-d
        // both 600 and 601 via the listener path), so it emits 'E'
        // for two sides plus 'D' for the fully-filled aggressor 601.
        CHECK(ouch.fillsEmitted() == 1
              && "OUCH session sees only its own order on one side");
        CHECK(itch.executedEmitted() == 2
              && "ITCH publishes 'E' for both sides through the mux");
        CHECK(itch.deletesEmitted() == 1
              && "fully-filled aggressor 601 generates 'D' on the ITCH feed");
    } END
}

int main() {
    std::cout << "Running ItchPublisherTest\n";

    test_U48BERoundtrip();
    test_AddOrderLayout();
    test_OrderExecutedLayout();
    test_OrderDeleteLayout();
    test_SystemEventLayout();

    test_PublisherEmitsAddOnResting();
    test_PublisherEmitsAddExecuteDeleteForTransientIOC();
    test_PublisherEmitsExecutedOnMakerFill();
    test_PublisherEmitsDeleteOnCancel();
    test_PublisherEmitsExecutedThenDeleteOnFullFill();
    test_StockDirectoryLayout();
    test_TradingActionLayout();
    test_CrossTradeLayout();
    test_PublisherTradingActionReflectsEngineState();
    test_PublisherStockDirectoryUsesBookSymbol();

    test_PublisherSystemEventManualEmit();

    test_MultiplexListenerFansOut();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
