#pragma once

// ConfigValidator — startup config validation with fail-fast semantics (P3-7).
//
// A production venue must NEVER start in a silently-degraded state. This module
// validates the engine's declared instruments, participants and journal path
// against hard invariants, and on ANY failure the caller prints every specific
// problem and exit(1)s. There is no partial / best-effort start.
//
// Config schema (key=value; see Config.h for the file/env format):
//
//   instruments = 1,2,3                      # comma-separated SymbolIds
//   instrument.<id>.min_price  = <int64>     # fixed-point (PRICE_PRECISION)
//   instrument.<id>.max_price  = <int64>
//   instrument.<id>.tick_size  = <int64>     # must be > 0
//   instrument.<id>.lot_size   = <int64>     # must be > 0
//
//   participants = 10,20                     # comma-separated ParticipantIds
//   participant.<id>.position_limit = <int64>   # must be set AND > 0
//   participant.<id>.otr_limit      = <double>  # must be set AND > 0
//
//   journal_path = /var/lib/orderengine/journal.wal   # parent dir must be writable
//
// Validated invariants:
//   * per instrument: min_price < max_price, tick_size > 0, lot_size > 0,
//     and (max_price - min_price) < FlatPriceMap capacity (band fits the map).
//   * per participant: BOTH position_limit and otr_limit present and > 0.
//   * journal_path (if set): its parent directory is writable.

#include "Config.h"
#include "FlatPriceMap.h"
#include "Types.h"

#include <cstdint>
#include <string>
#include <unistd.h>   // access(), W_OK
#include <vector>

namespace OrderMatcher {

struct ConfigValidationResult {
    bool ok = true;
    std::vector<std::string> errors;

    void fail(const std::string& msg) {
        ok = false;
        errors.push_back(msg);
    }
};

namespace detail {

// Split a comma-separated list, trimming whitespace around each element and
// dropping empty tokens. Small and dependency-free on purpose.
inline std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t comma = s.find(',', i);
        if (comma == std::string::npos) comma = s.size();
        size_t a = i, b = comma;
        while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
        if (b > a) out.push_back(s.substr(a, b - a));
        i = comma + 1;
    }
    return out;
}

// The directory that would contain `path`. "" (no slash) maps to "." (cwd).
inline std::string parentDir(const std::string& path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

}  // namespace detail

// True iff a journal file at `path` could be created/appended — i.e. its parent
// directory exists and is writable. Does NOT create the file.
inline bool isJournalPathWritable(const std::string& path) {
    if (path.empty()) return false;
    const std::string dir = detail::parentDir(path);
    return ::access(dir.c_str(), W_OK) == 0;
}

// Validate startup config. Pure — returns every problem found (does not exit),
// so the caller can print them all at once. `priceMapCapacity` defaults to the
// engine's real FlatPriceMap band width; overridable for tests.
inline ConfigValidationResult validateStartupConfig(
    const Config& cfg,
    size_t priceMapCapacity = FlatPriceMap::DEFAULT_CAPACITY) {
    ConfigValidationResult result;

    // ── Instruments ──────────────────────────────────────────────────────
    for (const std::string& idStr : detail::splitCsv(cfg.getString("instruments", ""))) {
        const std::string prefix = "instrument." + idStr + ".";
        const std::string label = "instrument " + idStr;

        auto requireKey = [&](const char* field) -> bool {
            if (!cfg.has(prefix + field)) {
                result.fail(label + ": missing required field '" + field + "'");
                return false;
            }
            return true;
        };

        const bool hasMin  = requireKey("min_price");
        const bool hasMax  = requireKey("max_price");
        const bool hasTick = requireKey("tick_size");
        const bool hasLot  = requireKey("lot_size");
        if (!(hasMin && hasMax && hasTick && hasLot)) continue;

        const int64_t minPrice = cfg.getInt64(prefix + "min_price");
        const int64_t maxPrice = cfg.getInt64(prefix + "max_price");
        const int64_t tick     = cfg.getInt64(prefix + "tick_size");
        const int64_t lot      = cfg.getInt64(prefix + "lot_size");

        if (minPrice >= maxPrice) {
            result.fail(label + ": min_price (" + std::to_string(minPrice) +
                        ") must be < max_price (" + std::to_string(maxPrice) + ")");
        }
        if (tick <= 0) {
            result.fail(label + ": tick_size (" + std::to_string(tick) +
                        ") must be > 0");
        }
        if (lot <= 0) {
            result.fail(label + ": lot_size (" + std::to_string(lot) +
                        ") must be > 0");
        }
        if (maxPrice > minPrice) {
            const uint64_t span = static_cast<uint64_t>(maxPrice) -
                                  static_cast<uint64_t>(minPrice);
            if (span >= static_cast<uint64_t>(priceMapCapacity)) {
                result.fail(label + ": price range span (" + std::to_string(span) +
                            ") does not fit FlatPriceMap capacity (" +
                            std::to_string(priceMapCapacity) + ")");
            }
        }
    }

    // ── Participants ─────────────────────────────────────────────────────
    for (const std::string& idStr : detail::splitCsv(cfg.getString("participants", ""))) {
        const std::string prefix = "participant." + idStr + ".";
        const std::string label = "participant " + idStr;

        if (!cfg.has(prefix + "position_limit")) {
            result.fail(label + ": position_limit not set");
        } else if (cfg.getInt64(prefix + "position_limit") <= 0) {
            result.fail(label + ": position_limit (" +
                        std::to_string(cfg.getInt64(prefix + "position_limit")) +
                        ") must be > 0");
        }

        if (!cfg.has(prefix + "otr_limit")) {
            result.fail(label + ": otr_limit not set");
        } else if (cfg.getDouble(prefix + "otr_limit") <= 0.0) {
            result.fail(label + ": otr_limit must be > 0");
        }
    }

    // ── Journal path ─────────────────────────────────────────────────────
    if (cfg.has("journal_path")) {
        const std::string jpath = cfg.getString("journal_path");
        if (!isJournalPathWritable(jpath)) {
            result.fail("journal_path '" + jpath +
                        "': parent directory does not exist or is not writable");
        }
    }

    return result;
}

}  // namespace OrderMatcher
