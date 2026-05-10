#pragma once

#include "Types.h"

namespace OrderMatcher {

// alignas(64) on the struct itself ensures every Order object starts on
// a cache-line boundary — critical for avoiding false sharing during matching.
// The previous pattern (alignas on a trailing zero-length array) only aligned
// the padding field, not the struct.

struct alignas(64) Order {
    // Hot path fields (frequently accessed during matching)
    OrderId id;
    Price price;
    Quantity remainingQty;
    Side side;
    OrderType type;
    OrderStatus status = OrderStatus::New;
    bool isHidden = false;
    uint64_t timestamp;

    // Intrusive List Pointers
    Order* next = nullptr;
    Order* prev = nullptr;

    // Quantity tracking
    Quantity initialQty;
    Quantity visibleQty = 0;
    Quantity displayQty = 0;  // Iceberg: amount to refresh each time

    // Participant
    ParticipantId participantId;
    SymbolId symbolId = 0;

    // Time-in-Force
    TimeInForce timeInForce = TimeInForce::GTC;
    uint64_t expiryTime = 0;  // For GTD/DAY orders

    // Stop orders
    Price stopPrice = 0;
    bool isStopTriggered = false;

    // Stop-Limit: limit price after stop triggers (separate from stopPrice)
    Price stopLimitPrice = 0;

    // Pegged orders
    PegType pegType = PegType::None;
    Price pegOffset = 0;  // Offset from peg reference

    // Trailing Stop
    Price trailAmount = 0;
    Price trailRefPrice = 0;  // Tracks best price seen

    // Minimum execution quantity
    Quantity minQty = 0;
};

} // namespace OrderMatcher
