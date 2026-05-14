#pragma once

// ItchProtocol — NASDAQ ITCH 5.0-style market-data binary codec.
//
// ITCH is the public market-data feed companion to OUCH order entry:
// unidirectional, venue → subscribers, sequenced, big-endian, fixed-
// width. Real deployments distribute over UDP multicast with A/B feed
// redundancy and gap recovery via a separate retransmission service;
// this codec emits the wire frames only — transport is the caller's
// choice (UDP, SHM, TCP fan-out).
//
// Subset implemented (the messages a passive subscriber needs to
// reconstruct the order book):
//   'S'  SystemEvent      12 bytes  (open/close transitions)
//   'A'  AddOrder         36 bytes  (new order added to book)
//   'E'  OrderExecuted    31 bytes  (existing order matched at its limit)
//   'C'  OrderExecutedWithPrice 36 bytes  (matched at a different price)
//   'X'  OrderCancel      23 bytes  (partial cancel — shares reduced)
//   'D'  OrderDelete      19 bytes  (full delete — order removed)
//   'P'  Trade            44 bytes  (non-display trade / cross)
//
// Wire conventions:
//   * Numeric fields big-endian.
//   * Timestamp is 48-bit nanoseconds since midnight (BE).
//   * Stock is 8-byte ASCII, left-justified, space-padded. We encode
//     SymbolId as decimal in the slot (parallel to OUCH).
//   * StockLocate (2B) is the exchange-internal symbol index. We use
//     the SymbolId directly (truncated to 16 bits) — fine up to 64k
//     symbols; production venues maintain a separate locate-code
//     table seeded by the Stock Directory ('R') message.

#include "OuchProtocol.h"  // reuses writeU16BE/U32BE/U64BE + writeAsciiDecimal
#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace OrderMatcher {

constexpr uint8_t ITCH_MT_SYSTEM_EVENT        = 'S';
constexpr uint8_t ITCH_MT_STOCK_DIRECTORY     = 'R';
constexpr uint8_t ITCH_MT_TRADING_ACTION      = 'H';
constexpr uint8_t ITCH_MT_ADD_ORDER           = 'A';
constexpr uint8_t ITCH_MT_ORDER_EXECUTED      = 'E';
constexpr uint8_t ITCH_MT_ORDER_EXECUTED_PX   = 'C';
constexpr uint8_t ITCH_MT_ORDER_CANCEL        = 'X';
constexpr uint8_t ITCH_MT_ORDER_DELETE        = 'D';
constexpr uint8_t ITCH_MT_TRADE               = 'P';
constexpr uint8_t ITCH_MT_CROSS_TRADE         = 'Q';

constexpr size_t ITCH_SIZE_SYSTEM_EVENT       = 12;
constexpr size_t ITCH_SIZE_STOCK_DIRECTORY    = 27;
constexpr size_t ITCH_SIZE_TRADING_ACTION     = 25;
constexpr size_t ITCH_SIZE_ADD_ORDER          = 36;
constexpr size_t ITCH_SIZE_ORDER_EXECUTED     = 31;
constexpr size_t ITCH_SIZE_ORDER_EXECUTED_PX  = 36;
constexpr size_t ITCH_SIZE_ORDER_CANCEL       = 23;
constexpr size_t ITCH_SIZE_ORDER_DELETE       = 19;
constexpr size_t ITCH_SIZE_TRADE              = 44;
constexpr size_t ITCH_SIZE_CROSS_TRADE        = 40;

// Trading-action state codes per ITCH 5.0:
constexpr char ITCH_TRADING_STATE_HALTED         = 'H';
constexpr char ITCH_TRADING_STATE_PAUSED         = 'P';
constexpr char ITCH_TRADING_STATE_QUOTATION_ONLY = 'Q';
constexpr char ITCH_TRADING_STATE_TRADING        = 'T';

// Cross-trade type codes:
constexpr char ITCH_CROSS_TYPE_OPENING  = 'O';
constexpr char ITCH_CROSS_TYPE_CLOSING  = 'C';
constexpr char ITCH_CROSS_TYPE_HALT     = 'H';
constexpr char ITCH_CROSS_TYPE_INTRADAY = 'I';

// System-event codes per ITCH 5.0.
constexpr char ITCH_SE_START_OF_MESSAGES   = 'O';
constexpr char ITCH_SE_START_OF_SYSTEM     = 'S';
constexpr char ITCH_SE_START_OF_MARKET     = 'Q';
constexpr char ITCH_SE_END_OF_MARKET       = 'M';
constexpr char ITCH_SE_END_OF_SYSTEM       = 'E';
constexpr char ITCH_SE_END_OF_MESSAGES     = 'C';

// 48-bit big-endian write — ITCH-specific. Takes the low 48 bits of v
// and writes them MSB-first to *p .. *(p+5). Used for nanoseconds-
// since-midnight timestamps; the bit budget is enough for ~78 hours.
inline void writeU48BE(uint8_t* p, uint64_t v) {
    p[0] = static_cast<uint8_t>(v >> 40);
    p[1] = static_cast<uint8_t>(v >> 32);
    p[2] = static_cast<uint8_t>(v >> 24);
    p[3] = static_cast<uint8_t>(v >> 16);
    p[4] = static_cast<uint8_t>(v >> 8);
    p[5] = static_cast<uint8_t>(v);
}

inline uint64_t readU48BE(const uint8_t* p) {
    return (uint64_t(p[0]) << 40) | (uint64_t(p[1]) << 32) |
           (uint64_t(p[2]) << 24) | (uint64_t(p[3]) << 16) |
           (uint64_t(p[4]) << 8)  |  uint64_t(p[5]);
}

// ─── Encoders ───────────────────────────────────────────────────────────────
// Caller provides a buffer of exactly the right size; each function
// returns the number of bytes written. Buffers are not zero-init'd
// because every byte is set by the encoder — explicit memsets would
// just waste cycles on the publish hot path.

inline size_t encodeSystemEvent(uint8_t* out, uint16_t stockLocate,
                                uint16_t trackingNumber, uint64_t timestampNs,
                                char eventCode) {
    out[0] = ITCH_MT_SYSTEM_EVENT;
    writeU16BE(out + 1, stockLocate);
    writeU16BE(out + 3, trackingNumber);
    writeU48BE(out + 5, timestampNs);
    out[11] = static_cast<uint8_t>(eventCode);
    return ITCH_SIZE_SYSTEM_EVENT;
}

inline size_t encodeAddOrder(uint8_t* out, uint16_t stockLocate,
                             uint16_t trackingNumber, uint64_t timestampNs,
                             uint64_t orderRefNumber, Side side,
                             Quantity shares, SymbolId stock, Price price) {
    out[0] = ITCH_MT_ADD_ORDER;
    writeU16BE(out + 1, stockLocate);
    writeU16BE(out + 3, trackingNumber);
    writeU48BE(out + 5, timestampNs);
    writeU64BE(out + 11, orderRefNumber);
    out[19] = (side == Side::Buy) ? 'B' : 'S';
    writeU32BE(out + 20, static_cast<uint32_t>(shares));
    writeAsciiDecimal(out + 24, 8, static_cast<uint64_t>(stock));
    writeU32BE(out + 32, static_cast<uint32_t>(price));
    return ITCH_SIZE_ADD_ORDER;
}

inline size_t encodeOrderExecuted(uint8_t* out, uint16_t stockLocate,
                                  uint16_t trackingNumber, uint64_t timestampNs,
                                  uint64_t orderRefNumber,
                                  Quantity executedShares, uint64_t matchNumber) {
    out[0] = ITCH_MT_ORDER_EXECUTED;
    writeU16BE(out + 1, stockLocate);
    writeU16BE(out + 3, trackingNumber);
    writeU48BE(out + 5, timestampNs);
    writeU64BE(out + 11, orderRefNumber);
    writeU32BE(out + 19, static_cast<uint32_t>(executedShares));
    writeU64BE(out + 23, matchNumber);
    return ITCH_SIZE_ORDER_EXECUTED;
}

// Emitted when a resting order fills at a price different from its
// limit (e.g. hidden / mid-peg orders). For plain price-time fills,
// use encodeOrderExecuted instead — the order's price is already
// known to subscribers from the prior 'A' message.
inline size_t encodeOrderExecutedWithPrice(
    uint8_t* out, uint16_t stockLocate, uint16_t trackingNumber,
    uint64_t timestampNs, uint64_t orderRefNumber,
    Quantity executedShares, uint64_t matchNumber,
    bool printable, Price executionPrice) {
    out[0] = ITCH_MT_ORDER_EXECUTED_PX;
    writeU16BE(out + 1, stockLocate);
    writeU16BE(out + 3, trackingNumber);
    writeU48BE(out + 5, timestampNs);
    writeU64BE(out + 11, orderRefNumber);
    writeU32BE(out + 19, static_cast<uint32_t>(executedShares));
    writeU64BE(out + 23, matchNumber);
    out[31] = printable ? 'Y' : 'N';
    writeU32BE(out + 32, static_cast<uint32_t>(executionPrice));
    return ITCH_SIZE_ORDER_EXECUTED_PX;
}

inline size_t encodeOrderCancel(uint8_t* out, uint16_t stockLocate,
                                uint16_t trackingNumber, uint64_t timestampNs,
                                uint64_t orderRefNumber,
                                Quantity canceledShares) {
    out[0] = ITCH_MT_ORDER_CANCEL;
    writeU16BE(out + 1, stockLocate);
    writeU16BE(out + 3, trackingNumber);
    writeU48BE(out + 5, timestampNs);
    writeU64BE(out + 11, orderRefNumber);
    writeU32BE(out + 19, static_cast<uint32_t>(canceledShares));
    return ITCH_SIZE_ORDER_CANCEL;
}

inline size_t encodeOrderDelete(uint8_t* out, uint16_t stockLocate,
                                uint16_t trackingNumber, uint64_t timestampNs,
                                uint64_t orderRefNumber) {
    out[0] = ITCH_MT_ORDER_DELETE;
    writeU16BE(out + 1, stockLocate);
    writeU16BE(out + 3, trackingNumber);
    writeU48BE(out + 5, timestampNs);
    writeU64BE(out + 11, orderRefNumber);
    return ITCH_SIZE_ORDER_DELETE;
}

inline size_t encodeTrade(uint8_t* out, uint16_t stockLocate,
                          uint16_t trackingNumber, uint64_t timestampNs,
                          uint64_t orderRefNumber, Side side,
                          Quantity shares, SymbolId stock,
                          Price price, uint64_t matchNumber) {
    out[0] = ITCH_MT_TRADE;
    writeU16BE(out + 1, stockLocate);
    writeU16BE(out + 3, trackingNumber);
    writeU48BE(out + 5, timestampNs);
    writeU64BE(out + 11, orderRefNumber);
    out[19] = (side == Side::Buy) ? 'B' : 'S';
    writeU32BE(out + 20, static_cast<uint32_t>(shares));
    writeAsciiDecimal(out + 24, 8, static_cast<uint64_t>(stock));
    writeU32BE(out + 32, static_cast<uint32_t>(price));
    writeU64BE(out + 36, matchNumber);
    return ITCH_SIZE_TRADE;
}

// Stock Directory ('R'): emitted at session start (before any 'A'
// orders) to announce each listed symbol to subscribers. 27 bytes:
//   0      'R'
//   1-2    StockLocate
//   3-4    TrackingNumber
//   5-10   Timestamp
//   11-18  Stock           (8B ASCII)
//   19     MarketCategory  ('N'=NYSE, 'Q'=NASDAQ, 'P'=ARCA, etc.)
//   20     FinancialStatusIndicator ('N'=Normal, 'D'=Deficient, etc.)
//   21-24  RoundLotSize    (BE u32 — usually 100)
//   25     RoundLotsOnly   ('Y' / 'N')
//   26     IssueClassification ('A'=ADR, 'C'=Common, etc.)
inline size_t encodeStockDirectory(uint8_t* out, uint16_t stockLocate,
                                   uint16_t trackingNumber,
                                   uint64_t timestampNs, SymbolId stock,
                                   char marketCategory = 'Q',
                                   char financialStatus = 'N',
                                   uint32_t roundLotSize = 100,
                                   char roundLotsOnly = 'N',
                                   char issueClassification = 'C') {
    out[0] = ITCH_MT_STOCK_DIRECTORY;
    writeU16BE(out + 1, stockLocate);
    writeU16BE(out + 3, trackingNumber);
    writeU48BE(out + 5, timestampNs);
    writeAsciiDecimal(out + 11, 8, static_cast<uint64_t>(stock));
    out[19] = static_cast<uint8_t>(marketCategory);
    out[20] = static_cast<uint8_t>(financialStatus);
    writeU32BE(out + 21, roundLotSize);
    out[25] = static_cast<uint8_t>(roundLotsOnly);
    out[26] = static_cast<uint8_t>(issueClassification);
    return ITCH_SIZE_STOCK_DIRECTORY;
}

// Trading Action ('H'): per-symbol halt / resume notification. The
// engine's TradingState changes drive these on the wire. 25 bytes:
//   0      'H'
//   1-2    StockLocate
//   3-4    TrackingNumber
//   5-10   Timestamp
//   11-18  Stock
//   19     TradingState ('H'=Halted, 'P'=Paused, 'Q'=Quotation-only, 'T'=Trading)
//   20     Reserved
//   21-24  Reason       (4-byte ASCII reason code)
inline size_t encodeTradingAction(uint8_t* out, uint16_t stockLocate,
                                  uint16_t trackingNumber,
                                  uint64_t timestampNs, SymbolId stock,
                                  char tradingState,
                                  const char reason4[4] = "    ") {
    out[0] = ITCH_MT_TRADING_ACTION;
    writeU16BE(out + 1, stockLocate);
    writeU16BE(out + 3, trackingNumber);
    writeU48BE(out + 5, timestampNs);
    writeAsciiDecimal(out + 11, 8, static_cast<uint64_t>(stock));
    out[19] = static_cast<uint8_t>(tradingState);
    out[20] = ' ';
    for (int i = 0; i < 4; ++i) out[21 + i] = static_cast<uint8_t>(reason4[i]);
    return ITCH_SIZE_TRADING_ACTION;
}

// Cross Trade ('Q'): opening / closing / halt-resume cross prints.
// Distinct from continuous-matching 'P' (Trade) because the
// quantity is the aggregate uncross volume and the price is the
// discovered equilibrium price, not a per-fill price. 40 bytes:
//   0      'Q'
//   1-2    StockLocate
//   3-4    TrackingNumber
//   5-10   Timestamp
//   11-18  Shares          (BE u64 — auction prints can be huge)
//   19-26  Stock           (8B ASCII)
//   27-30  CrossPrice      (BE u32)
//   31-38  MatchNumber     (BE u64)
//   39     CrossType       ('O'=opening, 'C'=closing, 'H'=halt, 'I'=intraday)
inline size_t encodeCrossTrade(uint8_t* out, uint16_t stockLocate,
                               uint16_t trackingNumber,
                               uint64_t timestampNs,
                               uint64_t shares, SymbolId stock,
                               Price crossPrice, uint64_t matchNumber,
                               char crossType) {
    out[0] = ITCH_MT_CROSS_TRADE;
    writeU16BE(out + 1, stockLocate);
    writeU16BE(out + 3, trackingNumber);
    writeU48BE(out + 5, timestampNs);
    writeU64BE(out + 11, shares);
    writeAsciiDecimal(out + 19, 8, static_cast<uint64_t>(stock));
    writeU32BE(out + 27, static_cast<uint32_t>(crossPrice));
    writeU64BE(out + 31, matchNumber);
    out[39] = static_cast<uint8_t>(crossType);
    return ITCH_SIZE_CROSS_TRADE;
}

// Map engine TradingState → ITCH trading-state character. Centralized
// here so callers don't have to memorize the mapping.
inline char tradingStateToItch(TradingState s) {
    switch (s) {
    case TradingState::Continuous:    return ITCH_TRADING_STATE_TRADING;
    case TradingState::Halted:        return ITCH_TRADING_STATE_HALTED;
    case TradingState::AuctionOpen:
    case TradingState::AuctionClose:
    case TradingState::PreOpen:       return ITCH_TRADING_STATE_QUOTATION_ONLY;
    case TradingState::PostClose:     return ITCH_TRADING_STATE_HALTED;
    }
    return ITCH_TRADING_STATE_HALTED;
}

}  // namespace OrderMatcher
