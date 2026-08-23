#pragma once

// OuchProtocol — NASDAQ OUCH 4.2-style binary order-entry codec.
//
// Wire format (per NASDAQ OUCH 4.2 spec):
//   * Every message begins with a 1-byte MessageType.
//   * Numeric fields are big-endian (network byte order).
//   * ASCII fields are left-justified and space-padded.
//   * Frames are fixed-width — the message-type byte determines the
//     length, so no length prefix is needed on the wire (real OUCH
//     deployments wrap OUCH in SoupBinTCP, which adds the length; for
//     this implementation we frame directly off the message-type byte).
//
// Supported messages (subset chosen to mirror the existing FIX surface):
//   Inbound  (client → engine):
//     'O'  EnterOrder        49 bytes
//     'X'  CancelOrder       19 bytes
//   Outbound (engine → client):
//     'A'  OrderAccepted     66 bytes
//     'J'  OrderRejected     24 bytes
//     'C'  OrderCanceled     28 bytes
//     'E'  OrderExecuted     40 bytes
//
// Conventions:
//   * OrderToken (14 bytes ASCII) is the client-side identifier. To
//     keep mapping table-free, this codec parses the token as a
//     decimal uint64; tokens that aren't all-digits are rejected.
//     Real venues maintain a token→id table; we trade that for
//     simplicity.
//   * Stock (8 bytes ASCII) is parsed as a decimal SymbolId for the
//     same reason. Production codecs would index a symbology table.
//   * Firm (4 bytes ASCII) is parsed as a decimal ParticipantId.

#include "Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace OrderMatcher {

constexpr uint8_t OUCH_MT_ENTER_ORDER    = 'O';
constexpr uint8_t OUCH_MT_CANCEL_ORDER   = 'X';
constexpr uint8_t OUCH_MT_REPLACE_ORDER  = 'U';  // inbound: replace with new token
constexpr uint8_t OUCH_MT_ORDER_ACCEPTED = 'A';
constexpr uint8_t OUCH_MT_ORDER_REJECTED = 'J';
constexpr uint8_t OUCH_MT_ORDER_CANCELED = 'C';
constexpr uint8_t OUCH_MT_ORDER_EXECUTED = 'E';
constexpr uint8_t OUCH_MT_ORDER_REPLACED = 'U';  // outbound: same tag, direction
                                                  // disambiguates context

constexpr size_t OUCH_SIZE_ENTER_ORDER    = 49;
constexpr size_t OUCH_SIZE_CANCEL_ORDER   = 19;
constexpr size_t OUCH_SIZE_REPLACE_ORDER  = 47;
constexpr size_t OUCH_SIZE_ORDER_ACCEPTED = 66;
constexpr size_t OUCH_SIZE_ORDER_REJECTED = 24;
constexpr size_t OUCH_SIZE_ORDER_CANCELED = 28;
constexpr size_t OUCH_SIZE_ORDER_EXECUTED = 40;
constexpr size_t OUCH_SIZE_ORDER_REPLACED = 79;

// OUCH 4.2 TimeInForce encoding. Non-IOC, non-DAY values are seconds
// until expiry; we collapse those to GTC since the engine's TIF model
// is enum-valued. Mapping table:
//   0     → IOC
//   99998 → Market Hours Day
//   99999 → System Hours Day
//   other → treat as GTC (engine doesn't track second-granular expiry)
constexpr uint32_t OUCH_TIF_IOC     = 0;
constexpr uint32_t OUCH_TIF_DAY     = 99998;
constexpr uint32_t OUCH_TIF_DAY_SYS = 99999;

// OUCH 4.2 reject-reason character codes. Map onto our RejectReason
// enum; values are spec-defined single ASCII characters.
constexpr char OUCH_REJECT_QUOTE_UNAVAILABLE   = 'A';
constexpr char OUCH_REJECT_DESTINATION_CLOSED  = 'C';
constexpr char OUCH_REJECT_INVALID_DISPLAY     = 'D';
constexpr char OUCH_REJECT_INVALID_MAX_FLOOR   = 'F';
constexpr char OUCH_REJECT_INVALID_PEG         = 'G';
constexpr char OUCH_REJECT_INVALID_IOC         = 'I';
constexpr char OUCH_REJECT_HALT                = 'H';
constexpr char OUCH_REJECT_INVALID_PRICE       = 'K';
constexpr char OUCH_REJECT_INVALID_MIN_QTY     = 'M';
constexpr char OUCH_REJECT_OTHER               = 'O';
constexpr char OUCH_REJECT_INVALID_QTY         = 'Q';
constexpr char OUCH_REJECT_INVALID_STOCK       = 'S';
constexpr char OUCH_REJECT_INVALID_TIF         = 'T';
constexpr char OUCH_REJECT_RATE_LIMIT          = 'L';
constexpr char OUCH_REJECT_RISK                = 'R';

inline char ouchRejectCode(RejectReason r) {
    switch (r) {
    case RejectReason::SymbolNotFound:              return OUCH_REJECT_INVALID_STOCK;
    case RejectReason::InvalidPrice:                return OUCH_REJECT_INVALID_PRICE;
    case RejectReason::InvalidQuantity:             return OUCH_REJECT_INVALID_QTY;
    case RejectReason::InvalidDisplayQty:           return OUCH_REJECT_INVALID_QTY;
    case RejectReason::OrderNotFound:               return OUCH_REJECT_OTHER;
    case RejectReason::RateLimitExceeded:           return OUCH_REJECT_RATE_LIMIT;
    case RejectReason::QueueBackpressure:           return OUCH_REJECT_OTHER;
    case RejectReason::EngineStopped:               return OUCH_REJECT_DESTINATION_CLOSED;
    case RejectReason::CapacityExhausted:           return OUCH_REJECT_OTHER;
    case RejectReason::OutOfPriceRange:             return OUCH_REJECT_INVALID_PRICE;
    case RejectReason::OutsidePriceBand:            return OUCH_REJECT_INVALID_PRICE;
    case RejectReason::RiskLimitBreached:           return OUCH_REJECT_RISK;
    case RejectReason::FOKInsufficientLiquidity:    return OUCH_REJECT_INVALID_IOC;
    case RejectReason::PostOnlyWouldCross:          return OUCH_REJECT_OTHER;
    case RejectReason::VolatilityCircuitBreaker:    return OUCH_REJECT_HALT;
    case RejectReason::MarketHalted:                return OUCH_REJECT_HALT;
    case RejectReason::MarketClosed:                return OUCH_REJECT_DESTINATION_CLOSED;
    case RejectReason::OrderTypeNotAllowedInState:  return OUCH_REJECT_HALT;
    case RejectReason::DuplicateOrderId:            return OUCH_REJECT_OTHER;
    case RejectReason::UnsupportedFixVersion:       return OUCH_REJECT_OTHER;
    case RejectReason::MissingRequiredField:        return OUCH_REJECT_OTHER;
    case RejectReason::None:                        return OUCH_REJECT_OTHER;
    case RejectReason::KillSwitchActive:            return OUCH_REJECT_HALT;
    case RejectReason::PositionLimitExceeded:       return OUCH_REJECT_RISK;
    case RejectReason::FatFingerReject:             return OUCH_REJECT_RISK;
    case RejectReason::OrderToTradeRatioExceeded:   return OUCH_REJECT_RISK;
    case RejectReason::PoolCapacityExceeded:        return OUCH_REJECT_OTHER;
    }
    return OUCH_REJECT_OTHER;
}

// ─── Byte-order helpers ─────────────────────────────────────────────────────
// OUCH is big-endian. These do raw byte writes/reads without alignment
// constraints, so they're safe to call on a packed buffer at any offset.

inline void writeU16BE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void writeU32BE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void writeU64BE(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
    }
}

inline uint16_t readU16BE(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

inline uint32_t readU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

inline uint64_t readU64BE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | uint64_t(p[i]);
    return v;
}

// Write an ASCII string into a fixed-width slot, left-justified and
// space-padded. Truncates if src is longer than the slot.
inline void writeFixedAscii(uint8_t* p, size_t slot, std::string_view src) {
    size_t n = std::min(src.size(), slot);
    if (n) std::memcpy(p, src.data(), n);
    if (n < slot) std::memset(p + n, ' ', slot - n);
}

// Parse a fixed-width ASCII slot as a decimal unsigned integer.
// Treats spaces as padding (skipped at both ends). Returns false on
// any non-digit, non-space character, or empty value.
inline bool parseAsciiDecimal(const uint8_t* p, size_t slot, uint64_t& out) {
    out = 0;
    size_t i = 0;
    while (i < slot && p[i] == ' ') ++i;
    size_t end = slot;
    while (end > i && p[end - 1] == ' ') --end;
    if (i == end) return false;
    uint64_t v = 0;
    for (; i < end; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + uint64_t(p[i] - '0');
    }
    out = v;
    return true;
}

// Render a uint64 as a left-justified, space-padded ASCII slot. Returns
// false if the value doesn't fit (caller decides how to handle).
inline bool writeAsciiDecimal(uint8_t* p, size_t slot, uint64_t v) {
    char buf[24];
    int n = std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    if (n < 0 || static_cast<size_t>(n) > slot) return false;
    std::memcpy(p, buf, n);
    if (static_cast<size_t>(n) < slot) {
        std::memset(p + n, ' ', slot - static_cast<size_t>(n));
    }
    return true;
}

// ─── Decoded inbound messages ───────────────────────────────────────────────

struct OuchEnterOrder {
    uint64_t   orderToken{0};   // decoded from 14-byte ASCII
    Side       side{Side::Buy};
    Quantity   shares{0};
    SymbolId   stock{0};        // decoded from 8-byte ASCII
    Price      price{0};
    uint32_t   timeInForce{0};
    ParticipantId firm{0};
    char       display{'Y'};
    char       capacity{'O'};   // 'A'=agent, 'P'=principal, 'R'=riskless, 'O'=other
    char       intermarketSweep{'N'};
    Quantity   minQuantity{0};
    char       crossType{'N'};
    char       customerType{'R'};
};

struct OuchCancelOrder {
    uint64_t orderToken{0};
    Quantity sharesToCancel{0};  // 0 = cancel all remaining
};

struct OuchReplaceOrder {
    uint64_t   existingOrderToken{0};
    uint64_t   newOrderToken{0};
    Quantity   shares{0};
    Price      price{0};
    uint32_t   timeInForce{0};
    char       display{'Y'};
    Quantity   minQuantity{0};
    char       intermarketSweep{'N'};
};

// ─── Decoders ───────────────────────────────────────────────────────────────
// Each returns true on a structurally well-formed frame. Semantic
// validation (e.g. unknown symbol) is the session's responsibility.

inline bool decodeEnterOrder(const uint8_t* p, size_t len, OuchEnterOrder& out) {
    if (len != OUCH_SIZE_ENTER_ORDER || p[0] != OUCH_MT_ENTER_ORDER) {
        return false;
    }
    uint64_t token = 0;
    if (!parseAsciiDecimal(p + 1, 14, token)) return false;
    out.orderToken = token;

    char sideChar = static_cast<char>(p[15]);
    if (sideChar == 'B') out.side = Side::Buy;
    else if (sideChar == 'S' || sideChar == 'T') out.side = Side::Sell;  // 'T'=short
    else return false;

    out.shares = readU32BE(p + 16);

    uint64_t stock = 0;
    if (!parseAsciiDecimal(p + 20, 8, stock)) return false;
    out.stock = static_cast<SymbolId>(stock);

    out.price = static_cast<Price>(readU32BE(p + 28));
    out.timeInForce = readU32BE(p + 32);

    uint64_t firm = 0;
    if (!parseAsciiDecimal(p + 36, 4, firm)) return false;
    out.firm = static_cast<ParticipantId>(firm);

    out.display = static_cast<char>(p[40]);
    out.capacity = static_cast<char>(p[41]);
    out.intermarketSweep = static_cast<char>(p[42]);
    out.minQuantity = readU32BE(p + 43);
    out.crossType = static_cast<char>(p[47]);
    out.customerType = static_cast<char>(p[48]);
    return true;
}

// Client-side EnterOrder encoder — the exact inverse of decodeEnterOrder
// (same field offsets). Used by order-entry / measurement tools that need to
// put an EnterOrder on the wire. Buffer must be >= OUCH_SIZE_ENTER_ORDER.
inline size_t encodeEnterOrder(uint8_t* out, const OuchEnterOrder& o) {
    std::memset(out, ' ', OUCH_SIZE_ENTER_ORDER);
    out[0] = OUCH_MT_ENTER_ORDER;
    writeAsciiDecimal(out + 1, 14, o.orderToken);
    out[15] = (o.side == Side::Buy) ? 'B' : 'S';
    writeU32BE(out + 16, static_cast<uint32_t>(o.shares));
    writeAsciiDecimal(out + 20, 8, static_cast<uint64_t>(o.stock));
    writeU32BE(out + 28, static_cast<uint32_t>(o.price));
    writeU32BE(out + 32, o.timeInForce);
    writeAsciiDecimal(out + 36, 4, static_cast<uint64_t>(o.firm));
    out[40] = o.display;
    out[41] = o.capacity;
    out[42] = o.intermarketSweep;
    writeU32BE(out + 43, static_cast<uint32_t>(o.minQuantity));
    out[47] = o.crossType;
    out[48] = o.customerType;
    return OUCH_SIZE_ENTER_ORDER;
}

inline bool decodeCancelOrder(const uint8_t* p, size_t len, OuchCancelOrder& out) {
    if (len != OUCH_SIZE_CANCEL_ORDER || p[0] != OUCH_MT_CANCEL_ORDER) {
        return false;
    }
    uint64_t token = 0;
    if (!parseAsciiDecimal(p + 1, 14, token)) return false;
    out.orderToken = token;
    out.sharesToCancel = readU32BE(p + 15);
    return true;
}

// ReplaceOrder ('U' inbound): existing token + new token + new
// attributes. 47 bytes total.
//   0      MessageType 'U'
//   1-14   ExistingOrderToken (14B ASCII decimal)
//   15-28  NewOrderToken      (14B ASCII decimal)
//   29-32  Shares             (BE u32)
//   33-36  Price              (BE u32)
//   37-40  TimeInForce        (BE u32)
//   41     Display
//   42-45  MinimumQuantity    (BE u32)
//   46     IntermarketSweep
inline bool decodeReplaceOrder(const uint8_t* p, size_t len,
                               OuchReplaceOrder& out) {
    if (len != OUCH_SIZE_REPLACE_ORDER || p[0] != OUCH_MT_REPLACE_ORDER) {
        return false;
    }
    uint64_t t = 0;
    if (!parseAsciiDecimal(p + 1, 14, t)) return false;
    out.existingOrderToken = t;
    if (!parseAsciiDecimal(p + 15, 14, t)) return false;
    out.newOrderToken = t;
    out.shares           = readU32BE(p + 29);
    out.price            = static_cast<Price>(readU32BE(p + 33));
    out.timeInForce      = readU32BE(p + 37);
    out.display          = static_cast<char>(p[41]);
    out.minQuantity      = readU32BE(p + 42);
    out.intermarketSweep = static_cast<char>(p[46]);
    return true;
}

// Returns the fixed-width frame length for a given message type, or 0
// if the type isn't recognized. Used by the session to know how many
// bytes to consume from the stream once it has the leading type byte.
inline size_t ouchInboundFrameSize(uint8_t messageType) {
    switch (messageType) {
    case OUCH_MT_ENTER_ORDER:   return OUCH_SIZE_ENTER_ORDER;
    case OUCH_MT_CANCEL_ORDER:  return OUCH_SIZE_CANCEL_ORDER;
    case OUCH_MT_REPLACE_ORDER: return OUCH_SIZE_REPLACE_ORDER;
    default:                    return 0;
    }
}

// ─── Encoders (outbound) ────────────────────────────────────────────────────
// All take a uint64 nanosecond timestamp and the order token to echo.
// Buffers are sized exactly to the message; functions return the byte
// count written so callers can pipe straight into a send_(...) sink.

inline size_t encodeOrderAccepted(uint8_t* out, uint64_t timestampNs,
                                  uint64_t orderToken, Side side,
                                  Quantity shares, SymbolId stock,
                                  Price price, uint32_t timeInForce,
                                  ParticipantId firm,
                                  uint64_t orderReferenceNumber,
                                  char display = 'Y') {
    std::memset(out, ' ', OUCH_SIZE_ORDER_ACCEPTED);
    out[0] = OUCH_MT_ORDER_ACCEPTED;
    writeU64BE(out + 1, timestampNs);
    writeAsciiDecimal(out + 9, 14, orderToken);
    out[23] = (side == Side::Buy) ? 'B' : 'S';
    writeU32BE(out + 24, static_cast<uint32_t>(shares));
    writeAsciiDecimal(out + 28, 8, static_cast<uint64_t>(stock));
    writeU32BE(out + 36, static_cast<uint32_t>(price));
    writeU32BE(out + 40, timeInForce);
    writeAsciiDecimal(out + 44, 4, static_cast<uint64_t>(firm));
    out[48] = display;
    writeU64BE(out + 49, orderReferenceNumber);
    out[57] = 'O';   // capacity: other
    out[58] = 'N';   // intermarket sweep
    writeU32BE(out + 59, 0);
    out[63] = 'N';   // cross type
    out[64] = 'L';   // order state: live
    out[65] = 'R';   // customer type: retail
    return OUCH_SIZE_ORDER_ACCEPTED;
}

// ─── Client-side ack decoder ────────────────────────────────────────────────
// Mirrors encodeOrderAccepted. Used by order-entry / measurement tools that
// receive the ack and need to correlate it (by orderToken) with the order sent.

struct OuchOrderAccepted {
    uint64_t      timestampNs{0};
    uint64_t      orderToken{0};
    Side          side{Side::Buy};
    Quantity      shares{0};
    SymbolId      stock{0};
    Price         price{0};
    uint32_t      timeInForce{0};
    ParticipantId firm{0};
    char          display{'Y'};
    uint64_t      orderReferenceNumber{0};  // engine-side order id
    char          orderState{'L'};
};

inline bool decodeOrderAccepted(const uint8_t* p, size_t len,
                                OuchOrderAccepted& out) {
    if (len != OUCH_SIZE_ORDER_ACCEPTED || p[0] != OUCH_MT_ORDER_ACCEPTED) {
        return false;
    }
    out.timestampNs = readU64BE(p + 1);
    uint64_t token = 0;
    if (!parseAsciiDecimal(p + 9, 14, token)) return false;
    out.orderToken = token;
    out.side = (static_cast<char>(p[23]) == 'B') ? Side::Buy : Side::Sell;
    out.shares = readU32BE(p + 24);
    uint64_t stock = 0;
    if (!parseAsciiDecimal(p + 28, 8, stock)) return false;
    out.stock = static_cast<SymbolId>(stock);
    out.price = static_cast<Price>(readU32BE(p + 36));
    out.timeInForce = readU32BE(p + 40);
    uint64_t firm = 0;
    if (!parseAsciiDecimal(p + 44, 4, firm)) return false;
    out.firm = static_cast<ParticipantId>(firm);
    out.display = static_cast<char>(p[48]);
    out.orderReferenceNumber = readU64BE(p + 49);
    out.orderState = static_cast<char>(p[64]);
    return true;
}

inline size_t encodeOrderRejected(uint8_t* out, uint64_t timestampNs,
                                  uint64_t orderToken, char reasonCode) {
    out[0] = OUCH_MT_ORDER_REJECTED;
    writeU64BE(out + 1, timestampNs);
    writeAsciiDecimal(out + 9, 14, orderToken);
    out[23] = static_cast<uint8_t>(reasonCode);
    return OUCH_SIZE_ORDER_REJECTED;
}

inline size_t encodeOrderCanceled(uint8_t* out, uint64_t timestampNs,
                                  uint64_t orderToken, Quantity canceledShares,
                                  char reason = 'U') {
    // reason 'U' = user requested; 'I' = immediate (IOC); 'T' = timeout (TIF).
    out[0] = OUCH_MT_ORDER_CANCELED;
    writeU64BE(out + 1, timestampNs);
    writeAsciiDecimal(out + 9, 14, orderToken);
    writeU32BE(out + 23, static_cast<uint32_t>(canceledShares));
    out[27] = static_cast<uint8_t>(reason);
    return OUCH_SIZE_ORDER_CANCELED;
}

inline size_t encodeOrderExecuted(uint8_t* out, uint64_t timestampNs,
                                  uint64_t orderToken, Quantity executedShares,
                                  Price executionPrice, uint64_t matchNumber) {
    out[0] = OUCH_MT_ORDER_EXECUTED;
    writeU64BE(out + 1, timestampNs);
    writeAsciiDecimal(out + 9, 14, orderToken);
    writeU32BE(out + 23, static_cast<uint32_t>(executedShares));
    writeU32BE(out + 27, static_cast<uint32_t>(executionPrice));
    out[31] = 'N';  // liquidity flag: non-displayed; real venues map maker/taker here
    writeU64BE(out + 32, matchNumber);
    return OUCH_SIZE_ORDER_EXECUTED;
}

// OrderReplaced ('U' outbound): the ack for an inbound Replace.
// Carries both the new token (now active) and the previous token
// (the one the client used to reference the soon-to-be-replaced
// order). 79 bytes total.
//   0      MessageType 'U'
//   1-8    Timestamp                (BE u64)
//   9-22   NewOrderToken            (14B ASCII decimal)
//   23     BuySellIndicator
//   24-27  Shares (new)             (BE u32)
//   28-35  Stock                    (8B ASCII decimal)
//   36-39  Price (new)              (BE u32)
//   40-43  TimeInForce              (BE u32)
//   44-47  Firm                     (4B ASCII decimal)
//   48     Display
//   49-56  OrderReferenceNumber     (BE u64, engine-side id — unchanged)
//   57     Capacity
//   58     IntermarketSweep
//   59-62  MinimumQuantity          (BE u32)
//   63     CrossType
//   64     OrderState ('L'=live, 'D'=dead)
//   65-78  PreviousOrderToken       (14B ASCII decimal)
inline size_t encodeOrderReplaced(uint8_t* out, uint64_t timestampNs,
                                  uint64_t newOrderToken, Side side,
                                  Quantity shares, SymbolId stock, Price price,
                                  uint32_t timeInForce, ParticipantId firm,
                                  uint64_t orderReferenceNumber,
                                  Quantity minQuantity,
                                  uint64_t previousOrderToken,
                                  char display = 'Y') {
    std::memset(out, ' ', OUCH_SIZE_ORDER_REPLACED);
    out[0] = OUCH_MT_ORDER_REPLACED;
    writeU64BE(out + 1, timestampNs);
    writeAsciiDecimal(out + 9, 14, newOrderToken);
    out[23] = (side == Side::Buy) ? 'B' : 'S';
    writeU32BE(out + 24, static_cast<uint32_t>(shares));
    writeAsciiDecimal(out + 28, 8, static_cast<uint64_t>(stock));
    writeU32BE(out + 36, static_cast<uint32_t>(price));
    writeU32BE(out + 40, timeInForce);
    writeAsciiDecimal(out + 44, 4, static_cast<uint64_t>(firm));
    out[48] = display;
    writeU64BE(out + 49, orderReferenceNumber);
    out[57] = 'O';   // capacity
    out[58] = 'N';   // intermarket sweep
    writeU32BE(out + 59, static_cast<uint32_t>(minQuantity));
    out[63] = 'N';   // cross type
    out[64] = 'L';   // order state: live
    writeAsciiDecimal(out + 65, 14, previousOrderToken);
    return OUCH_SIZE_ORDER_REPLACED;
}

// Convenience: TIF wire value → engine TIF + OrderType. OUCH conflates
// IOC into the TIF field; the engine separates them.
struct OuchTifDecoded {
    TimeInForce tif;
    OrderType   orderType;
};

inline OuchTifDecoded ouchDecodeTif(uint32_t wireTif) {
    if (wireTif == OUCH_TIF_IOC) {
        return {TimeInForce::GTC, OrderType::IOC};
    }
    if (wireTif == OUCH_TIF_DAY || wireTif == OUCH_TIF_DAY_SYS) {
        return {TimeInForce::DAY, OrderType::Limit};
    }
    return {TimeInForce::GTC, OrderType::Limit};
}

}  // namespace OrderMatcher
