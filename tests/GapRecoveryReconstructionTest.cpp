// GapRecoveryReconstructionTest — does a subscriber that LOSES packets and
// recovers them still end up with the engine's book?
//
// ItchRetransmissionTest already proves the recovery machinery replays the
// right bytes. It does not prove those bytes reconstruct the right book, which
// is the property a subscriber actually depends on. This file joins the two:
// publish a real order flow over MoldUDP64, drop datagrams on the way to the
// subscriber, recover the holes from the packet journal, and assert the
// replayed book equals getSnapshot().
//
// The contract being tested is subtler than "recovery works". MoldUDP64
// Subscriber::deliverSequenced hands a message to the application AS SOON AS it
// arrives — including messages ahead of an unfilled hole — and separately
// tracks the contiguous front. That is a deliberate choice (a trade-tape
// consumer wants them immediately) but it means a BOOK-building consumer must
// buffer past the hole itself: applying an 'E' whose 'A' is still missing is
// nonsense. Nothing in the repo does that buffering today, so this file models
// the correct subscriber and pins the requirement.
//
// Coverage:
//   1. A correct (buffering) subscriber recovers exactly the engine's book
//   2. Multiple, non-adjacent holes across a long flow
//   3. Unrecovered loss is detected, never silently wrong — which is also what
//      proves the recovered case is doing real work
//   4. A hole at the very start, where the lost 'A' messages underpin
//      everything that follows

#include "ItchBookReplay.h"
#include "ItchPublisher.h"
#include "MatchingEngine.h"
#include "MoldPacketJournal.h"
#include "MoldUDP64.h"
#include "OrderBook.h"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace OrderMatcher;
using OrderMatcher::testing::ItchBookReplay;

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
    for (const auto& e : levels) s += " " + std::to_string(e.first) + ":" + std::to_string(e.second);
    return s + " }";
}

// ─── A correct book-building subscriber ─────────────────────────────────────
//
// Applies strictly in sequence order. Messages arriving ahead of a hole are
// held, not applied; the hole is filled from the journal, then the held run is
// drained. This is the piece a real consumer has to supply on top of
// MoldUDP64Subscriber.

class OrderedBookSubscriber {
public:
    OrderedBookSubscriber(MoldPacketJournal& journal, bool recover)
        : journal_(journal), recover_(recover) {}

    // Every message the transport delivers, in arrival order.
    void onMessage(uint64_t seq, const uint8_t* data, size_t len) {
        if (seq < nextToApply_) return;  // already applied via recovery
        pending_[seq].assign(data, data + len);
        drain();
    }

    // The transport reports a hole. Only RECORDED here: a real subscriber
    // re-requests over a separate SoupBinTCP connection, and recovery must not
    // re-enter feedPacket from inside feedPacket. Serviced afterwards.
    void onGap(uint64_t expectedSeq, uint64_t receivedSeq) {
        ++gapsSeen_;
        if (recover_) pendingGaps_.push_back({expectedSeq, receivedSeq});
    }

    // Replay each outstanding hole back through the SAME subscriber, rather
    // than applying it here directly.
    //
    // This is load-bearing, not cosmetic. MoldUDP64Subscriber closes a hole
    // only when the missing messages flow back through feedPacket and advance
    // its contiguous front. Applying them out of band leaves the front parked
    // at the first hole forever — and because a new gap is reported only when
    // `expected >= gapReportedTo_`, every LATER hole is then silently
    // suppressed. One unrecovered gap would mask all the rest.
    template <typename Republish>
    void serviceRecovery(Republish&& republish) {
        while (!pendingGaps_.empty()) {
            const auto gap = pendingGaps_.front();
            pendingGaps_.erase(pendingGaps_.begin());
            const auto count = static_cast<uint16_t>(gap.second - gap.first);
            recovered_ += journal_.replayRange(
                gap.first, count,
                [&](uint64_t seq, const uint8_t* d, size_t l) { republish(seq, d, l); });
        }
    }

    const ItchBookReplay& book() const { return replay_; }
    uint64_t gapsSeen()  const { return gapsSeen_; }
    size_t   recovered() const { return recovered_; }
    size_t   stillHeld() const { return pending_.size(); }
    bool     hasPendingGaps() const { return !pendingGaps_.empty(); }

private:
    // Apply the longest contiguous run starting at nextToApply_. Anything
    // beyond a hole stays put until the hole is filled.
    void drain() {
        auto it = pending_.find(nextToApply_);
        while (it != pending_.end()) {
            replay_.applyMessage(it->second.data());
            pending_.erase(it);
            ++nextToApply_;
            it = pending_.find(nextToApply_);
        }
    }

    MoldPacketJournal&                       journal_;
    bool                                     recover_;
    ItchBookReplay                           replay_;
    std::map<uint64_t, std::vector<uint8_t>> pending_;
    uint64_t                                 nextToApply_{1};
    uint64_t                                 gapsSeen_{0};
    size_t                                   recovered_{0};
    std::vector<std::pair<uint64_t, uint64_t>> pendingGaps_;
};

// ─── Publisher side with a lossy link ───────────────────────────────────────
//
// Engine -> ItchPublisher -> MoldUDP64Publisher -> [drop filter] -> Subscriber,
// with every message also written to the journal, exactly as ItchMarketDataFeed
// wires it in production.

struct LossyFeed {
    MatchingEngine                         engine;
    OrderBook*                             book = nullptr;
    MoldPacketJournal                      journal{4096};
    MoldUDP64Subscriber                    sub;
    std::unique_ptr<MoldUDP64Publisher>    pub;
    // Second publisher for retransmissions: same session, never dropped. Its
    // packets carry the ORIGINAL sequence numbers via setNextSequence, which
    // is what lets the subscriber close the hole rather than see a new one.
    std::unique_ptr<MoldUDP64Publisher>    recoveryPub;
    std::unique_ptr<OrderedBookSubscriber> app;
    std::unique_ptr<ItchPublisher>         itch;

    std::set<uint64_t> dropPackets;   // 0-based index of datagrams to discard
    uint64_t           packetIndex{0};
    uint64_t           droppedCount{0};

    explicit LossyFeed(bool recover) {
        engine.addSymbol(1);
        engine.start();
        book = engine.getOrderBook(1);
        if (!book) throw std::runtime_error("no book for symbol");

        app = std::make_unique<OrderedBookSubscriber>(journal, recover);
        sub.setExpectedSession("MDGAP");
        sub.setOnMessage([this](uint64_t seq, const uint8_t* d, size_t l) {
            app->onMessage(seq, d, l);
        });
        sub.setOnGapDetected([this](uint64_t expected, uint64_t received) {
            app->onGap(expected, received);
        });

        // Datagrams either reach the subscriber or are discarded outright,
        // which is what UDP loss looks like from the receiver.
        pub = std::make_unique<MoldUDP64Publisher>(
            "MDGAP", [this](std::string_view packet) {
                const uint64_t idx = packetIndex++;
                if (dropPackets.count(idx)) { ++droppedCount; return; }
                sub.feedPacket(reinterpret_cast<const uint8_t*>(packet.data()),
                               packet.size());
                serviceRecovery();
            });

        recoveryPub = std::make_unique<MoldUDP64Publisher>(
            "MDGAP", [this](std::string_view packet) {
                sub.feedPacket(reinterpret_cast<const uint8_t*>(packet.data()),
                               packet.size());
            });

        // Journal each message under the sequence the publisher assigns it, so
        // a recovery request and the live stream agree on numbering — the same
        // wiring ItchMarketDataFeed uses.
        itch = std::make_unique<ItchPublisher>(*book, [this](std::string_view bytes) {
            const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
            const auto len = static_cast<uint16_t>(bytes.size());
            const uint64_t seq = pub->addMessage(p, len);
            journal.record(seq, p, len);
            pub->flush();  // one message per datagram, so drops are precise
        });
        book->setEventListener(itch.get());
    }

    // Drain outstanding holes after the live datagram has been fully
    // processed, so recovery never re-enters feedPacket.
    void serviceRecovery() {
        if (inRecovery_ || !app->hasPendingGaps()) return;
        inRecovery_ = true;
        app->serviceRecovery([this](uint64_t seq, const uint8_t* d, size_t l) {
            recoveryPub->setNextSequence(seq);
            recoveryPub->addMessage(d, static_cast<uint16_t>(l));
            recoveryPub->flush();
        });
        inRecovery_ = false;
    }

    bool inRecovery_{false};

    void check() const {
        for (Side side : {Side::Buy, Side::Sell}) {
            const auto replayed = app->book().depth(side);
            const auto actual = snapshotDepth(*book, side);
            if (replayed == actual) continue;
            throw std::runtime_error(
                std::string("book mismatch on ") + (side == Side::Buy ? "bids" : "asks") +
                "\n      recovered from feed: " + describe(replayed) +
                "\n      engine snapshot:     " + describe(actual));
        }
    }
};

// A deterministic two-sided flow with fills and cancels, enough that a lost
// packet in the middle is load-bearing rather than incidental.
void driveFlow(LossyFeed& f, int orders) {
    std::mt19937 rng(20260814u);
    std::uniform_int_distribution<int> priceDist(995, 1005);
    std::uniform_int_distribution<int> qtyDist(10, 60);
    std::vector<OrderId> resting;
    OrderId id = 1000;

    for (int i = 0; i < orders; ++i) {
        const auto price = static_cast<Price>(priceDist(rng));
        const auto qty   = static_cast<Quantity>(qtyDist(rng));
        const Side side  = (i % 2 == 0) ? Side::Buy : Side::Sell;
        if (i % 7 == 6 && !resting.empty()) {
            const size_t idx = rng() % resting.size();
            f.engine.cancelOrder(1, resting[idx]);
            resting.erase(resting.begin() + static_cast<long>(idx));
        } else {
            f.engine.submitOrder(1, id, 1, side, price, qty, OrderType::Limit);
            resting.push_back(id);
            ++id;
        }
    }
}

}  // namespace

// ─── Tests ──────────────────────────────────────────────────────────────────

void test_NoLossReconstructsExactly() {
    TEST(NoLossReconstructsExactly) {
        LossyFeed f(/*recover=*/true);
        driveFlow(f, 120);
        CHECK(f.app->gapsSeen() == 0);
        CHECK(f.app->stillHeld() == 0);
        f.check();
    } END
}

void test_RecoveredLossReconstructsExactly() {
    TEST(RecoveredLossReconstructsExactly) {
        LossyFeed f(/*recover=*/true);
        // Non-adjacent holes spread through the flow, so recovery has to work
        // repeatedly rather than once at a convenient moment.
        f.dropPackets = {5, 6, 23, 47, 48, 49, 90};
        driveFlow(f, 120);

        std::cout << "[packets=" << f.packetIndex << " dropped=" << f.droppedCount << "] ";
        CHECK(f.app->gapsSeen() > 0 && "transport must report the holes");
        CHECK(f.app->recovered() > 0 && "journal must have supplied the missing messages");
        CHECK(f.app->stillHeld() == 0 && "every hole must end up filled");
        f.check();
    } END
}

void test_UnrecoveredLossIsDetectedNotSilent() {
    TEST(UnrecoveredLossIsDetectedNotSilent) {
        // The same loss, with recovery disabled. The point is not that the
        // book is wrong — of course it is — but that the subscriber KNOWS.
        // Silent divergence is the failure mode that matters; a reported gap
        // is recoverable operationally. This also proves the recovered case
        // above is doing real work rather than passing trivially.
        LossyFeed f(/*recover=*/false);
        f.dropPackets = {5, 6, 23, 47, 48, 49, 90};
        driveFlow(f, 120);

        CHECK(f.app->gapsSeen() > 0 && "loss must be reported, never silent");
        CHECK(f.app->stillHeld() > 0 && "messages past the hole must remain unapplied");

        bool diverged = false;
        try {
            f.check();
        } catch (const std::exception&) {
            diverged = true;
        }
        CHECK(diverged && "without recovery the book must differ");
    } END
}

void test_HoleAtStreamStartRecovers() {
    TEST(HoleAtStreamStartRecovers) {
        // The very first datagrams carry 'A' messages everything later refers
        // to. Losing them is the worst case for an eager subscriber and the
        // clearest test of ordered application.
        LossyFeed f(/*recover=*/true);
        f.dropPackets = {0, 1, 2};
        driveFlow(f, 80);

        CHECK(f.droppedCount == 3);
        CHECK(f.app->recovered() >= 3);
        CHECK(f.app->stillHeld() == 0);
        f.check();
    } END
}

int main() {
    std::cout << "\nGap-recovery reconstruction tests\n";

    test_NoLossReconstructsExactly();
    test_RecoveredLossReconstructsExactly();
    test_UnrecoveredLossIsDetectedNotSilent();
    test_HoleAtStreamStartRecovers();

    std::cout << "\n" << tests_passed << " passed, "
              << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
