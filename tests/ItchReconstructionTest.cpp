// ItchReconstructionTest — feed-fidelity test for the order-level (L3/MBO) feed.
//
// Every other ITCH test checks that a message has the right bytes. This one
// checks the only property that actually matters to a subscriber: replay the
// whole byte stream into an independent book and you get the engine's book
// back.
//
// The reference is OrderBook::getSnapshot(), the L2 projection — which
// excludes hidden orders and counts an iceberg's visible slice rather than its
// true size. Holding the L3 replay against it pins the two market-data feeds
// to each other; they cannot drift into describing different books, which is
// exactly what happened before the BookVisibleUpdate channel existed (the L2
// path filtered hidden/iceberg, while the ITCH path republished the private
// Accepted notification verbatim and leaked both).
//
// Coverage:
//   1. Plain limit book: rests, partial fills, full fills, cancels, sweeps
//   2. Hidden orders never appear on the feed at all
//   3. Iceberg advertises only its slice, and survives a slice refresh
//   4. Modify-down emits 'X' so the replay does not keep the stale size
//   5. cancelReplace: shrink in place, grow with loss of priority, reprice
//      into crossing liquidity, and the iceberg/hidden variants
//   6. Stops are invisible while parked and enter the feed when they rest
//   7. Cancelling a parked order must not disturb its price level
//   8. Auction accumulation and uncross, including icebergs
//   9. Randomised soak: mixed order types, replay must match at every step

#include "ItchProtocol.h"
#include "ItchPublisher.h"
#include "MatchingEngine.h"
#include "OrderBook.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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

namespace {

// ─── Subscriber-side book reconstruction ────────────────────────────────────
//
// Deliberately dumb and independent of the engine: a flat id → order map fed
// only by the wire bytes. If it needed engine internals to stay in sync, it
// would not be testing what a real subscriber can actually do.

size_t itchMessageSize(uint8_t messageType) {
    switch (messageType) {
    case ITCH_MT_SYSTEM_EVENT:      return ITCH_SIZE_SYSTEM_EVENT;
    case ITCH_MT_STOCK_DIRECTORY:   return ITCH_SIZE_STOCK_DIRECTORY;
    case ITCH_MT_TRADING_ACTION:    return ITCH_SIZE_TRADING_ACTION;
    case ITCH_MT_ADD_ORDER:         return ITCH_SIZE_ADD_ORDER;
    case ITCH_MT_ORDER_EXECUTED:    return ITCH_SIZE_ORDER_EXECUTED;
    case ITCH_MT_ORDER_EXECUTED_PX: return ITCH_SIZE_ORDER_EXECUTED_PX;
    case ITCH_MT_ORDER_CANCEL:      return ITCH_SIZE_ORDER_CANCEL;
    case ITCH_MT_ORDER_DELETE:      return ITCH_SIZE_ORDER_DELETE;
    case ITCH_MT_TRADE:             return ITCH_SIZE_TRADE;
    case ITCH_MT_CROSS_TRADE:       return ITCH_SIZE_CROSS_TRADE;
    default:                        return 0;  // unknown type: unrecoverable
    }
}

class Reconstructor {
public:
    struct Entry {
        Side     side;
        Price    price;
        Quantity shares;
    };

    // Sink for ItchPublisher. Buffers because a transport is free to coalesce
    // frames; the parser must not assume one message per callback.
    void feed(std::string_view bytes) {
        buffer_.append(bytes);
        size_t offset = 0;
        while (offset < buffer_.size()) {
            const auto* p = reinterpret_cast<const uint8_t*>(buffer_.data()) + offset;
            const size_t size = itchMessageSize(p[0]);
            if (size == 0) throw std::runtime_error("unknown ITCH message type");
            if (offset + size > buffer_.size()) break;  // partial frame
            apply(p);
            offset += size;
        }
        buffer_.erase(0, offset);
    }

    // Aggregate the reconstructed orders into price levels, matching what
    // getSnapshot() reports. Zero-share entries are dropped: an order whose
    // shares reached zero via 'E' is awaiting its 'D' and is no longer
    // displayed, so it must not contribute a phantom level.
    std::map<Price, Quantity> depth(Side side) const {
        std::map<Price, Quantity> levels;
        for (const auto& [id, o] : orders_) {
            (void)id;
            if (o.side != side || o.shares == 0) continue;
            levels[o.price] += o.shares;
        }
        return levels;
    }

    bool   knows(OrderId id) const { return orders_.count(id) != 0; }
    size_t liveCount()       const { return orders_.size(); }

    // Per-order detail at one level, for diagnosing a depth mismatch.
    std::string describeLevel(Side side, Price price) const {
        std::string s;
        for (const auto& [id, o] : orders_) {
            if (o.side != side || o.price != price) continue;
            s += " #" + std::to_string(id) + "x" + std::to_string(o.shares);
        }
        return s.empty() ? std::string(" (none)") : s;
    }

private:
    void apply(const uint8_t* p) {
        const uint8_t type = p[0];
        // Every order-scoped message carries its reference at offset 11.
        const auto ref = static_cast<OrderId>(readU64BE(p + 11));

        switch (type) {
        case ITCH_MT_ADD_ORDER: {
            Entry e{};
            e.side   = (p[19] == 'B') ? Side::Buy : Side::Sell;
            e.shares = static_cast<Quantity>(readU32BE(p + 20));
            e.price  = static_cast<Price>(readU32BE(p + 32));
            orders_[ref] = e;
            break;
        }
        case ITCH_MT_ORDER_EXECUTED:
            reduce(ref, static_cast<Quantity>(readU32BE(p + 19)));
            break;
        case ITCH_MT_ORDER_CANCEL:
            reduce(ref, static_cast<Quantity>(readU32BE(p + 19)));
            break;
        case ITCH_MT_ORDER_DELETE:
            orders_.erase(ref);
            break;
        default:
            // S / R / H / P / Q carry no displayed-book state.
            break;
        }
    }

    // A subscriber handed more shares than it is tracking has lost sync —
    // that is the bug this whole file exists to catch, so it throws rather
    // than saturating and hiding the drift.
    void reduce(OrderId ref, Quantity qty) {
        auto it = orders_.find(ref);
        if (it == orders_.end())
            throw std::runtime_error("message for an order never added to the feed");
        if (qty > it->second.shares)
            throw std::runtime_error("reduction exceeds displayed shares (feed drift)");
        it->second.shares -= qty;
    }

    std::string                        buffer_;
    std::unordered_map<OrderId, Entry> orders_;
};

// ─── Engine-side reference ──────────────────────────────────────────────────

std::map<Price, Quantity> snapshotDepth(const OrderBook& book, Side side) {
    std::map<Price, Quantity> levels;
    const MarketDataSnapshot snap = book.getSnapshot(MarketDataSnapshot::MAX_DEPTH);
    const size_t count = (side == Side::Buy) ? snap.bidCount : snap.askCount;
    const PriceLevel* src = (side == Side::Buy) ? snap.bids : snap.asks;
    for (size_t i = 0; i < count; ++i) {
        if (src[i].totalQuantity == 0) continue;
        levels[src[i].price] = src[i].totalQuantity;
    }
    return levels;
}

std::string describe(const std::map<Price, Quantity>& levels) {
    std::string s = "{";
    for (const auto& [price, qty] : levels) {
        s += " " + std::to_string(price) + ":" + std::to_string(qty);
    }
    return s + " }";
}

// The core assertion of this file. Reports the diff rather than just failing —
// "the books differ" is useless when the soak test finds drift at step 300.
void assertSideMatches(const Reconstructor& reco, const OrderBook& book, Side side,
                       const char* label) {
    const auto replayed = reco.depth(side);
    const auto actual   = snapshotDepth(book, side);
    if (replayed == actual) return;

    // Name the first divergent level and show both sides order-by-order —
    // an aggregate diff alone rarely identifies which order went wrong.
    std::string detail;
    for (const auto& [price, qty] : replayed) {
        auto it = actual.find(price);
        if (it != actual.end() && it->second == qty) continue;
        detail = "\n      first divergent level: " + std::to_string(price) +
                 " (feed " + std::to_string(qty) + ", engine " +
                 (it == actual.end() ? "absent" : std::to_string(it->second)) + ")" +
                 "\n      feed orders there: " + reco.describeLevel(side, price) +
                 "\n      engine orders there:";
        const Price divergent = price;
        book.forEachOrder([&](const Order& o) {
            if (o.side != side || o.price != divergent) return;
            detail += " #" + std::to_string(o.id) + "x" +
                      std::to_string(displayQuantity(o)) +
                      "[type=" + std::to_string(static_cast<int>(o.type)) +
                      (o.inBook ? ",inBook" : ",PARKED") +
                      (o.isHidden ? ",hidden" : "") + "]";
        });
        break;
    }

    throw std::runtime_error(std::string("book mismatch on ") + label +
                             "\n      replayed from feed: " + describe(replayed) +
                             "\n      engine snapshot:    " + describe(actual) + detail);
}

void assertBooksMatch(const Reconstructor& reco, const OrderBook& book) {
    assertSideMatches(reco, book, Side::Buy, "bids");
    assertSideMatches(reco, book, Side::Sell, "asks");
}

// Wire an engine + publisher + reconstructor together for one symbol.
struct Fixture {
    MatchingEngine                 engine;
    OrderBook*                     book = nullptr;
    Reconstructor                  reco;
    std::unique_ptr<ItchPublisher> pub;

    explicit Fixture(SymbolId symbol = 1) {
        // addSymbol before start(): start() freezes the book set.
        engine.addSymbol(symbol);
        engine.start();
        book = engine.getOrderBook(symbol);
        if (!book) throw std::runtime_error("no book for symbol");
        pub = std::make_unique<ItchPublisher>(
            *book, [this](std::string_view b) { reco.feed(b); });
        book->setEventListener(pub.get());
    }

    void check() const { assertBooksMatch(reco, *book); }
};

}  // namespace

// ─── Tests ──────────────────────────────────────────────────────────────────

void test_PlainLimitBookReconstructs() {
    TEST(PlainLimitBookReconstructs) {
        Fixture f;
        // Two-sided book, several levels, multiple orders per level.
        f.engine.submitOrder(1, 100, 1, Side::Buy,  1000, 50, OrderType::Limit);
        f.engine.submitOrder(1, 101, 2, Side::Buy,  1000, 30, OrderType::Limit);
        f.engine.submitOrder(1, 102, 1, Side::Buy,   999, 70, OrderType::Limit);
        f.engine.submitOrder(1, 200, 3, Side::Sell, 1001, 40, OrderType::Limit);
        f.engine.submitOrder(1, 201, 3, Side::Sell, 1002, 60, OrderType::Limit);
        f.check();

        // Partial fill of a resting order.
        f.engine.submitOrder(1, 300, 4, Side::Buy, 1001, 15, OrderType::Limit);
        f.check();

        // Full fill of a resting order.
        f.engine.submitOrder(1, 301, 4, Side::Buy, 1001, 25, OrderType::Limit);
        f.check();

        // Cancel.
        f.engine.cancelOrder(1, 102);
        f.check();

        // Sweep that crosses several levels and leaves a residual resting.
        f.engine.submitOrder(1, 302, 5, Side::Sell, 999, 100, OrderType::Limit);
        f.check();
    } END
}

void test_HiddenOrderNeverReachesFeed() {
    TEST(HiddenOrderNeverReachesFeed) {
        Fixture f;
        f.engine.submitOrder(1, 100, 1, Side::Buy, 1000, 50, OrderType::Limit);
        const uint64_t addsAfterVisible = f.pub->addsEmitted();

        // Hidden order rests at a BETTER price than the displayed one. If the
        // publisher leaked it, the reconstruction would show a 1001 bid level
        // that getSnapshot() does not report.
        f.engine.submitOrder(1, 101, 2, Side::Buy, 1001, 80, OrderType::Hidden);

        CHECK(f.pub->addsEmitted() == addsAfterVisible);  // no 'A' for it
        CHECK(!f.reco.knows(101));
        f.check();

        // It still trades — the fill must not surface it either. A subscriber
        // that never saw 'A' must not receive 'E' or 'D' for that id.
        f.engine.submitOrder(1, 200, 3, Side::Sell, 1000, 30, OrderType::Limit);
        CHECK(!f.reco.knows(101));
        f.check();

        // And cancelling it stays invisible.
        f.engine.cancelOrder(1, 101);
        CHECK(!f.reco.knows(101));
        f.check();
    } END
}

void test_IcebergPublishesSliceAndSurvivesRefresh() {
    TEST(IcebergPublishesSliceAndSurvivesRefresh) {
        Fixture f;
        // 500 total, 100 displayed.
        f.engine.submitOrder(1, 100, 1, Side::Sell, 1000, 500, OrderType::Iceberg,
                             /*stopPrice=*/0, /*displayQty=*/100);
        CHECK(f.pub->addsEmitted() == 1);
        // The wire must show the slice, never the reserve.
        CHECK(f.reco.depth(Side::Sell).at(1000) == 100);
        f.check();

        // Consume part of the slice — no refresh yet.
        f.engine.submitOrder(1, 200, 2, Side::Buy, 1000, 40, OrderType::Limit);
        f.check();

        // Exhaust the slice, forcing a refresh. The refreshed slice is a new
        // arrival at the back of the queue: 'D' then 'A'.
        const uint64_t deletesBefore = f.pub->deletesEmitted();
        f.engine.submitOrder(1, 201, 2, Side::Buy, 1000, 60, OrderType::Limit);
        CHECK(f.pub->deletesEmitted() > deletesBefore);
        CHECK(f.pub->addsEmitted() == 2);
        f.check();

        // Drain the rest of the reserve through several more refreshes.
        for (int i = 0; i < 4; ++i) {
            f.engine.submitOrder(1, 300 + i, 3, Side::Buy, 1000, 100, OrderType::Limit);
            f.check();
        }
    } END
}

void test_ModifyDownEmitsCancelMessage() {
    TEST(ModifyDownEmitsCancelMessage) {
        Fixture f;
        f.engine.submitOrder(1, 100, 1, Side::Buy, 1000, 90, OrderType::Limit);
        CHECK(f.reco.depth(Side::Buy).at(1000) == 90);

        const uint64_t cancelsBefore = f.pub->cancelsEmitted();
        CHECK(f.engine.modifyOrder(1, 100, 35));

        // 'X' carries the shares REMOVED (55), not the new resting size.
        CHECK(f.pub->cancelsEmitted() == cancelsBefore + 1);
        CHECK(f.reco.depth(Side::Buy).at(1000) == 35);
        f.check();

        // A second reduction stacks correctly rather than resetting.
        CHECK(f.engine.modifyOrder(1, 100, 10));
        CHECK(f.reco.depth(Side::Buy).at(1000) == 10);
        f.check();

        // Reducing a HIDDEN order publishes nothing — there is no displayed
        // size to shrink.
        f.engine.submitOrder(1, 101, 2, Side::Buy, 1000, 70, OrderType::Hidden);
        const uint64_t cancelsAfterHiddenAdd = f.pub->cancelsEmitted();
        CHECK(f.engine.modifyOrder(1, 101, 20));
        CHECK(f.pub->cancelsEmitted() == cancelsAfterHiddenAdd);
        f.check();
    } END
}

void test_CancelReplaceReconstructs() {
    TEST(CancelReplaceReconstructs) {
        Fixture f;
        f.engine.submitOrder(1, 100, 1, Side::Buy, 1000, 90, OrderType::Limit);
        CHECK(f.reco.depth(Side::Buy).at(1000) == 90);

        // (a) Same price, quantity DOWN. The order keeps its place in the
        // queue and simply shrinks — the same shape as a modify-down, and it
        // must reach the feed as one.
        CHECK(f.engine.cancelReplace(1, 100, 1000, 40));
        CHECK(f.reco.depth(Side::Buy).at(1000) == 40);
        f.check();

        // (b) Same price, quantity UP. This forfeits time priority, so the
        // wire form is delete-then-add rather than an in-place grow.
        CHECK(f.engine.cancelReplace(1, 100, 1000, 75));
        CHECK(f.reco.depth(Side::Buy).at(1000) == 75);
        f.check();

        // (c) Price change with no crossing liquidity: the order moves.
        CHECK(f.engine.cancelReplace(1, 100, 998, 75));
        CHECK(f.reco.depth(Side::Buy).count(1000) == 0);
        CHECK(f.reco.depth(Side::Buy).at(998) == 75);
        f.check();

        // (d) Price change INTO crossing liquidity, partially filling. The
        // replaced order matches before it rests, so the feed has to get both
        // the executions and the final resting size right.
        f.engine.submitOrder(1, 200, 2, Side::Sell, 1002, 30, OrderType::Limit);
        f.check();
        CHECK(f.engine.cancelReplace(1, 100, 1002, 75));
        f.check();

        // (e) Price change that fully consumes the replaced order.
        f.engine.submitOrder(1, 300, 3, Side::Buy, 1010, 200, OrderType::Limit);
        f.engine.submitOrder(1, 301, 4, Side::Sell, 1020, 50, OrderType::Limit);
        f.check();
        CHECK(f.engine.cancelReplace(1, 301, 1005, 50));
        f.check();

        // (e2) A replaced order that crosses is acting as an AGGRESSOR: it is
        // off the book for the duration of the match. Its fills must therefore
        // be reported the way any aggressor's are — through the resting
        // maker's 'E' only. Emitting a second 'E' against the replaced order
        // would report the same trade twice, and would do so using its stale
        // pre-replace size, understating the quantity.
        {
            Fixture g;
            g.engine.submitOrder(1, 600, 1, Side::Sell, 1002, 200, OrderType::Limit);
            g.engine.submitOrder(1, 601, 2, Side::Buy,  1000,  50, OrderType::Limit);
            g.check();

            const uint64_t execsBefore = g.pub->executedEmitted();
            CHECK(g.engine.cancelReplace(1, 601, 1002, 200));
            g.check();

            CHECK(g.pub->executedEmitted() == execsBefore + 1 &&
                  "a crossing replace must report the trade once, via the maker");
        }

        // (f) Replacing an ICEBERG must keep publishing the slice, never the
        // reserve, and must not leave visibleQty above remainingQty.
        f.engine.submitOrder(1, 400, 5, Side::Sell, 1030, 500, OrderType::Iceberg,
                             /*stopPrice=*/0, /*displayQty=*/100);
        CHECK(f.reco.depth(Side::Sell).at(1030) == 100);
        f.check();
        CHECK(f.engine.cancelReplace(1, 400, 1030, 60));
        f.check();
        CHECK(f.engine.cancelReplace(1, 400, 1031, 200));
        f.check();

        // (g) Replacing a HIDDEN order stays entirely off the feed.
        f.engine.submitOrder(1, 500, 6, Side::Buy, 995, 80, OrderType::Hidden);
        CHECK(!f.reco.knows(500));
        CHECK(f.engine.cancelReplace(1, 500, 996, 40));
        CHECK(!f.reco.knows(500));
        f.check();
    } END
}

void test_UntriggeredStopIsInvisible() {
    TEST(UntriggeredStopIsInvisible) {
        Fixture f;
        f.engine.submitOrder(1, 100, 1, Side::Sell, 1005, 100, OrderType::Limit);
        const uint64_t addsAfterVisible = f.pub->addsEmitted();

        // A resting stop lives in stopOrders_, not in bids_/asks_. It is not
        // displayed liquidity until it triggers, so it must appear on neither
        // the feed nor the snapshot — the two must agree that it is absent.
        f.engine.submitOrder(1, 101, 2, Side::Buy, 990, 50, OrderType::Stop,
                             /*stopPrice=*/1005);
        CHECK(f.pub->addsEmitted() == addsAfterVisible);
        CHECK(!f.reco.knows(101));
        f.check();

        // Cancelling an untriggered stop is equally invisible.
        f.engine.cancelOrder(1, 101);
        CHECK(!f.reco.knows(101));
        f.check();
    } END
}

void test_CancellingParkedOrderLeavesItsPriceLevelIntact() {
    TEST(CancellingParkedOrderLeavesItsPriceLevelIntact) {
        // A parked order carries a price but was never linked into the book,
        // so its next/prev are null. Removing it from the intrusive list at
        // that price used to null out BOTH head and tail — silently orphaning
        // every genuine order resting there. They stayed allocated and
        // reachable via getOrder(), but vanished from the book with no fill,
        // no cancel and no notification to their owner.
        Fixture f;

        // Three real orders resting at 1001, plus a parked stop whose limit
        // price collides with them. The collision is ordinary flow.
        f.engine.submitOrder(1, 100, 1, Side::Sell, 1001, 40, OrderType::Limit);
        f.engine.submitOrder(1, 101, 2, Side::Sell, 1001, 60, OrderType::Limit);
        f.engine.submitOrder(1, 102, 3, Side::Sell, 1002, 25, OrderType::Limit);
        f.engine.submitOrder(1, 200, 4, Side::Sell, 1001, 70, OrderType::StopLimit,
                             /*stopPrice=*/990, /*displayQty=*/0,
                             TimeInForce::GTC, /*expiryTime=*/0,
                             /*stopLimitPrice=*/1001);
        CHECK(f.reco.depth(Side::Sell).at(1001) == 100);
        f.check();

        // Cancelling the parked stop must not disturb the level.
        f.engine.cancelOrder(1, 200);
        CHECK(f.reco.depth(Side::Sell).at(1001) == 100);
        f.check();

        // The survivors must still be tradable — the orphaning failure mode
        // left them present in lookups but permanently unmatchable, which a
        // depth check alone would not catch.
        f.engine.submitOrder(1, 300, 5, Side::Buy, 1001, 100, OrderType::Limit);
        CHECK(f.reco.depth(Side::Sell).count(1001) == 0 &&
              "both resting orders at 1001 must have filled");
        f.check();

        // And the untouched neighbouring level is unaffected.
        CHECK(f.reco.depth(Side::Sell).at(1002) == 25);
    } END
}

void test_TriggeredStopEntersFeedOnRest() {
    TEST(TriggeredStopEntersFeedOnRest) {
        Fixture f;
        // Resting ask at 1005 to trade against.
        f.engine.submitOrder(1, 100, 1, Side::Sell, 1005, 100, OrderType::Limit);

        // Buy stop: triggers at 1005, then becomes a limit at 990 — below the
        // best ask, so it rests rather than filling. That rest is the moment
        // it becomes displayed liquidity and must hit the feed.
        f.engine.submitOrder(1, 101, 2, Side::Buy, 990, 50, OrderType::Stop,
                             /*stopPrice=*/1005);
        const uint64_t addsBeforeTrigger = f.pub->addsEmitted();
        CHECK(!f.reco.knows(101));

        // Trade at 1005 triggers it.
        f.engine.submitOrder(1, 102, 3, Side::Buy, 1005, 30, OrderType::Limit);

        CHECK(f.pub->addsEmitted() == addsBeforeTrigger + 1);
        CHECK(f.reco.knows(101));
        CHECK(f.reco.depth(Side::Buy).at(990) == 50);
        f.check();

        // A triggered MIT becomes a market order and is never allowed to
        // rest, so it must never reach the feed at all.
        const uint64_t addsBeforeMit = f.pub->addsEmitted();
        f.engine.submitOrder(1, 103, 4, Side::Sell, 0, 20, OrderType::MIT,
                             /*stopPrice=*/1005);
        f.engine.submitOrder(1, 104, 5, Side::Buy, 1005, 10, OrderType::Limit);
        CHECK(!f.reco.knows(103));
        CHECK(f.pub->addsEmitted() == addsBeforeMit);
        f.check();
    } END
}

void test_AuctionUncrossReconstructs() {
    TEST(AuctionUncrossReconstructs) {
        Fixture f;
        f.book->setTradingState(TradingState::PreOpen);

        // Auction accumulation: orders rest without matching, so every one of
        // them is displayed and must appear on the feed.
        f.engine.submitOrder(1, 100, 1, Side::Buy,  1000, 100, OrderType::Limit);
        f.engine.submitOrder(1, 101, 2, Side::Buy,   999,  60, OrderType::Limit);
        f.engine.submitOrder(1, 200, 3, Side::Sell, 1000,  80, OrderType::Limit);
        f.engine.submitOrder(1, 201, 4, Side::Sell, 1001,  40, OrderType::Limit);
        CHECK(f.pub->addsEmitted() == 4);
        f.check();

        // A market order in an auction is PARKED, not booked — it has no
        // price to rest at until the uncross discovers one. It must be
        // invisible on both feed and snapshot while parked.
        f.engine.submitOrder(1, 202, 5, Side::Sell, 0, 25, OrderType::Market);
        CHECK(!f.reco.knows(202));
        f.check();

        // Uncross: parked markets are inserted at the clearing price, trades
        // execute, filled orders leave the book. The feed must land on the
        // same book the engine ends with.
        f.book->uncross();
        f.check();

        // An ICEBERG in an auction. The uncross fills against the order's full
        // remaining size, not its slice, so the displayed quantity has to be
        // re-derived afterwards — otherwise the snapshot keeps reporting a
        // slice larger than what is left.
        {
            Fixture g;
            g.book->setTradingState(TradingState::PreOpen);
            g.engine.submitOrder(1, 400, 1, Side::Sell, 1000, 500, OrderType::Iceberg,
                                 /*stopPrice=*/0, /*displayQty=*/100);
            g.engine.submitOrder(1, 401, 2, Side::Buy, 1000, 200, OrderType::Limit);
            CHECK(g.reco.depth(Side::Sell).at(1000) == 100);
            g.check();

            // Uncross fills 200 of the iceberg — twice its displayed slice.
            g.book->uncross();
            g.check();

            g.book->setTradingState(TradingState::Continuous);
            g.engine.submitOrder(1, 402, 3, Side::Buy, 1000, 50, OrderType::Limit);
            g.check();
        }

        // And the post-auction book stays consistent under continuous trading.
        f.book->setTradingState(TradingState::Continuous);
        f.engine.submitOrder(1, 300, 6, Side::Buy, 1001, 30, OrderType::Limit);
        f.check();
        f.engine.cancelOrder(1, 101);
        f.check();
    } END
}

void test_RandomisedFlowStaysInSync() {
    TEST(RandomisedFlowStaysInSync) {
        Fixture f;
        // Fixed seed: this must be reproducible when it fails.
        std::mt19937 rng(20260813u);
        std::uniform_int_distribution<int> actionDist(0, 16);
        std::uniform_int_distribution<int> priceDist(995, 1005);
        std::uniform_int_distribution<int> qtyDist(10, 120);

        std::vector<OrderId> resting;
        OrderId nextId = 1000;
        bool inAuction = false;

        // Coverage tracking. A soak that passes because it never reached the
        // paths it claims to cover is worse than no test — it reads as
        // evidence while asserting nothing. These are checked at the end.
        std::vector<OrderId> stopIds;
        std::set<OrderId> stopsThatReachedTheFeed;
        // MOC/LOC rest nowhere until the closing cross releases them, so one
        // of them reaching the feed is proof releaseOnCloseOrders() ran.
        std::vector<OrderId> onCloseIds;
        std::set<OrderId> onCloseThatReachedTheFeed;
        std::vector<OrderId> peggedIds;
        std::set<OrderId> peggedThatReachedTheFeed;
        int auctionCycles = 0;
        int replaces = 0;

        for (int step = 0; step < 600; ++step) {
            const int action = actionDist(rng);
            const auto price = static_cast<Price>(priceDist(rng));
            const auto qty   = static_cast<Quantity>(qtyDist(rng));
            const Side side  = (step % 2 == 0) ? Side::Buy : Side::Sell;
            const OrderId id = nextId++;

            // Periodically cycle through an auction. Orders accumulate without
            // matching, parked market orders sit outside the book entirely,
            // then the uncross inserts and executes them in one burst — a very
            // different event shape from continuous trading, over a book that
            // already holds icebergs, hidden orders and triggered stops.
            if (step % 97 == 0 && step > 0) {
                // Alternate opening and closing auctions. Only AuctionClose
                // runs releaseOnCloseOrders(), which parks MOC into the
                // auction-market list and moves LOC into the book — an add
                // path no other action reaches.
                f.book->setTradingState((auctionCycles % 2 == 0)
                                            ? TradingState::AuctionClose
                                            : TradingState::PreOpen);
                inAuction = true;
            } else if (inAuction && step % 97 == 11) {
                f.engine.submitOrder(1, nextId++, 9, side, 0, qty, OrderType::Market);
                f.check();
                f.book->uncross();
                f.book->setTradingState(TradingState::Continuous);
                inAuction = false;
                ++auctionCycles;
                f.check();
            }

            if (action <= 3) {
                f.engine.submitOrder(1, id, 1, side, price, qty, OrderType::Limit);
                resting.push_back(id);
            } else if (action <= 5) {
                f.engine.submitOrder(1, id, 2, side, price, qty, OrderType::Iceberg,
                                     0, /*displayQty=*/qty / 4 + 1);
                resting.push_back(id);
            } else if (action == 6) {
                f.engine.submitOrder(1, id, 3, side, price, qty, OrderType::Hidden);
                resting.push_back(id);
            } else if (action == 7) {
                // Stop: invisible until the market trades through stopPrice,
                // then rests as a limit (and shows up) or fills outright.
                const auto stopPx = static_cast<Price>(priceDist(rng));
                f.engine.submitOrder(1, id, 4, side, price, qty, OrderType::Stop,
                                     /*stopPrice=*/stopPx);
                resting.push_back(id);
                stopIds.push_back(id);
            } else if (action == 8) {
                // StopLimit: triggers to a limit at stopLimitPrice, which may
                // differ from the trigger level.
                const auto stopPx = static_cast<Price>(priceDist(rng));
                f.engine.submitOrder(1, id, 5, side, price, qty, OrderType::StopLimit,
                                     /*stopPrice=*/stopPx, /*displayQty=*/0,
                                     TimeInForce::GTC, /*expiryTime=*/0,
                                     /*stopLimitPrice=*/price);
                resting.push_back(id);
                stopIds.push_back(id);
            } else if (action == 13) {
                // Pegged: repriced in place after every trade, which is the
                // "price field vs. book linkage" hazard that produced the
                // parked-order corruption.
                f.engine.submitOrder(1, id, 6, side, price, qty, OrderType::Pegged,
                                     /*stopPrice=*/0, /*displayQty=*/0,
                                     TimeInForce::GTC, /*expiryTime=*/0,
                                     /*stopLimitPrice=*/0,
                                     (step % 3 == 0) ? PegType::MidPeg : PegType::PrimaryPeg,
                                     /*pegOffset=*/0);
                resting.push_back(id);
                peggedIds.push_back(id);
            } else if (action == 14) {
                // TrailingStop: parked, and mutated on every trade.
                f.engine.submitOrder(1, id, 7, side, price, qty, OrderType::TrailingStop,
                                     /*stopPrice=*/static_cast<Price>(priceDist(rng)),
                                     /*displayQty=*/0, TimeInForce::GTC,
                                     /*expiryTime=*/0, /*stopLimitPrice=*/0,
                                     PegType::None, /*pegOffset=*/0,
                                     /*trailAmount=*/2);
                resting.push_back(id);
            } else if (action == 15) {
                // MOC: parked until the closing cross releases it.
                f.engine.submitOrder(1, id, 8, side, price, qty, OrderType::MOC);
                resting.push_back(id);
                onCloseIds.push_back(id);
            } else if (action == 16) {
                // LOC: parked, then moved INTO the book at the close — an add
                // path nothing else in this soak exercises.
                f.engine.submitOrder(1, id, 8, side, price, qty, OrderType::LOC);
                resting.push_back(id);
                onCloseIds.push_back(id);
            } else if (action == 9 && !resting.empty()) {
                const size_t idx = rng() % resting.size();
                f.engine.modifyOrder(1, resting[idx], qty / 2 + 1);
            } else if (action == 10 && !resting.empty()) {
                // Reprice and resize. Hits shrink-in-place, grow-with-loss-of-
                // priority, and the crossing-aggressor path, over a book that
                // already holds icebergs, hidden orders and triggered stops.
                const size_t idx = rng() % resting.size();
                if (f.engine.cancelReplace(1, resting[idx], price, qty)) ++replaces;
            } else if (!resting.empty()) {
                const size_t idx = rng() % resting.size();
                f.engine.cancelOrder(1, resting[idx]);
                resting.erase(resting.begin() + static_cast<long>(idx));
            }

            // A stop that has reached the feed is one that triggered and
            // rested — the transition from invisible to displayed.
            for (OrderId s : stopIds) {
                if (f.reco.knows(s)) stopsThatReachedTheFeed.insert(s);
            }
            for (OrderId s : onCloseIds) {
                if (f.reco.knows(s)) onCloseThatReachedTheFeed.insert(s);
            }
            for (OrderId s : peggedIds) {
                if (f.reco.knows(s)) peggedThatReachedTheFeed.insert(s);
            }

            // The invariant holds after EVERY event, not just at the end —
            // an end-only check lets compensating errors cancel out.
            try {
                f.check();
            } catch (const std::exception& e) {
                throw std::runtime_error("step " + std::to_string(step) +
                                         " (action " + std::to_string(action) +
                                         ", id " + std::to_string(id) + "): " + e.what());
            }
        }

        // Coverage gates. Without these the soak could stop reaching stops or
        // auctions entirely — through a distribution change, a price-range
        // change, or an engine change that stops triggering — and still report
        // success, which would be a false all-clear on exactly the paths this
        // test was extended to cover.
        CHECK(auctionCycles >= 3);
        CHECK(!stopIds.empty());
        CHECK(!stopsThatReachedTheFeed.empty() &&
              "no stop ever triggered and rested — the stop path went unexercised");
        CHECK(replaces >= 5 && "cancelReplace path went unexercised");
        CHECK(!peggedThatReachedTheFeed.empty() &&
              "no pegged order ever rested — the peg reprice path went unexercised");
        CHECK(!onCloseThatReachedTheFeed.empty() &&
              "no MOC/LOC ever reached the book — releaseOnCloseOrders went unexercised");
        std::cout << "[" << auctionCycles << " auctions, "
                  << stopsThatReachedTheFeed.size() << "/" << stopIds.size()
                  << " stops triggered onto the feed, "
                  << replaces << " replaces, "
                  << peggedThatReachedTheFeed.size() << " pegged, "
                  << onCloseThatReachedTheFeed.size() << "/" << onCloseIds.size()
                  << " on-close released] ";
    } END
}

int main() {
    std::cout << "\nITCH reconstruction tests\n";

    test_PlainLimitBookReconstructs();
    test_HiddenOrderNeverReachesFeed();
    test_IcebergPublishesSliceAndSurvivesRefresh();
    test_ModifyDownEmitsCancelMessage();
    test_CancelReplaceReconstructs();
    test_UntriggeredStopIsInvisible();
    test_CancellingParkedOrderLeavesItsPriceLevelIntact();
    test_TriggeredStopEntersFeedOnRest();
    test_AuctionUncrossReconstructs();
    test_RandomisedFlowStaysInSync();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
