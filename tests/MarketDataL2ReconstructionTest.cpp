// MarketDataL2ReconstructionTest — feed-fidelity test for the price-level
// (L2/MBP) incremental feed, the counterpart to ItchReconstructionTest.
//
// ItchReconstructionTest replays the order-level stream and holds it against
// getSnapshot(). That cannot catch anything wrong with the L2 INCREMENTAL
// stream, because getSnapshot() reads the book directly and is therefore
// always right — the incremental messages are never consulted. This file
// closes that blind spot: it applies only onMarketData updates and asserts the
// resulting depth equals the engine's.
//
// notifyMarketData recomputes the whole level from the book on every call, so
// each message carries the ABSOLUTE state of one price level rather than a
// delta. That makes the stream self-correcting: any message about a level
// resets a subscriber to truth. The single way it can drift is a level that
// changes while NO message is emitted — so that is what this file hunts.
//
// Coverage:
//   1. Plain flow: adds, partial and full fills, cancels
//   2. Hidden orders contribute nothing; icebergs contribute only their slice
//   3. Pegged orders republish when the peg moves them to a new price
//   4. Randomised soak: reconstructed depth must equal the snapshot at every
//      step, with sequence numbers strictly increasing throughout

#include "MatchingEngine.h"
#include "OrderBook.h"

#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
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

namespace {

// ─── Subscriber-side depth reconstruction ───────────────────────────────────
//
// Holds only what the incremental stream says. It never reads the book, which
// is the entire point: if the engine changes a level without publishing, this
// view goes stale and the comparison fails.

class L2Reconstructor : public EventListener {
public:
    void onMarketData(const MarketDataUpdate& u) override {
        ++messages_;
        // Sequence numbers must strictly increase — a subscriber has nothing
        // else to detect loss or reordering with. Recorded rather than thrown:
        // this runs inline on the matching thread, and unwinding through the
        // engine would obscure the failure it is trying to report.
        if (sawSequence_ && u.sequenceNumber <= lastSequence_) ++sequenceViolations_;
        lastSequence_ = u.sequenceNumber;
        sawSequence_ = true;

        auto& book = (u.side == Side::Buy) ? bids_ : asks_;
        // A level whose displayed quantity reaches zero is gone. orderCount can
        // still be non-zero there (an iceberg resting with an exhausted slice),
        // which is exactly how getSnapshot reports it, so match on quantity.
        if (u.level.totalQuantity == 0) book.erase(u.level.price);
        else book[u.level.price] = u.level.totalQuantity;
    }

    const std::map<Price, Quantity>& depth(Side side) const {
        return (side == Side::Buy) ? bids_ : asks_;
    }
    uint64_t messages()           const { return messages_; }
    uint64_t sequenceViolations() const { return sequenceViolations_; }

private:
    std::map<Price, Quantity> bids_;
    std::map<Price, Quantity> asks_;
    uint64_t lastSequence_{0};
    bool     sawSequence_{false};
    uint64_t messages_{0};
    uint64_t sequenceViolations_{0};
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
    for (const auto& entry : levels) {
        s += " " + std::to_string(entry.first) + ":" + std::to_string(entry.second);
    }
    return s + " }";
}

void assertSideMatches(const L2Reconstructor& reco, const OrderBook& book,
                       Side side, const char* label) {
    const auto& replayed = reco.depth(side);
    const auto actual = snapshotDepth(book, side);
    if (replayed == actual) return;
    throw std::runtime_error(std::string("depth mismatch on ") + label +
                             "\n      replayed from incrementals: " + describe(replayed) +
                             "\n      engine snapshot:            " + describe(actual));
}

struct Fixture {
    MatchingEngine  engine;
    OrderBook*      book = nullptr;
    L2Reconstructor reco;

    explicit Fixture(SymbolId symbol = 1) {
        engine.addSymbol(symbol);
        engine.start();
        book = engine.getOrderBook(symbol);
        if (!book) throw std::runtime_error("no book for symbol");
        book->setEventListener(&reco);
    }

    void check() const {
        assertSideMatches(reco, *book, Side::Buy, "bids");
        assertSideMatches(reco, *book, Side::Sell, "asks");
    }
};

}  // namespace

// ─── Tests ──────────────────────────────────────────────────────────────────

void test_PlainFlowReconstructs() {
    TEST(PlainFlowReconstructs) {
        Fixture f;
        f.engine.submitOrder(1, 100, 1, Side::Buy,  1000, 50, OrderType::Limit);
        f.engine.submitOrder(1, 101, 2, Side::Buy,  1000, 30, OrderType::Limit);
        f.engine.submitOrder(1, 102, 1, Side::Buy,   999, 70, OrderType::Limit);
        f.engine.submitOrder(1, 200, 3, Side::Sell, 1001, 40, OrderType::Limit);
        f.check();
        CHECK(f.reco.messages() > 0);

        f.engine.submitOrder(1, 300, 4, Side::Buy, 1001, 15, OrderType::Limit);  // partial
        f.check();
        f.engine.submitOrder(1, 301, 4, Side::Buy, 1001, 25, OrderType::Limit);  // full
        f.check();
        f.engine.cancelOrder(1, 102);
        f.check();
        f.engine.submitOrder(1, 302, 5, Side::Sell, 999, 100, OrderType::Limit); // sweep
        f.check();
        CHECK(f.reco.sequenceViolations() == 0);
    } END
}

void test_HiddenAndIcebergDisplayCorrectly() {
    TEST(HiddenAndIcebergDisplayCorrectly) {
        Fixture f;
        f.engine.submitOrder(1, 100, 1, Side::Buy, 1000, 50, OrderType::Limit);
        // Hidden contributes nothing to the displayed level.
        f.engine.submitOrder(1, 101, 2, Side::Buy, 1000, 80, OrderType::Hidden);
        CHECK(f.reco.depth(Side::Buy).at(1000) == 50);
        f.check();

        // Iceberg contributes its slice, never its reserve.
        f.engine.submitOrder(1, 200, 3, Side::Sell, 1002, 500, OrderType::Iceberg,
                             /*stopPrice=*/0, /*displayQty=*/100);
        CHECK(f.reco.depth(Side::Sell).at(1002) == 100);
        f.check();

        // Consume past the slice so it refreshes.
        f.engine.submitOrder(1, 300, 4, Side::Buy, 1002, 140, OrderType::Limit);
        f.check();

        f.engine.cancelOrder(1, 101);
        f.check();
    } END
}

void test_PeggedRepriceIsPublished() {
    TEST(PeggedRepriceIsPublished) {
        Fixture f;
        // Establish a market so the peg has a reference.
        f.engine.submitOrder(1, 100, 1, Side::Buy,   995, 40, OrderType::Limit);
        f.engine.submitOrder(1, 200, 2, Side::Sell, 1005, 40, OrderType::Limit);
        f.check();

        // Primary-pegged buy tracks the best bid, so it rests at 995.
        f.engine.submitOrder(1, 101, 3, Side::Buy, 995, 25, OrderType::Pegged,
                             /*stopPrice=*/0, /*displayQty=*/0,
                             TimeInForce::GTC, /*expiryTime=*/0,
                             /*stopLimitPrice=*/0, PegType::PrimaryPeg,
                             /*pegOffset=*/0);
        f.check();

        // Improve the bid. updatePeggedOrders runs on the NEXT submission,
        // repricing the peg from 995 to 998 — a change to two levels that the
        // incremental stream has to publish, or every subscriber keeps
        // showing depth at a price the order has left.
        f.engine.submitOrder(1, 102, 4, Side::Buy, 998, 60, OrderType::Limit);
        f.check();
        f.engine.submitOrder(1, 103, 5, Side::Buy, 990, 10, OrderType::Limit);
        f.check();

        CHECK(f.reco.sequenceViolations() == 0);
    } END
}

void test_ProRataFillsArePublished() {
    TEST(ProRataFillsArePublished) {
        Fixture f;
        f.book->setMatchAlgorithm(MatchAlgorithm::ProRata);

        // Several orders at one level so the allocation spreads across them.
        f.engine.submitOrder(1, 100, 1, Side::Sell, 1000, 100, OrderType::Limit);
        f.engine.submitOrder(1, 101, 2, Side::Sell, 1000, 200, OrderType::Limit);
        f.engine.submitOrder(1, 102, 3, Side::Sell, 1001,  50, OrderType::Limit);
        f.check();

        f.engine.submitOrder(1, 200, 4, Side::Buy, 1000, 120, OrderType::Limit);
        f.check();

        // Sweep through both levels.
        f.engine.submitOrder(1, 201, 5, Side::Buy, 1001, 300, OrderType::Limit);
        f.check();

        // And an iceberg under pro-rata, whose slice refreshes in place.
        f.engine.submitOrder(1, 300, 6, Side::Sell, 1002, 400, OrderType::Iceberg,
                             /*stopPrice=*/0, /*displayQty=*/80);
        f.check();
        f.engine.submitOrder(1, 301, 7, Side::Buy, 1002, 150, OrderType::Limit);
        f.check();
    } END
}

void test_AuctionUncrossIsPublished() {
    TEST(AuctionUncrossIsPublished) {
        Fixture f;
        f.book->setTradingState(TradingState::PreOpen);

        f.engine.submitOrder(1, 100, 1, Side::Buy,  1000, 100, OrderType::Limit);
        f.engine.submitOrder(1, 101, 2, Side::Buy,   999,  60, OrderType::Limit);
        f.engine.submitOrder(1, 200, 3, Side::Sell, 1000,  80, OrderType::Limit);
        f.engine.submitOrder(1, 201, 4, Side::Sell, 1001,  40, OrderType::Limit);
        f.check();

        // The uncross executes outside the normal match path entirely, so it
        // needs its own depth publishing.
        f.book->uncross();
        f.check();

        f.book->setTradingState(TradingState::Continuous);
        f.engine.submitOrder(1, 300, 5, Side::Buy, 1001, 30, OrderType::Limit);
        f.check();
    } END
}

void test_SoakStaysInSync() {
    TEST(SoakStaysInSync) {
        Fixture f;
        std::mt19937 rng(20260814u);
        std::uniform_int_distribution<int> actionDist(0, 9);
        std::uniform_int_distribution<int> priceDist(995, 1005);
        std::uniform_int_distribution<int> qtyDist(10, 120);

        std::vector<OrderId> resting;
        OrderId nextId = 1000;
        int pegged = 0;

        for (int step = 0; step < 400; ++step) {
            const int action = actionDist(rng);
            const auto price = static_cast<Price>(priceDist(rng));
            const auto qty   = static_cast<Quantity>(qtyDist(rng));
            const Side side  = (step % 2 == 0) ? Side::Buy : Side::Sell;
            const OrderId id = nextId++;

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
                f.engine.submitOrder(1, id, 4, side, price, qty, OrderType::Pegged,
                                     /*stopPrice=*/0, /*displayQty=*/0,
                                     TimeInForce::GTC, /*expiryTime=*/0,
                                     /*stopLimitPrice=*/0,
                                     (step % 3 == 0) ? PegType::MidPeg : PegType::PrimaryPeg,
                                     /*pegOffset=*/0);
                resting.push_back(id);
                ++pegged;
            } else if (action == 8 && !resting.empty()) {
                const size_t idx = rng() % resting.size();
                f.engine.modifyOrder(1, resting[idx], qty / 2 + 1);
            } else if (!resting.empty()) {
                const size_t idx = rng() % resting.size();
                f.engine.cancelOrder(1, resting[idx]);
                resting.erase(resting.begin() + static_cast<long>(idx));
            }

            try {
                f.check();
            } catch (const std::exception& e) {
                throw std::runtime_error("step " + std::to_string(step) +
                                         " (action " + std::to_string(action) +
                                         ", id " + std::to_string(id) + "): " + e.what());
            }
        }

        CHECK(f.reco.sequenceViolations() == 0 &&
              "market-data sequence numbers must strictly increase");
        CHECK(pegged >= 10 && "peg path went unexercised");
        std::cout << "[" << f.reco.messages() << " updates, "
                  << pegged << " pegs] ";
    } END
}

int main() {
    std::cout << "\nL2 incremental market-data reconstruction tests\n";

    test_PlainFlowReconstructs();
    test_HiddenAndIcebergDisplayCorrectly();
    test_PeggedRepriceIsPublished();
    test_ProRataFillsArePublished();
    test_AuctionUncrossIsPublished();
    test_SoakStaysInSync();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
