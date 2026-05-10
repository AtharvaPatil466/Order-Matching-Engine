#pragma once

// StructuredLog — minimal pluggable event sink for operational telemetry.
//
// What this is: a one-virtual-call API for emitting structured events
// (severity + name + key/value pairs). The codebase calls
// `obSink().log(...)` at notable moments; users plug in a backend
// (Prometheus exporter, OpenTelemetry, JSON-to-stderr, alerting glue)
// by calling setSink(...).
//
// What this is NOT: a logging framework. There is no formatter, no
// filtering DSL, no thread-local context, no log rotation. The point is
// to give the codebase ONE seam to emit events through, so an
// integration team can swap backends without rewriting call sites.
//
// Design choices:
//   * Default sink is NullSink — silent, near-zero cost (one virtual
//     call returning immediately). Production code paying for telemetry
//     must explicitly opt in.
//   * Events carry a small fixed-capacity vector of (string, string)
//     pairs. No allocation in the hot path beyond what std::string
//     might do. Backends can re-format as they like.
//   * Sink replacement is not thread-safe by itself; install your sink
//     before the engine starts. (Adding a shared_mutex around setSink
//     is trivial if needed; left out to keep the no-op path cheap.)

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OrderMatcher {

enum class LogSeverity : uint8_t { Info, Warn, Error };

struct LogEvent {
    LogSeverity severity{LogSeverity::Info};
    std::string_view name;  // stable string; caller owns
    std::vector<std::pair<std::string, std::string>> fields;

    LogEvent& kv(std::string_view key, std::string_view value) {
        fields.emplace_back(std::string(key), std::string(value));
        return *this;
    }
    LogEvent& kv(std::string_view key, long long value) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%lld", value);
        return kv(key, buf);
    }
    LogEvent& kv(std::string_view key, unsigned long long value) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%llu", value);
        return kv(key, buf);
    }
    LogEvent& kv(std::string_view key, double value) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%g", value);
        return kv(key, buf);
    }
};

class StructuredSink {
public:
    virtual ~StructuredSink() = default;
    virtual void log(const LogEvent& event) = 0;
};

// Silent default. Zero allocations, one indirect call.
class NullSink : public StructuredSink {
public:
    void log(const LogEvent&) override {}
};

// Reference dev backend: one JSON line per event on stderr.
class JsonStderrSink : public StructuredSink {
public:
    void log(const LogEvent& e) override {
        const char* sev =
            e.severity == LogSeverity::Error ? "error" :
            e.severity == LogSeverity::Warn  ? "warn" : "info";
        std::fputs("{\"severity\":\"", stderr);
        std::fputs(sev, stderr);
        std::fputs("\",\"event\":\"", stderr);
        std::fwrite(e.name.data(), 1, e.name.size(), stderr);
        std::fputc('"', stderr);
        for (auto& [k, v] : e.fields) {
            std::fputs(",\"", stderr);
            std::fwrite(k.data(), 1, k.size(), stderr);
            std::fputs("\":\"", stderr);
            std::fwrite(v.data(), 1, v.size(), stderr);
            std::fputc('"', stderr);
        }
        std::fputs("}\n", stderr);
    }
};

// In-process capture for tests. Stores every event verbatim in order.
class CapturingSink : public StructuredSink {
public:
    void log(const LogEvent& e) override { events.push_back(e); }
    std::vector<LogEvent> events;
};

// Singleton accessor. Default is NullSink.
inline StructuredSink*& obSinkPtr() {
    static StructuredSink* p = nullptr;
    if (!p) {
        static NullSink fallback;
        p = &fallback;
    }
    return p;
}

inline StructuredSink& obSink() { return *obSinkPtr(); }

// Install a sink. Caller retains ownership; pass nullptr to revert to
// the default NullSink.
inline void setObSink(StructuredSink* s) {
    static NullSink fallback;
    obSinkPtr() = s ? s : &fallback;
}

// Convenience constructor — `obEvent("name").kv("key","val") ...`. The
// returned LogEvent is logged via the trailing call site:
//   obSink().log(obEvent("trade").kv("price", 1000));
inline LogEvent obEvent(std::string_view name,
                        LogSeverity sev = LogSeverity::Info) {
    LogEvent e;
    e.severity = sev;
    e.name = name;
    return e;
}

}  // namespace OrderMatcher
