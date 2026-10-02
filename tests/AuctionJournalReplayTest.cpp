// AuctionJournalReplayTest — recovery reproduces what the auctions did.
//
// WHAT WAS WRONG (roadmap 1.8-H1, finding AUCT-1). The journal recorded only
// orders, cancels, modifies, replaces and snapshots. Trading-state changes
// and uncrosses wrote nothing, so replay applied every order accumulated in
// PreOpen to a book in its default Continuous state, matching on arrival in
// arrival order. PreOpen buy 105, sell 100, buy 110, then the open: live, the
// 110 bid fills against the sell at the cross and the 105 bid rests;
// recovered, the 105 bid fills at 105 and the 110 bid rests. Orders that
// filled came back resting and the other way round, with a different last
// price — and a checkpoint taken in an auction state lost the state itself.
//
// WHAT IS PINNED. The live book and the recovered book agree — orders,
// remaining quantities, last price and trading state — after an opening
// cross, an explicit uncross, async workers, and a checkpoint taken while
// the book is accumulating.

#include "Journal.h"
#include "MatchingEngine.h"
#include "TempPath.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unistd.h>

using namespace OrderMatcher;
namespace fs = std::filesystem;

namespace {

constexpr SymbolId kSym = 0;

struct BookState {
    std::map<OrderId, Quantity> resting;  // id -> remainingQty
    Price lastTradePrice = 0;
    uint64_t trades = 0;
    TradingState state = TradingState::Continuous;
    bool operator==(const BookState&) const = default;
};

BookState capture(MatchingEngine& e) {
    OrderBook* b = e.getOrderBook(kSym);
    BookState s;
    b->forEachOrderLocked([&](const Order& o) { s.resting[o.id] = o.remainingQty; });
    s.lastTradePrice = b->getSnapshot().lastTradePrice;
    s.trades = b->getTradeCount();
    s.state = b->getTradingState();
    return s;
}

fs::path freshPath(const char* tag) {
    fs::path p = uniqueTempPath(std::string("auction_journal_") + tag + ".bin");
    fs::remove(p);
    fs::remove(fs::path(p.string() + ".tmp"));
    return p;
}

BookState recover(const fs::path& path) {
    MatchingEngine e;
    e.addSymbol(kSym);
    e.getOrderBook(kSym)->setCircuitBreakerThreshold(1e9);
    assert(e.enableJournal(path.string()));
    e.replayJournal();
    return capture(e);
}

// Runs `session` against a journaled engine (sync, or async with one worker),
// then checks a fresh engine replaying the journal ends in the same state.
void liveEqualsRecovered(const char* tag, bool async,
                         const std::function<void(MatchingEngine&)>& session) {
    const fs::path path = freshPath(tag);
    BookState live;
    {
        MatchingEngine e;
        e.addSymbol(kSym);
        e.getOrderBook(kSym)->setCircuitBreakerThreshold(1e9);
        assert(e.enableJournal(path.string()));
        if (async) e.startAsync(1, 1024); else e.start();
        session(e);
        e.waitForDrain();
        live = capture(e);
        if (async) e.stopAsync(); else e.stop();
    }
    const BookState recovered = recover(path);
    if (!(recovered == live)) {
        std::printf("  [%s] live: %zu resting, last %lld, %llu trades, state %d\n", tag,
                    live.resting.size(), (long long)live.lastTradePrice,
                    (unsigned long long)live.trades, (int)live.state);
        std::printf("  [%s] recovered: %zu resting, last %lld, %llu trades, state %d\n", tag,
                    recovered.resting.size(), (long long)recovered.lastTradePrice,
                    (unsigned long long)recovered.trades, (int)recovered.state);
    }
    assert(recovered == live && "the recovered book differs from the live one");
    fs::remove(path);
    std::printf("  %s: recovered book equals live\n", tag);
}

void openingCross(MatchingEngine& e) {
    e.setTradingStateBatch({kSym}, TradingState::PreOpen);
    e.submitOrder(kSym, 1, 101, Side::Buy, 105, 100, OrderType::Limit);   // A
    e.submitOrder(kSym, 2, 102, Side::Sell, 100, 100, OrderType::Limit);  // B
    e.submitOrder(kSym, 3, 103, Side::Buy, 110, 100, OrderType::Limit);   // C
    e.setTradingStateBatch({kSym}, TradingState::Continuous);
    e.submitOrder(kSym, 4, 104, Side::Sell, 104, 50, OrderType::Limit);   // trades with A
}

void explicitUncrossThenClose(MatchingEngine& e) {
    e.setTradingStateBatch({kSym}, TradingState::AuctionOpen);
    e.submitOrder(kSym, 1, 101, Side::Buy, 105, 100, OrderType::Limit);
    e.submitOrder(kSym, 2, 102, Side::Sell, 100, 60, OrderType::Limit);
    e.uncrossBatch({kSym});                                 // cross, stay in auction
    e.submitOrder(kSym, 3, 103, Side::Sell, 101, 100, OrderType::Limit);
    e.setTradingStateBatch({kSym}, TradingState::Halted);
    e.setTradingStateBatch({kSym}, TradingState::Continuous);
    e.setTradingStateBatch({kSym}, TradingState::AuctionClose);
    e.submitOrder(kSym, 4, 104, Side::Buy, 0, 30, OrderType::MOC);
    e.setTradingStateBatch({kSym}, TradingState::PostClose);
}

// A checkpoint taken mid-auction must keep the state: the accumulated book is
// crossed, and restored into a Continuous book it would trade on arrival.
void checkpointInPreOpen() {
    const fs::path path = freshPath("checkpoint");
    BookState live;
    {
        MatchingEngine e;
        e.addSymbol(kSym);
        e.getOrderBook(kSym)->setCircuitBreakerThreshold(1e9);
        assert(e.enableJournal(path.string()));
        e.start();
        e.setTradingStateBatch({kSym}, TradingState::PreOpen);
        e.submitOrder(kSym, 1, 101, Side::Buy, 105, 100, OrderType::Limit);
        e.submitOrder(kSym, 2, 102, Side::Sell, 100, 100, OrderType::Limit);
        e.checkpoint();
        live = capture(e);
        e.stop();
    }
    const BookState recovered = recover(path);
    assert(recovered.state == TradingState::PreOpen &&
           "a checkpoint taken in PreOpen came back Continuous");
    assert(recovered == live && "the checkpointed auction book matched on replay");
    fs::remove(path);
    std::puts("  checkpoint in PreOpen: recovered book equals live");
}

// A journal written before these records existed is still readable, and its
// header is marked with the new format before a new record lands, so a build
// that cannot read state records refuses the file instead of misreading it.
void v1JournalIsReadAndUpgraded() {
    const fs::path path = freshPath("v1");
    {
        Journal j(path.string());
        j.logAddOrder(1, 101, kSym, Side::Buy, 100, 10, OrderType::Limit);
        j.flush();
    }
    {   // Rewind the header to version 1, as an older build wrote it.
        FILE* f = std::fopen(path.c_str(), "r+b");
        assert(f);
        JournalFileHeader h{};
        assert(std::fread(&h, sizeof(h), 1, f) == 1);
        h.formatVersion = 1;
        std::fseek(f, 0, SEEK_SET);
        assert(std::fwrite(&h, sizeof(h), 1, f) == 1);
        std::fclose(f);
    }
    {
        MatchingEngine e;
        e.addSymbol(kSym);
        assert(e.enableJournal(path.string()) && "a version-1 journal was refused");
        assert(e.replayJournal() == 1);
        e.start();
        e.setTradingStateBatch({kSym}, TradingState::PreOpen);
        e.stop();
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    JournalFileHeader h{};
    assert(f && std::fread(&h, sizeof(h), 1, f) == 1);
    std::fclose(f);
    assert(h.formatVersion == JOURNAL_FORMAT_CURRENT &&
           "a state record was appended to a file still marked version 1");
    assert(recover(path).state == TradingState::PreOpen);
    fs::remove(path);
    std::puts("  a version-1 journal is read, and upgraded before a state record lands");
}

}  // namespace

int main() {
    liveEqualsRecovered("opening_cross_sync", /*async=*/false, openingCross);
    liveEqualsRecovered("opening_cross_async", /*async=*/true, openingCross);
    liveEqualsRecovered("uncross_and_close", /*async=*/false, explicitUncrossThenClose);
    liveEqualsRecovered("uncross_and_close_async", /*async=*/true, explicitUncrossThenClose);
    checkpointInPreOpen();
    v1JournalIsReadAndUpgraded();
    std::puts("AuctionJournalReplayTest passed");
    return 0;
}
