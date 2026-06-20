#pragma once

// Tracing — minimal, dependency-free distributed-tracing primitives.
//
// Roadmap (PRODUCTION_ROADMAP.md): "Prometheus / OpenTelemetry — export
// real-time metrics ... OpenTelemetry tracing not yet integrated." This
// header gives the engine an OpenTelemetry-SHAPED span/trace API today,
// built on nothing but the standard library, so call sites can be
// instrumented now and later bridged to a real OTel exporter without
// being rewritten.
//
// What this is: a Tracer that mints spans (root or child), lets callers
// attach key/value attributes, and emits each finished span to a
// pluggable SpanExporter on endSpan(). The data model mirrors OTel: a
// Span carries a 128-bit-style identity split into (traceId, spanId), a
// parentSpanId link (0 == root), a name, start/end timestamps in
// nanoseconds, and a flat attribute list.
//
// What this is NOT: an OpenTelemetry SDK. There is no OTLP/gRPC/protobuf,
// no context propagation across process boundaries, no sampler, no
// baggage, no W3C traceparent wire format. The point is one clean
// in-house seam — a real exporter (OTLP, Jaeger, stdout) can implement
// SpanExporter later without touching instrumentation.
//
// Design choices:
//   * Default exporter is none (nullptr) — the Tracer is a no-op and pays
//     no export cost. A NullExporter is also provided for callers that
//     want an explicit silent sink. CapturingExporter records finished
//     spans in end order for tests.
//   * Spans live in the Tracer until ended (kept in an open-span map keyed
//     by spanId). endSpan() stamps endNs, hands a const Span& to the
//     exporter, then drops it. A span is only exported once.
//   * The clock is injectable (std::function<uint64_t()> returning
//     nanoseconds) and defaults to a real steady_clock reading, so tests
//     get deterministic start/end timestamps.
//   * Ids are drawn from a monotonic 64-bit counter seeded off the clock;
//     a root span gets a fresh traceId, a child inherits its parent's
//     traceId and links the parent via parentSpanId.
//   * Not thread-safe by itself; instrument from a single thread or guard
//     externally. Keeping the no-op path lock-free is deliberate.

#include "Types.h"  // not strictly required, but keeps the engine's units in scope

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OrderMatcher {

// A single unit of work. Mirrors the OpenTelemetry span shape closely
// enough that a bridge can map fields one-to-one.
struct Span {
    uint64_t traceId{0};
    uint64_t spanId{0};
    uint64_t parentSpanId{0};  // 0 == root span
    std::string name;
    uint64_t startNs{0};
    uint64_t endNs{0};  // 0 until ended
    std::vector<std::pair<std::string, std::string>> attributes;

    // Convenience for callers/tests inspecting a captured span.
    bool ended() const { return endNs != 0; }
    uint64_t durationNs() const { return endNs >= startNs ? endNs - startNs : 0; }
};

// Pluggable sink. A finished span is delivered here exactly once.
class SpanExporter {
public:
    virtual ~SpanExporter() = default;
    virtual void onSpanEnd(const Span&) = 0;
};

// Explicit silent sink. Zero allocations, one indirect call.
class NullExporter : public SpanExporter {
public:
    void onSpanEnd(const Span&) override {}
};

// In-process capture for tests. Stores every finished span verbatim, in
// the order endSpan() fired.
class CapturingExporter : public SpanExporter {
public:
    void onSpanEnd(const Span& s) override { spans.push_back(s); }
    std::vector<Span> spans;
};

// Monotonic nanosecond clock used as the default time source. Pulled out
// so the default and any test override share one signature.
inline uint64_t steadyClockNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

class Tracer {
public:
    using Clock = std::function<uint64_t()>;

    Tracer() : clock_(&steadyClockNs) {}
    explicit Tracer(Clock clock) : clock_(std::move(clock)) {}

    // Install the export sink. Caller retains ownership. nullptr (the
    // default) disables export: startSpan/addAttribute/endSpan stay
    // valid no-ops cost-wise on the export path.
    void setExporter(SpanExporter* e) { exporter_ = e; }

    // Replace the time source. Useful for deterministic tests; the value
    // returned is interpreted as nanoseconds.
    void setClock(Clock clock) { clock_ = std::move(clock); }

    // Begin a span. parentSpanId == 0 mints a root span with a fresh
    // traceId; a non-zero parent that is currently open makes a child
    // sharing the parent's traceId and linking parentSpanId. (An unknown
    // parent id still produces a child link, but with a fresh traceId,
    // since the parent's trace is not known to this Tracer.) Returns the
    // new spanId, which is always non-zero.
    uint64_t startSpan(const std::string& name, uint64_t parentSpanId = 0) {
        const uint64_t spanId = nextId();

        Span span;
        span.spanId = spanId;
        span.parentSpanId = parentSpanId;
        span.name = name;
        span.startNs = now();

        if (parentSpanId != 0) {
            auto it = open_.find(parentSpanId);
            span.traceId = (it != open_.end()) ? it->second.traceId : nextId();
        } else {
            span.traceId = nextId();
        }

        open_.emplace(spanId, std::move(span));
        return spanId;
    }

    // Attach a key/value attribute to an open span. No-op if the span is
    // unknown or already ended.
    void addAttribute(uint64_t spanId, const std::string& k, const std::string& v) {
        auto it = open_.find(spanId);
        if (it == open_.end()) return;
        it->second.attributes.emplace_back(k, v);
    }

    // Finish a span: stamp endNs and, if an exporter is installed, hand it
    // the completed span. The span is then removed, so a second endSpan
    // for the same id is a harmless no-op (and never double-exports).
    void endSpan(uint64_t spanId) {
        auto it = open_.find(spanId);
        if (it == open_.end()) return;

        it->second.endNs = now();
        if (exporter_) {
            exporter_->onSpanEnd(it->second);
        }
        open_.erase(it);
    }

    // Spans started but not yet ended. Exposed for tests / leak checks.
    std::size_t openSpanCount() const { return open_.size(); }

private:
    uint64_t now() const { return clock_ ? clock_() : steadyClockNs(); }

    // Monotonic id source. Seeded off the real steady clock (NOT the
    // injectable clock_, so id generation never consumes a test clock
    // tick) so distinct Tracer runs are unlikely to collide, then
    // strictly incremented. Never returns 0 (0 is reserved for "no span").
    uint64_t nextId() {
        if (counter_ == 0) {
            counter_ = steadyClockNs() | 1ull;  // seed; ensure non-zero
        }
        uint64_t id = counter_++;
        if (id == 0) id = counter_++;  // wrap guard: skip the reserved 0
        return id;
    }

    SpanExporter* exporter_{nullptr};
    Clock clock_;
    uint64_t counter_{0};
    std::unordered_map<uint64_t, Span> open_;
};

}  // namespace OrderMatcher
