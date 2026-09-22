#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  REFERENCE PRICE-TIME FIFO MATCHER
//
//  WRITTEN FROM THE DEFINITION OF PRICE-TIME PRIORITY. NOT FROM src/OrderBook.cpp.
//
//  This file exists to disagree with the engine. That only means anything if it
//  was derived independently, so it is written from this description and
//  nothing else:
//
//     * An aggressing order consumes resting liquidity BEST PRICE FIRST.
//     * Within one price, EARLIEST ARRIVAL FIRST (FIFO).
//     * A trade happens at the RESTING (maker) order's price. The maker posted
//       the price; the taker accepted it.
//     * Consumption continues until the aggressor is filled or there is no
//       crossable price left.
//     * Limit   — an unfilled remainder RESTS at the back of its price level.
//     * Market  — no limit; sweeps until filled or the opposite side is empty;
//                 any remainder is cancelled, never rested.
//     * IOC     — matches at its limit like a Limit; remainder cancelled.
//     * FOK     — fills completely or not at all; if the crossable quantity at
//                 its limit is short, nothing trades and nothing rests.
//     * Cancel  — removes a resting order. Only the owner may (kAnyParticipant
//                 means "the venue is asking", which is always allowed).
//     * Modify  — this engine amends DOWN IN PLACE only, which keeps queue
//                 position. Anything else fails and leaves the order untouched.
//
//  The implementation is deliberately the dumbest thing that implements that:
//  std::map of price -> std::vector of orders in arrival order, linear scans,
//  no indices, no incremental aggregates, erase-from-front. It is slow. It is
//  meant to be READ and agreed with line by line, because it is the only thing
//  standing behind "the engine is correct".
//
//  Scope is the price-time core: Limit, Market, IOC, FOK. No icebergs,
//  pro-rata, stops, pegs, auctions, hidden orders or self-trade prevention —
//  those need a written specification, and a reference guessing at them would
//  be worse than no reference.
// ─────────────────────────────────────────────────────────────────────────────

#include "Types.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace RefMatch {

using OrderMatcher::kAnyParticipant;
using OrderMatcher::OrderId;
using OrderMatcher::OrderType;
using OrderMatcher::ParticipantId;
using OrderMatcher::Price;
using OrderMatcher::Quantity;
using OrderMatcher::Side;

struct RefTrade {
    OrderId buyId;
    OrderId sellId;
    Price price;
    Quantity qty;

    bool operator==(const RefTrade& o) const {
        return buyId == o.buyId && sellId == o.sellId && price == o.price && qty == o.qty;
    }
};

struct RefOrder {
    OrderId id;
    ParticipantId owner;
    Quantity qty;
};

// One price level as the outside world sees it, plus the queue behind it.
struct RefLevel {
    Price price;
    Quantity totalQty;
    uint32_t orderCount;
    std::vector<OrderId> queue;  // front = next to fill
};

enum class AddOutcome { Accepted, RejectedFOK, RejectedDuplicateId };

class RefBook {
public:
    // Price-keyed queues. Bids are read back-to-front (highest price is best),
    // asks front-to-back (lowest price is best). One container type for both
    // sides so there is only one copy of the matching loop to check.
    using Ladder = std::map<Price, std::vector<RefOrder>>;

    AddOutcome add(OrderId id, ParticipantId owner, Side side, Price limitPrice, Quantity qty,
                   OrderType type, std::vector<RefTrade>& tradesOut) {
        if (live_.count(id) != 0) return AddOutcome::RejectedDuplicateId;

        // Fill-or-kill is decided before anything moves: if the book cannot
        // cover the whole order at a crossable price, the order never existed.
        if (type == OrderType::FOK && crossableQty(side, limitPrice, false) < qty) {
            return AddOutcome::RejectedFOK;
        }

        Quantity remaining = qty;
        const bool isMarket = (type == OrderType::Market);
        Ladder& opposite = (side == Side::Buy) ? asks_ : bids_;

        while (remaining > 0 && !opposite.empty()) {
            // Best price on the side being consumed: lowest ask, highest bid.
            auto lvl = (side == Side::Buy) ? opposite.begin() : std::prev(opposite.end());
            const Price restingPrice = lvl->first;
            if (!isMarket && !crosses(side, limitPrice, restingPrice)) break;

            std::vector<RefOrder>& queue = lvl->second;
            while (remaining > 0 && !queue.empty()) {
                RefOrder& maker = queue.front();  // earliest arrival at this price
                const Quantity traded = std::min(remaining, maker.qty);
                tradesOut.push_back(side == Side::Buy
                                        ? RefTrade{id, maker.id, restingPrice, traded}
                                        : RefTrade{maker.id, id, restingPrice, traded});
                remaining -= traded;
                maker.qty -= traded;
                if (maker.qty == 0) {
                    live_.erase(maker.id);
                    queue.erase(queue.begin());
                }
            }
            if (queue.empty()) opposite.erase(lvl);
        }

        // Only a Limit order leaves a remainder behind. Market and IOC cancel
        // theirs; FOK never gets here with one.
        if (remaining > 0 && type == OrderType::Limit) {
            (side == Side::Buy ? bids_ : asks_)[limitPrice].push_back({id, owner, remaining});
            live_.insert(id);
        }
        return AddOutcome::Accepted;
    }

    // True if the order was removed.
    bool cancel(OrderId id, ParticipantId requester) {
        for (Ladder* book : {&bids_, &asks_}) {
            for (auto lvl = book->begin(); lvl != book->end(); ++lvl) {
                std::vector<RefOrder>& queue = lvl->second;
                for (auto o = queue.begin(); o != queue.end(); ++o) {
                    if (o->id != id) continue;
                    if (requester != kAnyParticipant && o->owner != requester) return false;
                    queue.erase(o);
                    live_.erase(id);
                    if (queue.empty()) book->erase(lvl);
                    return true;
                }
            }
        }
        return false;
    }

    // Amend down in place; queue position is kept because the order did not
    // move and nobody behind it gained anything. Any other amendment fails.
    bool modify(OrderId id, Quantity newQty, ParticipantId requester) {
        for (Ladder* book : {&bids_, &asks_}) {
            for (auto& [price, queue] : *book) {
                (void)price;
                for (RefOrder& o : queue) {
                    if (o.id != id) continue;
                    if (requester != kAnyParticipant && o.owner != requester) return false;
                    if (newQty == 0 || newQty >= o.qty) return false;
                    o.qty = newQty;
                    return true;
                }
            }
        }
        return false;
    }

    // Best price first.
    std::vector<RefLevel> ladder(Side side) const {
        const Ladder& book = (side == Side::Buy) ? bids_ : asks_;
        std::vector<RefLevel> out;
        out.reserve(book.size());
        for (const auto& [price, queue] : book) {
            RefLevel lvl{price, 0, static_cast<uint32_t>(queue.size()), {}};
            for (const RefOrder& o : queue) {
                lvl.totalQty += o.qty;
                lvl.queue.push_back(o.id);
            }
            out.push_back(std::move(lvl));
        }
        if (side == Side::Buy) std::reverse(out.begin(), out.end());  // highest bid first
        return out;
    }

    // Every resting order, keyed by id, for whole-book comparison.
    std::map<OrderId, RefOrder> resting() const {
        std::map<OrderId, RefOrder> out;
        for (const Ladder* book : {&bids_, &asks_})
            for (const auto& [price, queue] : *book) {
                (void)price;
                for (const RefOrder& o : queue) out[o.id] = o;
            }
        return out;
    }

    size_t levelCount(Side side) const { return (side == Side::Buy ? bids_ : asks_).size(); }
    size_t restingCount() const { return live_.size(); }
    bool isLive(OrderId id) const { return live_.count(id) != 0; }

    // Remaining size of a resting order, or 0 if it is not resting.
    Quantity qtyOf(OrderId id) const {
        for (const Ladder* book : {&bids_, &asks_})
            for (const auto& [price, queue] : *book) {
                (void)price;
                for (const RefOrder& o : queue)
                    if (o.id == id) return o.qty;
            }
        return 0;
    }

    // Best price on a side, or 0 when that side is empty.
    Price best(Side side) const {
        if (side == Side::Buy) return bids_.empty() ? 0 : std::prev(bids_.end())->first;
        return asks_.empty() ? 0 : asks_.begin()->first;
    }

    // How much an aggressor on `side` could trade against the book at `limitPrice`.
    // Used by FOK, and by the generator to aim orders at the fill/no-fill edge.
    Quantity crossableQty(Side side, Price limitPrice, bool isMarket) const {
        const Ladder& opposite = (side == Side::Buy) ? asks_ : bids_;
        Quantity total = 0;
        for (const auto& [price, queue] : opposite) {
            if (!isMarket && !crosses(side, limitPrice, price)) continue;
            for (const RefOrder& o : queue) total += o.qty;
        }
        return total;
    }

    static bool crosses(Side aggressor, Price limitPrice, Price restingPrice) {
        return aggressor == Side::Buy ? limitPrice >= restingPrice : limitPrice <= restingPrice;
    }

private:
    Ladder bids_;
    Ladder asks_;
    std::set<OrderId> live_;
};

}  // namespace RefMatch
