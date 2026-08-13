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
//   5. Randomised soak: mixed order types, replay must match at every step

#include "ItchProtocol.h"
#include "ItchPublisher.h"
#include "MatchingEngine.h"
#include "OrderBook.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <random>
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
    throw std::runtime_error(std::string("book mismatch on ") + label +
                             "\n      replayed from feed: " + describe(replayed) +
                             "\n      engine snapshot:    " + describe(actual));
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

void test_RandomisedFlowStaysInSync() {
    TEST(RandomisedFlowStaysInSync) {
        Fixture f;
        // Fixed seed: this must be reproducible when it fails.
        std::mt19937 rng(20260813u);
        std::uniform_int_distribution<int> actionDist(0, 9);
        std::uniform_int_distribution<int> priceDist(995, 1005);
        std::uniform_int_distribution<int> qtyDist(10, 120);

        std::vector<OrderId> resting;
        OrderId nextId = 1000;

        for (int step = 0; step < 400; ++step) {
            const int action = actionDist(rng);
            const auto price = static_cast<Price>(priceDist(rng));
            const auto qty   = static_cast<Quantity>(qtyDist(rng));
            const Side side  = (step % 2 == 0) ? Side::Buy : Side::Sell;
            const OrderId id = nextId++;

            if (action <= 4) {
                f.engine.submitOrder(1, id, 1, side, price, qty, OrderType::Limit);
                resting.push_back(id);
            } else if (action <= 6) {
                f.engine.submitOrder(1, id, 2, side, price, qty, OrderType::Iceberg,
                                     0, /*displayQty=*/qty / 4 + 1);
                resting.push_back(id);
            } else if (action == 7) {
                f.engine.submitOrder(1, id, 3, side, price, qty, OrderType::Hidden);
                resting.push_back(id);
            } else if (action == 8 && !resting.empty()) {
                const size_t idx = rng() % resting.size();
                f.engine.modifyOrder(1, resting[idx], qty / 2 + 1);
            } else if (!resting.empty()) {
                const size_t idx = rng() % resting.size();
                f.engine.cancelOrder(1, resting[idx]);
                resting.erase(resting.begin() + static_cast<long>(idx));
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
    } END
}

int main() {
    std::cout << "\nITCH reconstruction tests\n";

    test_PlainLimitBookReconstructs();
    test_HiddenOrderNeverReachesFeed();
    test_IcebergPublishesSliceAndSurvivesRefresh();
    test_ModifyDownEmitsCancelMessage();
    test_RandomisedFlowStaysInSync();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
