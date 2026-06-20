#pragma once

// ResearchSerializer.h — header-only serialization utilities for research output.
//
// Converts history/event vectors to CSV or JSONL strings.
// No file I/O — the caller is responsible for writing or streaming the result.

#include "SimulationDriver.h"
#include "MicrostructureMetrics.h"
#include "ExecutionAnalytics.h"
#include "Types.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace OrderMatcher {

namespace detail {

// Format a double with 6 decimal places into a string.
inline std::string fmtDouble(double v) {
    std::ostringstream oss;
    oss.precision(6);
    oss << std::fixed << v;
    return oss.str();
}

// Return "Buy" or "Sell" string for a Side value.
inline std::string sideStr(Side s) {
    return (s == Side::Buy) ? "Buy" : "Sell";
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// CSV serialization of SimulationDriver::TickSnapshot history
// ─────────────────────────────────────────────────────────────────────────────
// Columns: tick,bestBid,bestAsk,lastTradePrice,bidDepth,askDepth,volume,makerNetPosition

inline std::string tickHistoryToCsv(
        const std::vector<SimulationDriver::TickSnapshot>& history) {
    std::string out;
    out.reserve(history.size() * 80 + 80);

    // Header
    out += "tick,bestBid,bestAsk,lastTradePrice,bidDepth,askDepth,volume,makerNetPosition";

    for (const auto& s : history) {
        out += '\n';
        out += std::to_string(s.tick);
        out += ',';
        out += std::to_string(s.bestBid);
        out += ',';
        out += std::to_string(s.bestAsk);
        out += ',';
        out += std::to_string(s.lastTradePrice);
        out += ',';
        out += std::to_string(s.bidDepth);
        out += ',';
        out += std::to_string(s.askDepth);
        out += ',';
        out += std::to_string(s.volume);
        out += ',';
        out += std::to_string(s.makerNetPosition);
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// JSONL serialization of SimulationDriver::TickSnapshot history
// ─────────────────────────────────────────────────────────────────────────────
// One JSON object per line. No trailing newline after the last line.

inline std::string tickHistoryToJsonl(
        const std::vector<SimulationDriver::TickSnapshot>& history) {
    std::string out;
    out.reserve(history.size() * 120);

    for (std::size_t i = 0; i < history.size(); ++i) {
        const auto& s = history[i];
        if (i > 0) out += '\n';

        out += "{\"tick\":";
        out += std::to_string(s.tick);
        out += ",\"bestBid\":";
        out += std::to_string(s.bestBid);
        out += ",\"bestAsk\":";
        out += std::to_string(s.bestAsk);
        out += ",\"lastTradePrice\":";
        out += std::to_string(s.lastTradePrice);
        out += ",\"bidDepth\":";
        out += std::to_string(s.bidDepth);
        out += ",\"askDepth\":";
        out += std::to_string(s.askDepth);
        out += ",\"volume\":";
        out += std::to_string(s.volume);
        out += ",\"makerNetPosition\":";
        out += std::to_string(s.makerNetPosition);
        out += '}';
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// CSV serialization of MicrostructureMetrics::TradeEvent buffer
// ─────────────────────────────────────────────────────────────────────────────
// Columns: seqNum,price,qty,side,midpoint

inline std::string tradeEventsToCsv(
        const std::vector<MicrostructureMetrics::TradeEvent>& events) {
    std::string out;
    out.reserve(events.size() * 60 + 40);

    out += "seqNum,price,qty,side,midpoint";

    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& e = events[i];
        out += '\n';
        out += std::to_string(i);           // seqNum = index in the vector
        out += ',';
        out += std::to_string(e.price);
        out += ',';
        out += std::to_string(e.qty);
        out += ',';
        out += detail::sideStr(e.side);
        out += ',';
        out += std::to_string(e.midpoint);
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// CSV serialization of ExecutionAnalytics::FillRecord vector
// ─────────────────────────────────────────────────────────────────────────────
// Columns: seqNum,price,qty

inline std::string fillRecordsToCsv(
        const std::vector<FillRecord>& fills) {
    std::string out;
    out.reserve(fills.size() * 50 + 20);

    out += "seqNum,price,qty";

    for (const auto& f : fills) {
        out += '\n';
        out += std::to_string(f.seqNum);
        out += ',';
        out += detail::fmtDouble(f.price);
        out += ',';
        out += detail::fmtDouble(f.qty);
    }

    return out;
}

} // namespace OrderMatcher
