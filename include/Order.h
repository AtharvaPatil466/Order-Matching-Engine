#pragma once

#include "Types.h"

#include <cstddef>  // offsetof, for the hot-region static_assert

namespace OrderMatcher {

// alignas(64) on the struct itself ensures every Order object starts on
// a cache-line boundary — critical for avoiding false sharing during matching.
// The previous pattern (alignas on a trailing zero-length array) only aligned
// the padding field, not the struct.
//
// HOT / COLD FIELD ORDERING
// -------------------------
// Fields are split into a "hot" region (everything the continuous matching
// loop in OrderBook.cpp touches) followed by a "cold" region (lifecycle,
// risk, protocol and exotic-order fields read off the hot path). The hot
// region is laid out to fit inside the FIRST 64-byte cache line, so a normal
// limit-order match — list traversal plus price/qty/fill math — faults a
// single line per order instead of straying across the struct.
//
// Hot set (read inside the match loop): id, next, prev, price, remainingQty,
// initialQty (fill calc), visibleQty (iceberg available), side, type, status,
// isHidden. The eight 8-byte hot fields plus the four 1-byte enums/bool fill
// the line exactly (56 + 4 + 4 pad = 64). displayQty (iceberg slice refill
// only) is the first cold field and therefore the first byte of line 2; the
// common path never reads it. timestamp is write-once at creation and is read
// only off the hot path, so it lives in the cold region too.
//
// This is a pure field reorder: identical field names, types and default
// initializers, so every existing call site and the ObjectPool<Order> layout
// are unchanged. Note line size is 64B on x86-64 (the CI target) and 128B on
// Apple Silicon, so on ARM the hot set trivially shares one line either way;
// the 64-byte packing is what matters for the x86 hot path.

struct alignas(64) Order {
    // ─── HOT (first cache line: matching-loop working set) ──────────────────
    OrderId id;

    // Intrusive list pointers — traversed every iteration of the match loop.
    Order* next = nullptr;
    Order* prev = nullptr;

    Price price;
    Quantity remainingQty;
    Quantity initialQty;        // fill calc: filled = initialQty - remainingQty
    Quantity visibleQty = 0;    // iceberg: displayed quantity available to match

    Side side;
    OrderType type;
    OrderStatus status = OrderStatus::New;
    bool isHidden = false;
    // 4 bytes padding here → hot region ends exactly at offset 64.

    // ─── COLD (rarely touched on the hot path) ──────────────────────────────
    Quantity displayQty = 0;    // Iceberg: amount to refresh each slice

    // Participant
    ParticipantId participantId;
    SymbolId symbolId = 0;

    // Time-in-Force
    TimeInForce timeInForce = TimeInForce::GTC;
    uint64_t timestamp;         // set once at creation; read only off hot path
    uint64_t expiryTime = 0;    // For GTD/DAY orders

    // Stop orders
    Price stopPrice = 0;
    Price stopLimitPrice = 0;   // Stop-Limit: limit price after stop triggers
    bool isStopTriggered = false;

    // Pegged orders
    PegType pegType = PegType::None;
    Price pegOffset = 0;        // Offset from peg reference

    // Trailing Stop
    Price trailAmount = 0;
    Price trailRefPrice = 0;    // Tracks best price seen

    // Minimum execution quantity
    Quantity minQty = 0;

    // Book-membership flag for the STP occupancy counter (OrderBook::stpResting_):
    // true iff this order is currently RESTING in bids_/asks_. Set on book entry
    // (addToBook), cleared on every book exit, so the per-participant resting
    // count stays balanced across cancel/fill/expiry/uncross without an O(depth)
    // rescan. Cold field — touched only at book entry/exit, never in the fill
    // math, and absorbed by existing padding (sizeof(Order) stays 192).
    bool inBook = false;
};

// The hot working set must fit in the first 64-byte cache line. displayQty is
// the first cold field, so its offset is the end of the hot region; keeping it
// at (or below) 64 proves every loop-touched field shares line one on x86-64.
static_assert(offsetof(Order, displayQty) <= 64,
              "Order hot fields must fit within the first 64-byte cache line");

// Reordering must not grow the object — keep parity with the 192-byte layout
// the ObjectPool capacity math was sized against.
static_assert(sizeof(Order) == 192, "Order size changed; check pool sizing");

// Publicly displayed size of a resting order: an iceberg shows only its
// current slice, everything else shows its full leaves. Hidden orders are
// filtered by the caller — they display nothing at all, which is a
// suppress-the-event decision rather than a quantity of zero.
//
// This is the single definition of "what the market can see" for one order.
// The L2 aggregation in OrderBook::notifyMarketData and the order-level
// BookVisibleUpdate feed must agree on it, or the two market-data feeds
// describe different books.
inline Quantity displayQuantity(const Order& o) {
    return (o.type == OrderType::Iceberg) ? o.visibleQty : o.remainingQty;
}

} // namespace OrderMatcher
