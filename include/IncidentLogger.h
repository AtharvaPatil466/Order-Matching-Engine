#pragma once

// IncidentLogger — Reg SCI-compliant structured incident logging.
//
// Roadmap Phase 4, Week 15: Reg SCI Incident Logging
//
// Every halt, circuit break, or kill switch generates a structured log
// entry with: timestamp (nanosecond), level, symbol (if applicable),
// triggering condition, current book state summary, participant count,
// order count. Logged to a separate file from normal operations, rotated
// hourly. Format is newline-delimited JSON (NDJSON) for easy ingestion.

#include "Types.h"
#include "Utils.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace OrderMatcher {

enum class IncidentType : uint8_t {
    CircuitBreaker = 1,
    SymbolHalt     = 2,
    GlobalHalt     = 3,
    KillSwitch     = 4,
    LULDPause      = 5,
    LULDResume     = 6,
    STPTriggered   = 7,
    WashTradeFlagged = 8,
    CapacityAlert  = 9,
    ReplicationLag = 10
};

inline const char* incidentTypeStr(IncidentType t) {
    switch (t) {
        case IncidentType::CircuitBreaker:   return "circuit_breaker";
        case IncidentType::SymbolHalt:       return "symbol_halt";
        case IncidentType::GlobalHalt:       return "global_halt";
        case IncidentType::KillSwitch:       return "kill_switch";
        case IncidentType::LULDPause:        return "luld_pause";
        case IncidentType::LULDResume:       return "luld_resume";
        case IncidentType::STPTriggered:     return "stp_triggered";
        case IncidentType::WashTradeFlagged: return "wash_trade_flagged";
        case IncidentType::CapacityAlert:    return "capacity_alert";
        case IncidentType::ReplicationLag:   return "replication_lag";
    }
    return "unknown";
}

struct IncidentRecord {
    uint64_t     timestampNs{0};
    IncidentType type{IncidentType::CircuitBreaker};
    SymbolId     symbolId{0};
    std::string  triggerCondition;
    Price        bestBid{0};
    Price        bestAsk{0};
    Price        referencePrice{0};
    uint32_t     participantCount{0};
    uint32_t     activeOrderCount{0};
    std::string  additionalInfo;
};

class IncidentLogger {
public:
    IncidentLogger() = default;

    ~IncidentLogger() {
        close();
    }

    IncidentLogger(const IncidentLogger&) = delete;
    IncidentLogger& operator=(const IncidentLogger&) = delete;

    // Open the incident log file. Returns false on failure.
    bool open(const std::string& path) {
        std::lock_guard<std::mutex> lock(mu_);
        if (fp_) {
            std::fclose(fp_);
        }
        fp_ = std::fopen(path.c_str(), "a");
        if (!fp_) return false;
        path_ = path;
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mu_);
        if (fp_) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    // Log an incident as NDJSON.
    void log(const IncidentRecord& record) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!fp_) return;

        // Build NDJSON line
        std::fprintf(fp_,
            "{\"timestamp_ns\":%llu,"
            "\"type\":\"%s\","
            "\"symbol_id\":%u,"
            "\"trigger\":\"%s\","
            "\"best_bid\":%lld,"
            "\"best_ask\":%lld,"
            "\"reference_price\":%lld,"
            "\"participant_count\":%u,"
            "\"active_order_count\":%u",
            static_cast<unsigned long long>(record.timestampNs),
            incidentTypeStr(record.type),
            record.symbolId,
            escapeJson(record.triggerCondition).c_str(),
            static_cast<long long>(record.bestBid),
            static_cast<long long>(record.bestAsk),
            static_cast<long long>(record.referencePrice),
            record.participantCount,
            record.activeOrderCount);

        if (!record.additionalInfo.empty()) {
            std::fprintf(fp_, ",\"info\":\"%s\"",
                         escapeJson(record.additionalInfo).c_str());
        }

        std::fprintf(fp_, "}\n");
        std::fflush(fp_);

        ++totalIncidents_;

        // Check if hourly rotation is needed
        maybeRotate();
    }

    // Convenience: log a circuit breaker incident
    void logCircuitBreaker(SymbolId symbol, Price triggerPrice,
                           Price refPrice, Price bestBid, Price bestAsk) {
        IncidentRecord r;
        r.timestampNs = nowNs();
        r.type = IncidentType::CircuitBreaker;
        r.symbolId = symbol;
        r.triggerCondition = "price_deviation_exceeded";
        r.bestBid = bestBid;
        r.bestAsk = bestAsk;
        r.referencePrice = refPrice;
        r.additionalInfo = "trigger_price=" + std::to_string(triggerPrice);
        log(r);
    }

    // Convenience: log a kill switch activation
    void logKillSwitch(const std::string& level, SymbolId symbol = 0) {
        IncidentRecord r;
        r.timestampNs = nowNs();
        r.type = IncidentType::KillSwitch;
        r.symbolId = symbol;
        r.triggerCondition = "kill_switch_" + level;
        log(r);
    }

    // Convenience: log LULD pause
    void logLULDPause(SymbolId symbol, Price refPrice,
                      Price bestBid, Price bestAsk) {
        IncidentRecord r;
        r.timestampNs = nowNs();
        r.type = IncidentType::LULDPause;
        r.symbolId = symbol;
        r.triggerCondition = "luld_band_breach";
        r.bestBid = bestBid;
        r.bestAsk = bestAsk;
        r.referencePrice = refPrice;
        log(r);
    }

    // Convenience: log STP triggered
    void logSTP(SymbolId symbol, ParticipantId participant,
                const std::string& mode) {
        IncidentRecord r;
        r.timestampNs = nowNs();
        r.type = IncidentType::STPTriggered;
        r.symbolId = symbol;
        r.triggerCondition = "self_trade_" + mode;
        r.additionalInfo = "participant=" + std::to_string(participant);
        log(r);
    }

    // Convenience: log capacity alert
    void logCapacityAlert(const std::string& resource, double usagePct) {
        IncidentRecord r;
        r.timestampNs = nowNs();
        r.type = IncidentType::CapacityAlert;
        r.triggerCondition = resource + "_exhaustion";
        r.additionalInfo = "usage_pct=" + std::to_string(usagePct);
        log(r);
    }

    uint64_t totalIncidents() const { return totalIncidents_; }
    bool isOpen() const { return fp_ != nullptr; }

private:
    static std::string escapeJson(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:   out += c;
            }
        }
        return out;
    }

    void maybeRotate() {
        // Simple hourly rotation: check if an hour has passed
        uint64_t now = nowNs();
        constexpr uint64_t ONE_HOUR_NS = 3600ULL * 1000000000ULL;
        if (lastRotationNs_ == 0) {
            lastRotationNs_ = now;
            return;
        }
        if (now - lastRotationNs_ < ONE_HOUR_NS) {
            return;
        }

        // Rotate: close current, open new with timestamp suffix
        if (fp_) {
            std::fclose(fp_);
        }
        std::string rotatedPath = path_ + "." + std::to_string(now);
        std::rename(path_.c_str(), rotatedPath.c_str());
        fp_ = std::fopen(path_.c_str(), "a");
        lastRotationNs_ = now;
    }

    std::mutex mu_;
    FILE* fp_{nullptr};
    std::string path_;
    uint64_t totalIncidents_{0};
    uint64_t lastRotationNs_{0};
};

}  // namespace OrderMatcher
