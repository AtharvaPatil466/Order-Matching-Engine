#pragma once

#include "EventListener.h"
#include "Journal.h"

#include <optional>
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "Types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace OrderMatcher {

// ResearchHarness — replay driver for microstructure research.
//
// Loads a Journal file, translates each entry to an OrderRequest, and
// submits it to a live MatchingEngine using the engine's synchronous
// submitOrder / submitCancel / submitModify / submitCancelReplace paths.
// An internal EventListener is installed on every OrderBook so that every
// Trade and book-update fires the user-supplied callbacks.
//
// Lifecycle:
//   1. Create the engine and add all required symbols via engine.addSymbol().
//   2. Call engine.start().
//   3. Construct a ResearchHarness and call replay() or loadJournal()/step().
//
// The harness does NOT own the engine and does NOT register new symbols; the
// caller must have added all symbols that appear in the journal before
// starting the engine.

class ResearchHarness : private EventListener {
public:
    using TradeCallback      = std::function<void(const Trade&)>;
    using BookUpdateCallback = std::function<void(SymbolId, Price /*midpoint*/,
                                                  Quantity /*bidQty*/,
                                                  Quantity /*askQty*/)>;

    explicit ResearchHarness(MatchingEngine& engine)
        : engine_(engine) {}

    // -----------------------------------------------------------------
    // Bulk replay
    // -----------------------------------------------------------------

    // Replay every entry from journalPath through the engine.
    // Registered TradeCallback / BookUpdateCallback are called for each
    // event produced during replay.
    // Returns the number of journal entries replayed.
    uint64_t replay(const std::string& journalPath) {
        loadJournal(journalPath);
        while (step()) {}
        return eventsReplayed_;
    }

    // -----------------------------------------------------------------
    // Step-by-step replay
    // -----------------------------------------------------------------

    // Load journal entries into an in-memory buffer; does not replay yet.
    void loadJournal(const std::string& journalPath) {
        // Flush any previous state.
        entries_.clear();
        cursor_    = 0;
        eventsReplayed_ = 0;
        tradesObserved_ = 0;

        Journal j(journalPath, Journal::SyncPolicy::GroupCommit, 64);
        entries_ = j.readAll(/*validateCRC=*/true, /*validateSequence=*/false);

        // Install this harness as the listener on every known book.
        installListeners();
    }

    // Replay one entry; returns false when all entries have been processed.
    bool step() {
        if (cursor_ >= entries_.size()) {
            return false;
        }

        submitEntry(entries_[cursor_]);
        ++cursor_;
        ++eventsReplayed_;
        return true;
    }

    // -----------------------------------------------------------------
    // Callback registration
    // -----------------------------------------------------------------

    void onTrade(TradeCallback cb)           { tradeCb_      = std::move(cb); }
    void onBookUpdate(BookUpdateCallback cb) { bookUpdateCb_ = std::move(cb); }

    // -----------------------------------------------------------------
    // Counters
    // -----------------------------------------------------------------

    uint64_t tradesObserved()  const { return tradesObserved_; }
    uint64_t eventsReplayed()  const { return eventsReplayed_; }

private:
    // ── EventListener interface ──────────────────────────────────────

    void onTrade(const Trade& t) override {
        ++tradesObserved_;
        if (tradeCb_) tradeCb_(t);

        // After a trade we emit a book-update with the current mid-price
        // and top-of-book quantities.
        if (bookUpdateCb_) {
            OrderBook* bk = engine_.getOrderBook(t.symbolId);
            if (bk) {
                emitBookUpdate(*bk, t.symbolId);
            }
        }
    }

    void onOrderUpdate(const OrderUpdate& /*u*/) override {}

    void onMarketData(const MarketDataUpdate& u) override {
        // After every book-level change, fire the book-update callback
        // so researchers can track the spread / depth evolution even when
        // no trade occurred.
        if (!bookUpdateCb_) return;
        // We need to know the symbol; we'll fire for every registered book.
        // In practice callers only register one symbol at a time for replay.
        for (SymbolId sym : knownSymbols_) {
            OrderBook* bk = engine_.getOrderBook(sym);
            if (bk) {
                emitBookUpdate(*bk, sym);
            }
        }
        (void)u;
    }

    // ── Helpers ──────────────────────────────────────────────────────

    // Which book currently holds `orderId`, or nullopt if no book does — an
    // order already filled or cancelled earlier in the replay, where doing
    // nothing is the right answer. Nullopt rather than a sentinel symbol
    // because 0 is a legal SymbolId and is exactly the wrong guess to make
    // here; that is the bug this replaced.
    //
    // Single-threaded by construction: the harness replays one entry at a
    // time on the caller's thread, which is the contract getOrder() documents.
    std::optional<SymbolId> symbolOf(OrderId orderId) const {
        for (SymbolId s : knownSymbols_) {
            const OrderBook* bk = engine_.getOrderBook(s);
            if (bk && bk->getOrder(orderId)) {
                return s;
            }
        }
        return std::nullopt;
    }

    void installListeners() {
        knownSymbols_.clear();
        // Collect symbols referenced by entries and install this listener
        // on each corresponding OrderBook that already exists in the engine.
        // The caller is responsible for having registered all symbols via
        // engine.addSymbol() before engine.start().
        for (const auto& e : entries_) {
            SymbolId sym = e.symbolId;
            // Check if we already tracked this symbol.
            bool found = false;
            for (SymbolId s : knownSymbols_) {
                if (s == sym) { found = true; break; }
            }
            if (!found) {
                knownSymbols_.push_back(sym);
                OrderBook* bk = engine_.getOrderBook(sym);
                if (bk) {
                    bk->setEventListener(this);
                }
            }
        }
    }

    void submitEntry(const JournalEntry& e) {
        switch (e.entryType) {
            case JournalEntry::Type::AddOrder:
            case JournalEntry::Type::Snapshot:
                engine_.submitOrder(
                    e.symbolId, e.orderId, e.participantId,
                    e.side, e.price, e.quantity, e.orderType,
                    e.stopPrice, e.displayQty,
                    e.timeInForce, e.expiryTime,
                    e.stopLimitPrice,
                    e.pegType, e.pegOffset,
                    e.trailAmount, e.minQty, e.hidden);
                break;

            // These three records carry NO symbol: Journal::logCancelOrder and
            // friends build a value-initialised JournalEntry and set only the
            // type, timestamp and order id, so symbolId reads 0. Dispatching on
            // it sent every cancel, modify and replace to book 0 — correct by
            // accident for a single-symbol journal, and a silent no-op for any
            // other symbol. Research replay of a real multi-symbol journal
            // therefore kept every cancelled order resting.
            //
            // Resolve by order id instead, which is what the engine's own
            // replayJournal() does with the same records.
            case JournalEntry::Type::CancelOrder:
                if (auto sym = symbolOf(e.orderId)) {
                    engine_.submitCancel(*sym, e.orderId);
                }
                break;

            case JournalEntry::Type::ModifyOrder:
                if (auto sym = symbolOf(e.orderId)) {
                    engine_.submitModify(*sym, e.orderId, e.newQty);
                }
                break;

            case JournalEntry::Type::CancelReplace:
                if (auto sym = symbolOf(e.orderId)) {
                    engine_.submitCancelReplace(*sym, e.orderId,
                                                e.newPrice, e.newQty);
                }
                break;
        }
    }

    void emitBookUpdate(const OrderBook& bk, SymbolId sym) {
        // getMidPrice() is lock-free (reads the price index directly) and
        // safe to call from within the EventListener callbacks, which are
        // invoked while the book's bookLock_ unique_lock is held by the
        // writer.  getSnapshot() takes a shared_lock and would deadlock,
        // so we pass 0 for the bid/ask quantities here; callers that need
        // full depth should take a snapshot *outside* the callback.
        Price mid = bk.getMidPrice();
        bookUpdateCb_(sym, mid, /*bidQty=*/0, /*askQty=*/0);
    }

    // ── State ────────────────────────────────────────────────────────
    MatchingEngine&              engine_;
    std::vector<JournalEntry>    entries_;
    size_t                       cursor_{0};
    uint64_t                     eventsReplayed_{0};
    uint64_t                     tradesObserved_{0};

    TradeCallback                tradeCb_;
    BookUpdateCallback           bookUpdateCb_;

    std::vector<SymbolId>        knownSymbols_;
};

} // namespace OrderMatcher
