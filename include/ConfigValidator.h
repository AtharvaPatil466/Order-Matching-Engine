#pragma once

// ConfigValidator — startup config validation with fail-fast semantics (P3-7).
//
// A production venue must NEVER start in a silently-degraded state. This module
// checks what OrderEngine applies from its config, and on ANY failure the
// caller prints every specific problem and exit(1)s. There is no partial /
// best-effort start, and nothing is validated that the engine then ignores
// (SURF-4: tick, lot and OTR keys were certified and never applied).
//
// Config schema (key=value; see Config.h for the file/env format):
//
//   instrument.<s>.min_price  = <int64>     # symbol s's band, in ticks of 0.0001;
//   instrument.<s>.max_price  = <int64>     # every s in 0 .. --symbols-1
//
//   participants = 10,20                    # comma-separated ParticipantIds
//   participant.<id>.position_limit = <int64>   # must be set AND > 0
//
//   worker_cores = 2,3,4,5                  # validateWorkerCores(), below
//
// Validated invariants:
//   * per symbol: both band keys, unless the band is waived (--no-price-band);
//     0 < min_price < max_price; and the band fits a book's price map. Every
//     book has a fixed tick of 0.0001 over FlatPriceMap::DEFAULT_CAPACITY
//     (200,001) levels, so the widest band is 200,000 ticks = 20.0000.
//     instrument.<s>.tick_size / lot_size are refused: nothing applies them.
//   * participants: non-empty unless risk limits are waived (--no-risk-limits);
//     each id below kMaxParticipants with a position_limit > 0.
//     participant.<id>.otr_limit is refused: the engine's OTR throttle is one
//     engine-wide setting, not per participant, and the file does not set it.

#include "Config.h"
#include "FlatPriceMap.h"
#include "Types.h"
#include "Utils.h"

#include <algorithm>
#include <charconv>
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

// Controls OrderEngine may start without, each only when waived by name. A
// waiver allows the control to be absent; one that is configured still applies.
struct ControlWaivers {
    bool priceBand = false;   // --no-price-band / OB_NO_PRICE_BAND=1
    bool riskLimits = false;  // --no-risk-limits / OB_NO_RISK_LIMITS=1
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

inline void validateBand(const Config& cfg, size_t symbol, bool waived, size_t capacity,
                         ConfigValidationResult& result) {
    const std::string prefix = "instrument." + std::to_string(symbol) + ".";
    const std::string label = "symbol " + std::to_string(symbol);
    for (const char* unapplied : {"tick_size", "lot_size"}) {
        if (cfg.has(prefix + unapplied)) {
            result.fail(label + ": " + prefix + unapplied + " is not applied: every book "
                        "has a fixed tick of 0.0001 and no lot size; remove the key");
        }
    }
    const bool hasMin = cfg.has(prefix + "min_price");
    const bool hasMax = cfg.has(prefix + "max_price");
    if (!hasMin && !hasMax) {
        if (!waived) {
            result.fail(label + " has no price band: set " + prefix + "min_price and " +
                        prefix + "max_price, or pass --no-price-band (OB_NO_PRICE_BAND=1) to "
                        "let its first resting order centre the band");
        }
        return;
    }
    if (!hasMin || !hasMax) {
        result.fail(label + ": set both " + prefix + "min_price and " + prefix + "max_price");
        return;
    }
    const int64_t minPrice = cfg.getInt64(prefix + "min_price");
    const int64_t maxPrice = cfg.getInt64(prefix + "max_price");
    if (minPrice <= 0 || minPrice >= maxPrice) {
        result.fail(label + ": need 0 < min_price (" + std::to_string(minPrice) +
                    ") < max_price (" + std::to_string(maxPrice) + ")");
        return;
    }
    const uint64_t span = static_cast<uint64_t>(maxPrice) - static_cast<uint64_t>(minPrice);
    if (span >= static_cast<uint64_t>(capacity)) {
        result.fail(label + ": band [" + std::to_string(minPrice) + ", " +
                    std::to_string(maxPrice) + "] is " + std::to_string(span + 1) +
                    " price levels; a book holds " + std::to_string(capacity) +
                    " levels at a fixed tick of 0.0001 (FlatPriceMap), so the widest band is " +
                    std::to_string(capacity - 1) + " ticks. Narrow the band; per-instrument "
                    "tick sizes are not supported");
    }
}

}  // namespace detail

// True iff a journal file at `path` could be created/appended — i.e. its parent
// directory exists and is writable. Does NOT create the file.
inline bool isJournalPathWritable(const std::string& path) {
    if (path.empty()) return false;
    const auto slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
    return ::access(dir.c_str(), W_OK) == 0;
}

// The ids `participants` names; each bad entry is reported in `result`.
inline std::vector<ParticipantId> configuredParticipants(const Config& cfg,
                                                         ConfigValidationResult& result) {
    std::vector<ParticipantId> ids;
    for (const std::string& tok : detail::splitCsv(cfg.getString("participants", ""))) {
        uint32_t id = 0;
        const auto [end, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), id);
        if (ec != std::errc() || end != tok.data() + tok.size() || id >= kMaxParticipants) {
            result.fail("participants: '" + tok + "' is not a participant id below " +
                        std::to_string(kMaxParticipants));
            continue;
        }
        ids.push_back(static_cast<ParticipantId>(id));
    }
    return ids;
}

// Validate startup config for an engine of `numSymbols` books. Pure — returns
// every problem found (does not exit), so the caller can print them all at
// once. `priceMapCapacity` defaults to the engine's real FlatPriceMap size;
// overridable for tests.
inline ConfigValidationResult validateStartupConfig(
    const Config& cfg, size_t numSymbols, ControlWaivers waivers,
    size_t priceMapCapacity = FlatPriceMap::DEFAULT_CAPACITY) {
    ConfigValidationResult result;

    for (size_t s = 0; s < numSymbols; ++s) {
        detail::validateBand(cfg, s, waivers.priceBand, priceMapCapacity, result);
    }

    const size_t errorsBefore = result.errors.size();
    const std::vector<ParticipantId> participants = configuredParticipants(cfg, result);
    if (participants.empty() && result.errors.size() == errorsBefore && !waivers.riskLimits) {
        result.fail("no per-participant risk limits: set participants = <id,...> and "
                    "participant.<id>.position_limit for each, or pass --no-risk-limits "
                    "(OB_NO_RISK_LIMITS=1) to trade without position limits");
    }
    for (const ParticipantId id : participants) {
        const std::string prefix = "participant." + std::to_string(id) + ".";
        const std::string label = "participant " + std::to_string(id);
        if (!cfg.has(prefix + "position_limit")) {
            result.fail(label + ": position_limit not set");
        } else if (cfg.getInt64(prefix + "position_limit") <= 0) {
            result.fail(label + ": position_limit (" +
                        std::to_string(cfg.getInt64(prefix + "position_limit")) +
                        ") must be > 0");
        }
        if (cfg.has(prefix + "otr_limit")) {
            result.fail(label + ": otr_limit is not applied: the order-to-trade throttle "
                        "is engine-wide, not per participant; remove the key");
        }
    }
    return result;
}

// Every participant the order port will serve as must have a position limit,
// unless risk limits are waived. `credentialed` is ParticipantAuth's ids.
inline void validateRiskCoverage(const Config& cfg,
                                 const std::vector<ParticipantId>& credentialed,
                                 bool waived, ConfigValidationResult& result) {
    if (waived) return;
    ConfigValidationResult ignored;  // validateStartupConfig reported these
    const std::vector<ParticipantId> limited = configuredParticipants(cfg, ignored);
    for (const ParticipantId id : credentialed) {
        if (std::find(limited.begin(), limited.end(), id) == limited.end()) {
            result.fail("participant " + std::to_string(id) + " can trade (credentials) but "
                        "has no position limit: add it to participants and set participant." +
                        std::to_string(id) + ".position_limit, or pass --no-risk-limits "
                        "(OB_NO_RISK_LIMITS=1)");
        }
    }
}

// worker_cores = 2,3,4,5 (OB_WORKER_CORES): the CPU each async worker pins
// itself to, in worker order, numbered as `taskset -c` numbers them. Without it
// worker i takes the i-th CPU the process may run on. Never ignored: unless
// every entry is a CPU number in `allowed` (Utils::allowedCpus(); empty where
// there is no affinity mask, so any list fails there), none repeats, and there
// is one per worker, it is a startup error. Returns the list; empty when the
// key is unset or wrong.
inline std::vector<int> validateWorkerCores(const Config& cfg, size_t workers,
                                            const std::vector<int>& allowed,
                                            ConfigValidationResult& result) {
    if (!cfg.has("worker_cores")) return {};
    const std::string spec = cfg.getString("worker_cores");
    std::vector<int> cores;
    for (const std::string& tok : detail::splitCsv(spec)) {
        int cpu = -1;
        const auto [end, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), cpu);
        if (ec != std::errc() || end != tok.data() + tok.size() || cpu < 0) {
            result.fail("worker_cores '" + spec + "': '" + tok + "' is not a CPU number");
            return {};
        }
        cores.push_back(cpu);
    }
    if (const std::string err = Utils::workerCoresError(cores, workers, allowed); !err.empty()) {
        result.fail("worker_cores '" + spec + "': " + err);
        return {};
    }
    return cores;
}

}  // namespace OrderMatcher
