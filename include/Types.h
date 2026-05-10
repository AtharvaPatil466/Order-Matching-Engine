#pragma once

#include <cstdint>
#include <limits>

namespace OrderMatcher {

using OrderId = uint64_t;
using ParticipantId = uint64_t;
using Price = int64_t;
using Quantity = uint64_t;
using SymbolId = uint32_t;

enum class Side : uint8_t {
    Buy,
    Sell
};

enum class OrderType : uint8_t {
    Limit,
    Market,
    IOC,          // Immediate or Cancel
    FOK,          // Fill or Kill
    Stop,
    StopLimit,    // Stop with separate limit price
    Iceberg,
    PostOnly,     // Maker-only: reject if would cross spread
    Pegged,       // Pegged to reference price (mid, primary)
    TrailingStop,
    Hidden        // Fully dark order
};

enum class TimeInForce : uint8_t {
    GTC,  // Good Till Cancel (default)
    GTD,  // Good Till Date/Time
    DAY   // Expires at end of trading session
};

enum class OrderStatus : uint8_t {
    New,
    Accepted,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected
};

enum class PegType : uint8_t {
    None,
    MidPeg,      // Pegged to mid-price ((best_bid + best_ask) / 2)
    PrimaryPeg   // Pegged to same-side best (best bid for buy, best ask for sell)
};

enum class RejectReason : uint8_t {
    None,
    VolatilityCircuitBreaker, // The order that tripped the breaker
    PostOnlyWouldCross,
    FOKInsufficientLiquidity,
    RiskLimitBreached,
    InvalidPrice,
    InvalidQuantity,
    SymbolNotFound,
    OrderNotFound,
    OutOfPriceRange,
    CapacityExhausted,        // Object pool or book capacity full
    RateLimitExceeded,        // Per-client rate limit exceeded
    QueueBackpressure,
    EngineStopped,
    MarketHalted,             // Submitted while book is in Halted state
    OrderTypeNotAllowedInState, // e.g. Market/IOC/FOK during auction
    OutsidePriceBand,         // LULD-style price collar violation (no halt)
    MarketClosed,             // Submitted while book is in PostClose state
    DuplicateOrderId,         // AddOrder with an id already in the book
    UnsupportedFixVersion,    // BeginString outside the session's accept-list
    MissingRequiredField      // Required field absent (e.g. TransactTime on FIX 4.4)
};

// Trading state controls how new orders are admitted into the book and
// whether continuous matching runs. State transitions are monotonic with
// respect to the lifecycle of a single trading session and only the
// engine / regulator should drive them — not market participants.
enum class TradingState : uint8_t {
    Continuous,    // Normal trading: new orders match immediately
    Halted,        // Regulator/auto halt: reject new orders; cancels allowed
    AuctionOpen,   // Opening auction: orders accumulate; no continuous match
    AuctionClose,  // Closing auction: same admission rules as AuctionOpen
    PreOpen,       // Pre-session: same admission as AuctionOpen, prior to
                   // the opening uncross. Orders accumulate to seed the
                   // opening auction.
    PostClose      // Post-session: reject all new orders (MarketClosed);
                   // cancels still allowed for participants cleaning up
                   // remaining day orders.
};

enum class MatchAlgorithm : uint8_t {
    PriceTime,  // FIFO at each price level (default)
    ProRata     // Proportional allocation at each price level
};

// Fixed-point price constants (4 decimal places)
constexpr int64_t PRICE_PRECISION = 10000;

inline double toDouble(Price p) {
    return static_cast<double>(p) / PRICE_PRECISION;
}

inline Price toPrice(double p) {
    return static_cast<Price>(p * PRICE_PRECISION + 0.5);
}

} // namespace OrderMatcher
