#pragma once

// DurabilityGate — hold client-visible events until the journal entry that
// produced them is durable, then release them in order.
//
// THE PROBLEM (audit C4)
//
// MatchingEngine::submitOrder calls book->addOrder(), which runs the whole
// match and dispatches every fill and status update to the client listener
// from inside it. Only afterwards does the engine call journal_->logAddOrder().
// So a client-visible fill precedes not just the fdatasync behind it but the
// journal APPEND entirely. With the default GroupCommit policy syncing every
// 64 entries, a kill -9 can lose orders whose fills clients have already acted
// on — and the venue has no record of trades its participants believe happened.
//
// Nothing else closes this. The replication ack is already correctly gated on
// Journal's durability barrier (it fires onCommit_ strictly after a successful
// syncFile), but no client-facing path waits for anything.
//
// THE MECHANISM
//
// This is an EventListener decorator. While armed it captures what the book
// emits instead of forwarding it; the engine ties each captured group to the
// ordinal of the journal entry for that order; and Journal's existing commit
// callback releases every group whose entry is now on stable storage.
//
// Entries commit strictly FIFO — commitBatch() writes a prefix of batch_ and
// pops exactly what it wrote — so a single "entries appended" vs "entries
// committed" counter is enough to decide what is safe to release. Sequence
// numbers cannot be used for this: Journal assigns them at commit time, not
// append time, precisely so a crash leaks no phantom sequences.
//
// WHAT THIS DOES NOT DO
//
// It does not make anything durable faster; it makes the engine stop claiming
// durability it does not have. Turning it on moves client-visible latency from
// "immediately" to "at the next commit", which under GroupCommit(64) is a real
// P50 cost. That is the honest trade and the reason it is opt-in rather than
// the default: what a venue promises on an ack belongs to whoever operates it,
// not to this class.
//
// THREADING
//
// Arm/capture/commit run on the thread processing the order. releaseThrough()
// runs from Journal's commit callback. On the synchronous journal path that is
// the same thread, which is the configuration this supports today. Journal's
// async io_uring path fires onCommit_ from a reaper thread and documents that
// the callback must be lock-free, so enabling the gate alongside it needs a
// lock-free handoff that does not exist yet — the engine refuses that
// combination rather than pretending.

// OrderBook.h, not EventListener.h: the latter only forward-declares the four
// event structs, and this buffers them by value.
#include "EventListener.h"
#include "OrderBook.h"

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace OrderMatcher {

class DurabilityGate : public EventListener {
public:
    explicit DurabilityGate(EventListener* downstream = nullptr)
        : downstream_(downstream) {}

    void setDownstream(EventListener* d) { downstream_ = d; }
    EventListener* downstream() const { return downstream_; }

    // Off means pure pass-through: no buffering, current behaviour exactly.
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // ── EventListener ───────────────────────────────────────────────────────
    void onTrade(const Trade& t) override {
        if (!capturing_) { if (downstream_) downstream_->onTrade(t); return; }
        current_.push_back(Event{Event::Kind::Trade, t, {}, {}, {}});
    }
    void onOrderUpdate(const OrderUpdate& u) override {
        if (!capturing_) { if (downstream_) downstream_->onOrderUpdate(u); return; }
        current_.push_back(Event{Event::Kind::OrderUpdate, {}, u, {}, {}});
    }
    void onMarketData(const MarketDataUpdate& u) override {
        if (!capturing_) { if (downstream_) downstream_->onMarketData(u); return; }
        current_.push_back(Event{Event::Kind::MarketData, {}, {}, u, {}});
    }
    void onBookVisible(const BookVisibleUpdate& u) override {
        if (!capturing_) { if (downstream_) downstream_->onBookVisible(u); return; }
        current_.push_back(Event{Event::Kind::BookVisible, {}, {}, {}, u});
    }

    // ── Engine-side control ─────────────────────────────────────────────────

    // Start capturing the events one order is about to produce. No-op when
    // disabled, so the caller does not need to branch.
    void beginOrder() {
        if (!enabled_) return;
        current_.clear();
        capturing_ = true;
    }

    // The order produced the journal entry with this append ordinal. Its
    // events stay held until that ordinal is reported durable.
    void commitOrder(uint64_t appendOrdinal) {
        if (!capturing_) return;
        capturing_ = false;
        if (current_.empty()) return;
        pending_.emplace_back(appendOrdinal, std::move(current_));
        current_.clear();
    }

    // The order produced NO journal entry — a reject, or a path that does not
    // log. There is nothing to become durable, so release immediately rather
    // than holding these forever.
    void abandonOrder() {
        if (!capturing_) return;
        capturing_ = false;
        dispatchAll(current_);
        current_.clear();
    }

    // Every journal entry up to and including `durableOrdinal` is on stable
    // storage. Release the groups that were waiting on them, in order.
    void releaseThrough(uint64_t durableOrdinal) {
        while (!pending_.empty() && pending_.front().first <= durableOrdinal) {
            auto group = std::move(pending_.front().second);
            pending_.pop_front();
            dispatchAll(group);
        }
    }

    // Event groups still waiting on a commit. A healthy engine drains this
    // every commit; a growing value means the journal has stopped making
    // progress and clients are being told nothing.
    size_t pendingGroups() const { return pending_.size(); }

    // Release everything still held, durable or not. For shutdown only: the
    // alternative is dropping events clients will never hear about, which is
    // worse than telling them about writes that may not have survived. The
    // caller is expected to have already flushed the journal.
    void releaseAllForShutdown() {
        if (capturing_) abandonOrder();
        while (!pending_.empty()) {
            auto group = std::move(pending_.front().second);
            pending_.pop_front();
            dispatchAll(group);
        }
    }

private:
    struct Event {
        enum class Kind : uint8_t { Trade, OrderUpdate, MarketData, BookVisible };
        Kind              kind;
        Trade             trade;
        OrderUpdate       orderUpdate;
        MarketDataUpdate  marketData;
        BookVisibleUpdate bookVisible;
    };

    void dispatchAll(const std::vector<Event>& events) {
        if (!downstream_) return;
        for (const auto& e : events) {
            switch (e.kind) {
                case Event::Kind::Trade:       downstream_->onTrade(e.trade);             break;
                case Event::Kind::OrderUpdate: downstream_->onOrderUpdate(e.orderUpdate); break;
                case Event::Kind::MarketData:  downstream_->onMarketData(e.marketData);   break;
                case Event::Kind::BookVisible: downstream_->onBookVisible(e.bookVisible); break;
            }
        }
    }

    EventListener* downstream_{nullptr};
    bool           enabled_{false};
    bool           capturing_{false};

    std::vector<Event>                                  current_;
    std::deque<std::pair<uint64_t, std::vector<Event>>> pending_;
};

}  // namespace OrderMatcher
