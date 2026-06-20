#pragma once

// Config — dependency-free configuration management.
//
// Loads settings from a simple key=value file (# comments, blank lines ignored).
// Every key can be overridden by an environment variable with the prefix OB_
// (e.g. key "threads" is overridden by OB_THREADS).
//
// Usage:
//   Config cfg;
//   cfg.loadFile("/etc/orderengine/engine.conf");
//   int threads = cfg.getInt("threads", 4);
//   std::string journal = cfg.getString("journal_path", "/var/lib/orderengine/journal");

#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OrderMatcher {

class Config {
public:
    Config() = default;

    // Load from a key=value file. Returns false if the file can't be opened.
    // Can be called multiple times; later loads override earlier keys.
    bool loadFile(const std::string& path) {
        std::ifstream in(path);
        if (!in.is_open()) return false;

        std::vector<std::pair<std::string, std::string>> changed;
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::string line;
            int lineNum = 0;
            while (std::getline(in, line)) {
                ++lineNum;
                // Strip leading/trailing whitespace
                auto start = line.find_first_not_of(" \t");
                if (start == std::string::npos) continue;
                line = line.substr(start);

                // Skip comments and blank lines
                if (line.empty() || line[0] == '#') continue;

                auto eq = line.find('=');
                if (eq == std::string::npos) {
                    std::cerr << "[Config] WARNING: no '=' on line " << lineNum
                              << " of " << path << ": " << line << "\n";
                    continue;
                }

                std::string key = trim(line.substr(0, eq));
                std::string val = trim(line.substr(eq + 1));

                // Strip optional quotes around value
                if (val.size() >= 2 &&
                    ((val.front() == '"' && val.back() == '"') ||
                     (val.front() == '\'' && val.back() == '\''))) {
                    val = val.substr(1, val.size() - 2);
                }

                store_[key] = val;
                changed.emplace_back(key, val);
            }
        }
        // Fire listener outside the lock so callbacks can call back into
        // Config (e.g. has()/getString()) without self-deadlocking.
        if (listener_) {
            for (auto& [k, v] : changed) listener_(k, v);
        }
        return true;
    }

    // Load from a map (for testing or programmatic config).
    void loadMap(const std::unordered_map<std::string, std::string>& m) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [k, v] : m) {
                store_[k] = v;
            }
        }
        if (listener_) {
            for (auto& [k, v] : m) listener_(k, v);
        }
    }

    // Set a single key.
    void set(const std::string& key, const std::string& value) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            store_[key] = value;
        }
        if (listener_) listener_(key, value);
    }

    // ─── Getters (env var override → file value → default) ──────────────

    std::string getString(const std::string& key,
                          const std::string& defaultVal = "") const {
        // 1. Check environment: OB_<UPPER_KEY>
        std::string envKey = envName(key);
        const char* env = std::getenv(envKey.c_str());
        if (env) return env;

        // 2. Check stored config
        std::lock_guard<std::mutex> lock(mu_);
        auto it = store_.find(key);
        if (it != store_.end()) return it->second;

        return defaultVal;
    }

    int getInt(const std::string& key, int defaultVal = 0) const {
        std::string s = getString(key, "");
        if (s.empty()) return defaultVal;
        try { return std::stoi(s); }
        catch (...) { return defaultVal; }
    }

    int64_t getInt64(const std::string& key, int64_t defaultVal = 0) const {
        std::string s = getString(key, "");
        if (s.empty()) return defaultVal;
        try { return std::stoll(s); }
        catch (...) { return defaultVal; }
    }

    double getDouble(const std::string& key, double defaultVal = 0.0) const {
        std::string s = getString(key, "");
        if (s.empty()) return defaultVal;
        try { return std::stod(s); }
        catch (...) { return defaultVal; }
    }

    bool getBool(const std::string& key, bool defaultVal = false) const {
        std::string s = getString(key, "");
        if (s.empty()) return defaultVal;
        // Accept: true/1/yes/on (case-insensitive)
        for (auto& c : s) c = static_cast<char>(std::tolower(c));
        return s == "true" || s == "1" || s == "yes" || s == "on";
    }

    // Check if a key exists (in env or file).
    bool has(const std::string& key) const {
        std::string envKey = envName(key);
        if (std::getenv(envKey.c_str())) return true;
        std::lock_guard<std::mutex> lock(mu_);
        return store_.count(key) > 0;
    }

    // Dump all keys for debugging.
    std::string dump() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::ostringstream out;
        for (auto& [k, v] : store_) {
            out << k << "=" << v << "\n";
        }
        return out.str();
    }

    // Register a listener for config changes (called on loadFile/loadMap/set).
    using Listener = std::function<void(const std::string& key, const std::string& value)>;
    void setListener(Listener fn) { listener_ = std::move(fn); }

private:
    static std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    static std::string envName(const std::string& key) {
        std::string env = "OB_";
        for (char c : key) {
            if (c == '.' || c == '-') env += '_';
            else env += static_cast<char>(std::toupper(c));
        }
        return env;
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> store_;
    Listener listener_;
};

// ── Cross-DC / WAN replication tuning ─────────────────────────────────────
// OB_REPLICATION_ASYNC_MODE     0|1    (default: 0)
//   When 1, replicateEntry() returns immediately without waiting for the
//   remote write to complete. Use for cross-DC where synchronous ack would
//   add RTT to the matching hot path. Accept the corresponding RPO window.
//
// OB_REPLICATION_WAN_BUFFER_KB  int    (default: 256)
//   TCP SO_SNDBUF size in kilobytes for the replication socket. 256 KB
//   is suitable for same-rack; increase to 4096+ for cross-DC to absorb
//   BDP (bandwidth-delay product) on WAN links.
//
// OB_REPLICATION_ACK_TIMEOUT_MS int    (default: 500)
//   How long replicateEntry() waits for a TCP send to complete (via
//   SO_SNDTIMEO) before declaring the peer dead. 500ms is correct for
//   same-rack; for WAN set to 3000+ to tolerate jitter without false
//   failovers.
//
// OB_REPLICATION_HEARTBEAT_MS   int    (default: 100)
//   Interval between primary→backup heartbeat messages. Lower values
//   reduce RTO but increase replication bandwidth.
//
// OB_REPLICATION_TIMEOUT_MS     int    (default: 500)
//   Heartbeat miss threshold after which backup considers primary dead.
//   Must be > OB_REPLICATION_HEARTBEAT_MS * 2. For WAN, set to 2000+.

// Convenience getters for replication WAN config
inline bool replicationAsyncMode(const Config& cfg) {
    return cfg.getBool("OB_REPLICATION_ASYNC_MODE", false);
}
inline int replicationWanBufferKb(const Config& cfg) {
    return cfg.getInt("OB_REPLICATION_WAN_BUFFER_KB", 256);
}
inline int replicationAckTimeoutMs(const Config& cfg) {
    return cfg.getInt("OB_REPLICATION_ACK_TIMEOUT_MS", 500);
}
inline int replicationHeartbeatMs(const Config& cfg) {
    return cfg.getInt("OB_REPLICATION_HEARTBEAT_MS", 100);
}
inline int replicationTimeoutMs(const Config& cfg) {
    return cfg.getInt("OB_REPLICATION_TIMEOUT_MS", 500);
}

}  // namespace OrderMatcher
