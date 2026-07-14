#pragma once

// CatReporter — engine-agnostic Consolidated Audit Trail recorder. Captures the
// regulator-relevant facts of every order action (new/modify/cancel/trade) into
// an append-only, monotonically-sequenced log and serializes it to JSON Lines,
// in the spirit of US CAT / European audit-trail regimes.
//
// FORMAT/serialization only — not a network client, submission pipeline, or
// schema validator. No I/O beyond producing a string; an integration team owns
// transport, batching, encryption, and the official record layout.
//
// Design:
//   * Engine-agnostic: depends only on Types.h + stdlib (never OrderBook.h /
//     MatchingEngine.h), so it reuses in replay tools, gateways, post-trade.
//   * The reporter owns sequencing — every record gets the next value of a
//     monotonic counter (from 1) in call order; callers can't fabricate or
//     reorder. Downstream auditors rely on this.
//   * Timestamps are caller-supplied (the engine already has a clock), keeping
//     the reporter deterministic and testable.
//   * A Trade is ONE record: aggressor/buy id in orderId, resting/sell id in
//     contraOrderId (see recordTrade). Non-trade events leave contraOrderId and
//     tradeId at 0.
//   * Storage is an append-only std::vector<CatRecord> until clear().

#include "Types.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace OrderMatcher {

// The four order-lifecycle events the audit trail distinguishes.
enum class CatEventType : uint8_t {
    New,     // Order accepted into the book.
    Modify,  // Resting order quantity amended.
    Cancel,  // Order removed before full execution.
    Trade    // Two orders matched and executed.
};

// One immutable audit-trail row. Fixed layout so downstream parsers and
// the JSONL writer agree on exactly which fields exist.
struct CatRecord {
    uint64_t      sequence{0};       // Monotonic, assigned by the reporter from 1.
    uint64_t      timestampNs{0};    // Event time, caller-supplied (nanoseconds).
    CatEventType  type{CatEventType::New};
    OrderId       orderId{0};        // Trade: the buy (aggressor) order id.
    OrderId       contraOrderId{0};  // Trade only: the sell (contra) order id; 0 otherwise.
    ParticipantId participant{0};
    SymbolId      symbol{0};
    Side          side{Side::Buy};
    Price         price{0};
    Quantity      quantity{0};
    uint64_t      tradeId{0};        // Trade events only (0 otherwise).
};

class CatReporter {
public:
    CatReporter() = default;

    // Record a newly accepted order. Captures the full identity of the
    // order so later Modify/Cancel/Trade rows can be correlated by id.
    void recordNewOrder(uint64_t ts, OrderId orderId, ParticipantId participant,
                        SymbolId symbol, Side side, Price price, Quantity quantity) {
        CatRecord r;
        r.sequence    = nextSequence();
        r.timestampNs = ts;
        r.type        = CatEventType::New;
        r.orderId     = orderId;
        r.participant = participant;
        r.symbol      = symbol;
        r.side        = side;
        r.price       = price;
        r.quantity    = quantity;
        records_.push_back(r);
    }

    // Record a quantity amendment on an existing order. Only the id and
    // the new quantity are regulator-relevant here; price/side/participant
    // are recoverable from the correlated New record, so they stay at
    // their defaults rather than being guessed.
    void recordModify(uint64_t ts, OrderId orderId, Quantity newQty) {
        CatRecord r;
        r.sequence    = nextSequence();
        r.timestampNs = ts;
        r.type        = CatEventType::Modify;
        r.orderId     = orderId;
        r.quantity    = newQty;
        records_.push_back(r);
    }

    // Record a cancel. As with Modify, the id is the load-bearing fact;
    // the rest is correlated from the original New record.
    void recordCancel(uint64_t ts, OrderId orderId) {
        CatRecord r;
        r.sequence    = nextSequence();
        r.timestampNs = ts;
        r.type        = CatEventType::Cancel;
        r.orderId     = orderId;
        records_.push_back(r);
    }

    // Record an execution as a single Trade row. By convention the buy
    // (aggressor) order id lands in orderId and the sell (contra) order id
    // in contraOrderId, so both legs are auditable from one record. side
    // is reported as Buy to reflect that orderId is the buy side.
    void recordTrade(uint64_t ts, uint64_t tradeId, OrderId buyOrderId,
                    OrderId sellOrderId, SymbolId symbol, Price price,
                    Quantity quantity) {
        CatRecord r;
        r.sequence      = nextSequence();
        r.timestampNs   = ts;
        r.type          = CatEventType::Trade;
        r.orderId       = buyOrderId;
        r.contraOrderId = sellOrderId;
        r.symbol        = symbol;
        r.side          = Side::Buy;  // orderId is the buy leg by convention.
        r.price         = price;
        r.quantity      = quantity;
        r.tradeId       = tradeId;
        records_.push_back(r);
    }

    size_t recordCount() const { return records_.size(); }

    const std::vector<CatRecord>& records() const { return records_; }

    // Serialize the whole log to JSON Lines: one compact JSON object per
    // record, newline-separated (a trailing newline follows each line,
    // including the last — standard JSONL). `type` is rendered as a string
    // ("New"/"Modify"/"Cancel"/"Trade"); every numeric field is emitted so
    // a parser never has to infer a missing column. Side is rendered as
    // "Buy"/"Sell". The result is empty when there are no records.
    std::string toJsonl() const {
        std::string out;
        // Rough reservation: a populated record line is well under 256
        // bytes. Pre-sizing keeps reallocations modest on large logs.
        out.reserve(records_.size() * 192);

        char buf[512];
        for (const auto& r : records_) {
            const int n = std::snprintf(
                buf, sizeof(buf),
                "{\"sequence\":%llu,\"timestampNs\":%llu,\"type\":\"%s\","
                "\"orderId\":%llu,\"contraOrderId\":%llu,\"participant\":%llu,"
                "\"symbol\":%llu,\"side\":\"%s\",\"price\":%lld,"
                "\"quantity\":%llu,\"tradeId\":%llu}\n",
                static_cast<unsigned long long>(r.sequence),
                static_cast<unsigned long long>(r.timestampNs),
                typeName(r.type),
                static_cast<unsigned long long>(r.orderId),
                static_cast<unsigned long long>(r.contraOrderId),
                static_cast<unsigned long long>(r.participant),
                static_cast<unsigned long long>(r.symbol),
                sideName(r.side),
                static_cast<long long>(r.price),
                static_cast<unsigned long long>(r.quantity),
                static_cast<unsigned long long>(r.tradeId));
            // n is the would-be length; with the 512-byte buffer and the
            // fixed-width numeric maxima above it cannot overflow, but
            // clamp defensively so a future field addition fails loud in
            // tests rather than silently truncating mid-object.
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n < static_cast<int>(sizeof(buf))
                                                         ? n
                                                         : sizeof(buf) - 1));
            }
        }
        return out;
    }

    // Reset to the empty state: drop all records AND rewind the sequence
    // counter so a fresh session restarts at 1. (A reporter is reused
    // across trading sessions / test cases; clear() must make it
    // indistinguishable from a freshly constructed one.)
    void clear() {
        records_.clear();
        nextSequence_ = 1;
    }

    // String form of an event type, used by toJsonl and reusable by
    // callers building their own views.
    static const char* typeName(CatEventType t) {
        switch (t) {
        case CatEventType::New:    return "New";
        case CatEventType::Modify: return "Modify";
        case CatEventType::Cancel: return "Cancel";
        case CatEventType::Trade:  return "Trade";
        }
        return "Unknown";  // Unreachable for the closed enum; keeps the
                           // compiler from warning on a non-exhaustive path.
    }

    static const char* sideName(Side s) {
        return s == Side::Buy ? "Buy" : "Sell";
    }

private:
    // Hand out the next sequence number and advance. Centralized so every
    // record() path shares one counter — the single source of ordering.
    uint64_t nextSequence() { return nextSequence_++; }

    std::vector<CatRecord> records_;
    uint64_t               nextSequence_{1};  // First record is sequence 1.
};

}  // namespace OrderMatcher
