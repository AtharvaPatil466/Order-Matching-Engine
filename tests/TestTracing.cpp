// TestTracing — verifies the dependency-free span/trace primitives.
//
// The Tracer is OpenTelemetry-SHAPED but pulls in no SDK. These tests
// pin the contract instrumentation depends on:
//   1. A root span plus two children export, with both children sharing
//      the root's traceId and pointing parentSpanId at the root.
//   2. Attributes attached to an open span survive into the exported span.
//   3. CapturingExporter records exactly the spans ended, in end order.
//   4. A Tracer with no exporter (and one with an explicit NullExporter)
//      records nothing and stays a no-op.
//   5. An injected clock yields deterministic start/end timestamps.

#include "Tracing.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using namespace OrderMatcher;

static int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { ++passed; std::cout << " PASS\n"; } while (0)

namespace {

// Find the first captured span with a given name, or nullptr.
const Span* findSpan(const CapturingExporter& exp, const std::string& name) {
    for (const auto& s : exp.spans) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// A deterministic clock: each tick() advances by a fixed step, and now()
// returns the current value. Lets tests assert exact start/end stamps.
class FakeClock {
public:
    explicit FakeClock(uint64_t start = 1000, uint64_t step = 10)
        : value_(start), step_(step) {}

    // Bind as the Tracer's clock. Reading the clock advances it by one
    // step, so successive timestamps are distinct and predictable.
    uint64_t operator()() {
        uint64_t v = value_;
        value_ += step_;
        return v;
    }

private:
    uint64_t value_;
    uint64_t step_;
};

void test_root_and_children_share_trace() {
    TEST(RootAndChildrenShareTrace);

    CapturingExporter exp;
    Tracer tracer;
    tracer.setExporter(&exp);

    uint64_t root = tracer.startSpan("match_cycle");
    uint64_t childA = tracer.startSpan("admit_order", root);
    uint64_t childB = tracer.startSpan("publish_md", root);

    // End children before the root (typical nesting order).
    tracer.endSpan(childA);
    tracer.endSpan(childB);
    tracer.endSpan(root);

    assert(exp.spans.size() == 3);
    assert(tracer.openSpanCount() == 0);

    const Span* sRoot = findSpan(exp, "match_cycle");
    const Span* sA = findSpan(exp, "admit_order");
    const Span* sB = findSpan(exp, "publish_md");
    assert(sRoot && sA && sB);

    // Ids are non-zero and distinct.
    assert(sRoot->spanId != 0 && sA->spanId != 0 && sB->spanId != 0);
    assert(sRoot->spanId != sA->spanId);
    assert(sA->spanId != sB->spanId);
    assert(sRoot->spanId == root && sA->spanId == childA && sB->spanId == childB);

    // Root is a root: parentSpanId == 0.
    assert(sRoot->parentSpanId == 0);

    // Children link to the root and inherit its traceId.
    assert(sA->parentSpanId == sRoot->spanId);
    assert(sB->parentSpanId == sRoot->spanId);
    assert(sA->traceId == sRoot->traceId);
    assert(sB->traceId == sRoot->traceId);

    // The root's traceId is itself non-zero.
    assert(sRoot->traceId != 0);

    PASS();
}

void test_attributes_captured() {
    TEST(AttributesCaptured);

    CapturingExporter exp;
    Tracer tracer;
    tracer.setExporter(&exp);

    uint64_t span = tracer.startSpan("submit_order");
    tracer.addAttribute(span, "symbol", "7");
    tracer.addAttribute(span, "side", "buy");
    tracer.addAttribute(span, "order_id", "42");

    // Attribute on an unknown span id is a harmless no-op.
    tracer.addAttribute(span + 9999, "ignored", "value");

    tracer.endSpan(span);

    assert(exp.spans.size() == 1);
    const Span& s = exp.spans[0];
    assert(s.attributes.size() == 3);
    assert(s.attributes[0] == std::make_pair(std::string("symbol"), std::string("7")));
    assert(s.attributes[1] == std::make_pair(std::string("side"), std::string("buy")));
    assert(s.attributes[2] ==
           std::make_pair(std::string("order_id"), std::string("42")));

    PASS();
}

void test_capturing_records_in_end_order() {
    TEST(CapturingRecordsInEndOrder);

    CapturingExporter exp;
    Tracer tracer;
    tracer.setExporter(&exp);

    uint64_t s1 = tracer.startSpan("first");
    uint64_t s2 = tracer.startSpan("second");
    uint64_t s3 = tracer.startSpan("third");

    // End out of start order: third, first, second.
    tracer.endSpan(s3);
    tracer.endSpan(s1);
    tracer.endSpan(s2);

    assert(exp.spans.size() == 3);
    assert(exp.spans[0].name == "third");
    assert(exp.spans[1].name == "first");
    assert(exp.spans[2].name == "second");

    // Every captured span is actually ended.
    for (const auto& s : exp.spans) {
        assert(s.ended());
    }

    PASS();
}

void test_double_end_does_not_double_export() {
    TEST(DoubleEndDoesNotDoubleExport);

    CapturingExporter exp;
    Tracer tracer;
    tracer.setExporter(&exp);

    uint64_t s = tracer.startSpan("once");
    tracer.endSpan(s);
    tracer.endSpan(s);  // second end is a no-op
    tracer.endSpan(s + 1234);  // unknown id is a no-op

    assert(exp.spans.size() == 1);
    assert(exp.spans[0].name == "once");

    PASS();
}

void test_no_exporter_is_noop() {
    TEST(NoExporterIsNoop);

    // No exporter set at all: every call is valid and exports nothing.
    Tracer tracer;
    uint64_t root = tracer.startSpan("silent_root");
    uint64_t child = tracer.startSpan("silent_child", root);
    tracer.addAttribute(child, "k", "v");
    tracer.endSpan(child);
    tracer.endSpan(root);
    assert(tracer.openSpanCount() == 0);

    // Explicit NullExporter behaves identically: records nothing.
    NullExporter null;
    Tracer tracer2;
    tracer2.setExporter(&null);
    uint64_t s = tracer2.startSpan("null_sink_span");
    tracer2.addAttribute(s, "x", "y");
    tracer2.endSpan(s);
    assert(tracer2.openSpanCount() == 0);

    // Setting an exporter back to nullptr disables export again.
    CapturingExporter exp;
    Tracer tracer3;
    tracer3.setExporter(&exp);
    uint64_t a = tracer3.startSpan("recorded");
    tracer3.endSpan(a);
    tracer3.setExporter(nullptr);
    uint64_t b = tracer3.startSpan("not_recorded");
    tracer3.endSpan(b);
    assert(exp.spans.size() == 1);
    assert(exp.spans[0].name == "recorded");

    PASS();
}

void test_injected_clock_is_deterministic() {
    TEST(InjectedClockIsDeterministic);

    CapturingExporter exp;
    // Clock starts at 1000 and advances 10ns per read.
    Tracer tracer(FakeClock(1000, 10));
    tracer.setExporter(&exp);

    // Reads (in order): startSpan(root) start=1000; child start=1010;
    // endSpan(child) end=1020; endSpan(root) end=1030.
    uint64_t root = tracer.startSpan("root");
    uint64_t child = tracer.startSpan("child", root);
    tracer.endSpan(child);
    tracer.endSpan(root);

    const Span* sRoot = findSpan(exp, "root");
    const Span* sChild = findSpan(exp, "child");
    assert(sRoot && sChild);

    assert(sChild->startNs == 1010);
    assert(sChild->endNs == 1020);
    assert(sChild->durationNs() == 10);

    assert(sRoot->startNs == 1000);
    assert(sRoot->endNs == 1030);
    assert(sRoot->durationNs() == 30);

    // setClock can re-bind a fresh deterministic source mid-flight.
    Tracer tracer2;
    tracer2.setExporter(&exp);
    tracer2.setClock(FakeClock(500, 5));
    uint64_t s = tracer2.startSpan("rebind");  // start=500
    tracer2.endSpan(s);                          // end=505
    const Span* sRebind = findSpan(exp, "rebind");
    assert(sRebind);
    assert(sRebind->startNs == 500);
    assert(sRebind->endNs == 505);

    PASS();
}

}  // namespace

int main() {
    std::cout << "\n=== Tracing Tests ===\n\n";

    test_root_and_children_share_trace();
    test_attributes_captured();
    test_capturing_records_in_end_order();
    test_double_end_does_not_double_export();
    test_no_exporter_is_noop();
    test_injected_clock_is_deterministic();

    std::cout << "\n--- Results: " << passed << " passed ---\n";
    std::cout << "\nAll tracing tests passed.\n\n";
    return 0;
}
