// CheckpointTradingStateTest — a checkpoint brings back every resting and
// parked order whatever trading state the book was in, and the book then goes
// through its next transition as it would have without the restart.
//
// A checkpoint writes a book's trading state before its orders (AUCT-1), and
// each order came back through addOrder, which judged it as a new arrival:
// under Halted every re-add was refused MarketHalted and under PostClose
// MarketClosed, silently in replay mode, so a halted or closed book restored
// EMPTY. gracefulShutdown checkpoints, so a SIGTERM of a closed book lost
// every GTC order. Arrival rules refused others in any state: a trailing stop
// entered without a price, judged against a last trade the checkpoint puts
// back only after the orders, and a post-only offer an auction's call period
// crossed, judged against a bid that arrived after it.
//
// Each scenario builds one book with every order kind it can hold — limit,
// iceberg, GTD, pegged, stop, stop-limit, trailing stop, on-close (MOC/LOC),
// hidden, post-only, minQty, and in an auction crossing limits and parked
// market orders — and takes it into one state. That book is then read as it
// is, after a restart from the plain journal, after a restart from the
// checkpoint gracefulShutdown takes, and after two of those in a row. Read:
// state, pre-halt state, session date, last trade and reference price, and
// every order in book and parked-list order with its price, leaves, slice,
// trigger and whether it rests. Then each book takes the next transitions to
// continuous trading (a halt resumes, an auction crosses, a closed book opens
// the next session), two orders sweep it and it runs a closing auction, and
// it is read again.

#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 0;
constexpr Price kPx = 10000;
constexpr uint32_t kDate = 20000;  // days since 1970

enum class Life { Uninterrupted, Replayed, Checkpointed, CheckpointedTwice };
const char* name(Life l) {
    switch (l) {
    case Life::Uninterrupted:     return "uninterrupted";
    case Life::Replayed:          return "replayed";
    case Life::Checkpointed:      return "checkpointed";
    case Life::CheckpointedTwice: return "checkpointed x2";
    }
    return "";
}

const char* name(TradingState s) {
    switch (s) {
    case TradingState::Continuous:        return "Continuous";
    case TradingState::Halted:            return "Halted";
    case TradingState::AuctionOpen:       return "AuctionOpen";
    case TradingState::AuctionClose:      return "AuctionClose";
    case TradingState::PreOpen:           return "PreOpen";
    case TradingState::PostClose:         return "PostClose";
    case TradingState::VolatilityAuction: return "VolatilityAuction";
    }
    return "?";
}

const char* code(OrderType t) {
    switch (t) {
    case OrderType::Limit:        return "L";
    case OrderType::Market:       return "M";
    case OrderType::IOC:          return "IOC";
    case OrderType::FOK:          return "FOK";
    case OrderType::Stop:         return "S";
    case OrderType::StopLimit:    return "SL";
    case OrderType::Iceberg:      return "I";
    case OrderType::PostOnly:     return "PO";
    case OrderType::Hidden:       return "H";
    case OrderType::Pegged:       return "P";
    case OrderType::TrailingStop: return "T";
    case OrderType::MOC:          return "MOC";
    case OrderType::LOC:          return "LOC";
    case OrderType::MIT:          return "MIT";
    }
    return "?";
}

void boot(MatchingEngine& engine, const std::string& path) {
    engine.addSymbol(kSym);
    assert(engine.enableJournal(path, Journal::SyncPolicy::Immediate));
    engine.start();
}

struct Spec {
    OrderId id; Side side; Price price; Quantity qty; OrderType type;
    Price stop = 0; Quantity display = 0; TimeInForce tif = TimeInForce::GTC;
    uint64_t expiry = 0; Price stopLimit = 0; PegType peg = PegType::None;
    Price offset = 0; Price trail = 0; Quantity minQty = 0;
};
void add(MatchingEngine& e, const Spec& s) {
    const SubmitResult r = e.submitOrder(kSym, s.id, s.id, s.side, s.price, s.qty, s.type,
                                         s.stop, s.display, s.tif, s.expiry, s.stopLimit,
                                         s.peg, s.offset, s.trail, s.minQty);
    if (!r.isAccepted())
        std::printf("    setup: order %llu refused (reason %d)\n",
                    static_cast<unsigned long long>(s.id), static_cast<int>(r.rejectReason));
    assert(r.isAccepted());
}
void to(MatchingEngine& e, TradingState s) {
    assert(e.setTradingStateBatch({kSym}, s) == 1);
}

uint64_t inAnHour() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>((system_clock::now() + hours(1)).time_since_epoch()).count());
}

// A trade at 100.00 (last trade and reference), then one order of each kind
// continuous trading admits. The peg rests at the bid behind 3 and 5; the
// stops and the trailing stop (100.00 - 0.30) fire on a fall to 99.60.
void seed(MatchingEngine& e) {
    using S = Side; using T = OrderType;
    add(e, {1, S::Buy, kPx, 10, T::Limit});
    add(e, {2, S::Sell, kPx, 10, T::Limit});
    add(e, {3, S::Buy, kPx - 10, 10, T::Limit});
    add(e, {4, S::Sell, kPx + 10, 10, T::Limit});
    add(e, {5, S::Buy, kPx - 10, 30, T::Iceberg, 0, 10});
    add(e, {6, S::Sell, kPx + 20, 10, T::Limit, 0, 0, TimeInForce::GTD, inAnHour()});
    add(e, {7, S::Buy, kPx, 10, T::Pegged, 0, 0, TimeInForce::GTC, 0, 0, PegType::PrimaryPeg});
    add(e, {8, S::Sell, 0, 10, T::Stop, kPx - 40});
    add(e, {9, S::Sell, 0, 10, T::StopLimit, kPx - 40, 0, TimeInForce::GTC, 0, kPx - 60});
    add(e, {10, S::Sell, 0, 10, T::TrailingStop, 0, 0, TimeInForce::GTC, 0, 0,
            PegType::None, 0, 30});
    add(e, {11, S::Buy, 0, 10, T::MOC});
    add(e, {12, S::Sell, kPx - 5, 10, T::LOC});
    add(e, {13, S::Buy, kPx - 20, 10, T::Hidden});
    add(e, {14, S::Sell, kPx + 30, 10, T::PostOnly});
    add(e, {15, S::Buy, kPx - 30, 20, T::Limit, 0, 0, TimeInForce::GTC, 0, 0,
            PegType::None, 0, 0, 10});
}

// The call period: a bid through the offers up to the post-only one, an offer
// through the bids, and a parked market order each way.
void callPeriod(MatchingEngine& e) {
    using S = Side; using T = OrderType;
    add(e, {20, S::Buy, kPx + 30, 15, T::Limit});
    add(e, {21, S::Sell, kPx - 15, 10, T::Limit});
    add(e, {22, S::Buy, 0, 5, T::Market});
    add(e, {23, S::Sell, 0, 5, T::Market});
}

void continuous(MatchingEngine& e) { seed(e); }
void preOpen(MatchingEngine& e) {
    seed(e);
    assert(e.beginTradingSession({kSym}, kDate) == 1);
    callPeriod(e);
}
void openingAuction(MatchingEngine& e) {
    seed(e);
    to(e, TradingState::AuctionOpen);
    callPeriod(e);
}
void volatilityAuction(MatchingEngine& e) {
    seed(e);
    to(e, TradingState::VolatilityAuction);
    callPeriod(e);
}
// The on-close orders are released into the cross; two more arrive in it.
void closingAuction(MatchingEngine& e) {
    seed(e);
    to(e, TradingState::AuctionClose);
    callPeriod(e);
    add(e, {24, Side::Sell, 0, 5, OrderType::MOC});
    add(e, {25, Side::Buy, kPx + 5, 10, OrderType::LOC});
}
std::function<void(MatchingEngine&)> halted(void (*from)(MatchingEngine&)) {
    return [from](MatchingEngine& e) { from(e); to(e, TradingState::Halted); };
}
std::function<void(MatchingEngine&)> closed(std::function<void(MatchingEngine&)> from) {
    return [from](MatchingEngine& e) { from(e); to(e, TradingState::PostClose); };
}

// State, prices, then every order in the order a checkpoint walks them:
// levels front to back (bids, then asks), then the parked lists. "s8:S@0x10
// stop9960 parked" is sell order 8, a stop for 10, triggering at 99.60, not
// in the book; "/10" after the leaves is an iceberg's slice, "*" an elected
// stop.
std::string dump(const OrderBook& b) {
    std::string s = name(b.getTradingState());
    if (b.getTradingState() == TradingState::Halted)
        s += std::string("(from ") + name(b.preHaltState()) + ")";
    const OrderBook::CheckpointPrices p = b.checkpointPricesLocked();
    s += " date " + std::to_string(b.sessionDate()) + " last " + std::to_string(p.lastTrade) +
         " ref " + std::to_string(p.reference) + " |";
    b.forEachOrderLocked([&](const Order& o) {
        s += std::string(" ") + (o.side == Side::Buy ? "b" : "s") + std::to_string(o.id) + ":" +
             code(o.type) + "@" + std::to_string(o.price) + "x" + std::to_string(o.remainingQty);
        if (o.type == OrderType::Iceberg) s += "/" + std::to_string(o.visibleQty);
        if (o.stopPrice != 0) s += " stop" + std::to_string(o.stopPrice);
        if (o.isStopTriggered) s += "*";
        if (!o.inBook) s += " parked";
    });
    return s;
}

// The next transitions to continuous trading, then a seller sweeps the bids
// to 99.40 (the stops fire), a buyer lifts the offers to 100.60, and the book
// runs a closing auction.
std::string advance(OrderBook& b) {
    const uint64_t tradesBefore = b.getTradeCount();
    std::string path;
    for (int step = 0; step < 4 && b.getTradingState() != TradingState::Continuous; ++step) {
        switch (b.getTradingState()) {
        case TradingState::Halted:       b.setTradingState(b.preHaltState()); break;
        case TradingState::AuctionClose: b.setTradingState(TradingState::PostClose); break;
        case TradingState::PostClose:    b.beginSession(kDate + 1); break;
        default:                         b.setTradingState(TradingState::Continuous); break;
        }
        path += std::string(name(b.getTradingState())) + " > ";
    }
    b.addOrder(90, 90, Side::Sell, kPx - 60, 60, OrderType::Limit);
    b.addOrder(91, 91, Side::Buy, kPx + 60, 60, OrderType::Limit);
    path += std::to_string(b.getTradeCount() - tradesBefore) + " trades, " + dump(b);
    // Then the close, which releases the on-close orders into its cross.
    const uint64_t tradesBeforeClose = b.getTradeCount();
    b.setTradingState(TradingState::AuctionClose);
    b.setTradingState(TradingState::PostClose);
    return path + " || close: " + std::to_string(b.getTradeCount() - tradesBeforeClose) +
           " trades, " + dump(b);
}

struct Outcome { std::string restored, next; };

Outcome after(Life life, const std::function<void(MatchingEngine&)>& setup) {
    const std::string path = uniqueTempPath("checkpoint_state.wal");
    std::remove(path.c_str());
    Outcome out;
    const auto run = [&](OrderBook& b) {
        out.restored = dump(b);
        out.next = advance(b);
    };
    const bool checkpoints = life == Life::Checkpointed || life == Life::CheckpointedTwice;
    {
        MatchingEngine engine;
        boot(engine, path);
        setup(engine);
        if (life == Life::Uninterrupted) run(*engine.getOrderBook(kSym));
        if (checkpoints) engine.gracefulShutdown();
        engine.stop();
    }
    const int restarts = life == Life::Uninterrupted ? 0 : life == Life::CheckpointedTwice ? 2 : 1;
    for (int i = 1; i <= restarts; ++i) {
        MatchingEngine restarted;
        boot(restarted, path);
        restarted.replayJournal();
        if (i == restarts) run(*restarted.getOrderBook(kSym));
        else restarted.gracefulShutdown();
        restarted.stop();
    }
    std::remove(path.c_str());
    return out;
}

int check(const char* what, const std::function<void(MatchingEngine&)>& setup) {
    int failures = 0;
    std::printf("  %s\n", what);
    const Outcome want = after(Life::Uninterrupted, setup);
    for (Life life : {Life::Uninterrupted, Life::Replayed, Life::Checkpointed,
                      Life::CheckpointedTwice}) {
        const Outcome got = life == Life::Uninterrupted ? want : after(life, setup);
        const bool restoredOk = got.restored == want.restored, nextOk = got.next == want.next;
        std::printf("    %-15s restored  %s%s\n", name(life), got.restored.c_str(),
                    restoredOk ? "" : "  <- differs");
        std::printf("    %-15s next      %s%s\n", name(life), got.next.c_str(),
                    nextOk ? "" : "  <- differs");
        failures += !restoredOk + !nextOk;
    }
    return failures;
}

}  // namespace

int main() {
    int failures = 0;
    failures += check("Continuous", continuous);
    failures += check("PreOpen", preOpen);
    failures += check("AuctionOpen", openingAuction);
    failures += check("VolatilityAuction", volatilityAuction);
    failures += check("AuctionClose", closingAuction);
    failures += check("Halted from Continuous", halted(continuous));
    failures += check("Halted from PreOpen", halted(preOpen));
    failures += check("Halted from AuctionOpen", halted(openingAuction));
    failures += check("Halted from VolatilityAuction", halted(volatilityAuction));
    failures += check("Halted from AuctionClose", halted(closingAuction));
    failures += check("PostClose from Continuous", closed(continuous));
    failures += check("PostClose after the closing cross", closed(closingAuction));
    failures += check("PostClose from a halted closing auction", closed(halted(closingAuction)));
    if (failures != 0) {
        std::printf("CheckpointTradingStateTest FAILED: %d restarted book state(s) differ\n",
                    failures);
        return 1;
    }
    std::puts("CheckpointTradingStateTest passed");
    return 0;
}
