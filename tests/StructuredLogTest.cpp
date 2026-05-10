// StructuredLogTest — verifies the pluggable event-sink primitive.
//
// The default NullSink emits nothing — production code paying for
// telemetry must opt in via setObSink(). Tests use CapturingSink to
// inspect what events fired.
//
// Two halves:
//   1. Sink-mechanics: NullSink silent, CapturingSink records, JSON
//      sink produces expected text shape.
//   2. Wired-in events: drive a scenario through the engine and assert
//      the right events fired with the right keys (engine_start,
//      engine_stop, breaker_trip).

#include "Journal.h"
#include "MatchingEngine.h"
#include "Metrics.h"
#include "OrderBook.h"
#include "StructuredLog.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace OrderMatcher;

namespace {

// Find first event with the given name, or nullptr if absent.
const LogEvent* findEvent(const CapturingSink& sink, std::string_view name) {
    for (const auto& e : sink.events) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

// Find a field in an event, or empty if absent.
std::string field(const LogEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) {
        if (k == key) return v;
    }
    return {};
}

void test_null_sink_default() {
    // Default sink swallows events. Should NOT crash, and should
    // produce no observable side-effect.
    setObSink(nullptr);
    obSink().log(obEvent("anything").kv("x", 1ll));
    // No assertion possible against a no-op; just verify we got here.
}

void test_capturing_sink_records_in_order() {
    CapturingSink cap;
    setObSink(&cap);

    obSink().log(obEvent("a").kv("k1", "v1"));
    obSink().log(obEvent("b", LogSeverity::Warn).kv("count", 42ll));

    assert(cap.events.size() == 2);
    assert(cap.events[0].name == "a");
    assert(cap.events[0].severity == LogSeverity::Info);
    assert(field(cap.events[0], "k1") == "v1");
    assert(cap.events[1].name == "b");
    assert(cap.events[1].severity == LogSeverity::Warn);
    assert(field(cap.events[1], "count") == "42");

    setObSink(nullptr);
}

void test_engine_emits_start_and_stop() {
    CapturingSink cap;
    setObSink(&cap);

    {
        MatchingEngine engine;
        engine.addSymbol(1);
        engine.addSymbol(2);
        engine.start();
        engine.stop();
    }

    auto* start = findEvent(cap, "engine_start");
    auto* stop  = findEvent(cap, "engine_stop");
    assert(start && "engine_start not emitted");
    assert(stop  && "engine_stop not emitted");
    assert(field(*start, "mode") == "sync");
    // The MatchingEngine constructor pre-registers a default symbol (0)
    // for compatibility with single-symbol callers; with addSymbol(1)
    // and addSymbol(2) on top, the count at start() time is 3.
    assert(field(*start, "symbols") == "3");

    setObSink(nullptr);
}

void test_breaker_trip_emits_event() {
    CapturingSink cap;
    setObSink(&cap);

    OrderBook book(7);
    book.setCircuitBreakerThreshold(0.02);
    // Establish reference at 1,000,000.
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    // 3% move trips the 2% breaker.
    book.addOrder(2, 2, Side::Sell, 1030000, 100, OrderType::Limit);

    auto* trip = findEvent(cap, "breaker_trip");
    assert(trip && "breaker_trip not emitted on threshold breach");
    assert(trip->severity == LogSeverity::Warn);
    assert(field(*trip, "symbol") == "7");
    assert(field(*trip, "ref")    == "1000000");
    assert(field(*trip, "price")  == "1030000");

    setObSink(nullptr);
}

void test_trading_state_change_event() {
    CapturingSink cap;
    setObSink(&cap);

    OrderBook book(7);
    // Halted from Continuous: emit one event with from/to.
    book.setTradingState(TradingState::Halted);
    // Same-state assignment is a no-op and must NOT emit.
    book.setTradingState(TradingState::Halted);
    // Resume: another event.
    book.setTradingState(TradingState::Continuous);

    auto* halt = findEvent(cap, "trading_state_change");
    assert(halt && "trading_state_change not emitted");

    int stateChanges = 0;
    for (auto& e : cap.events) if (e.name == "trading_state_change") ++stateChanges;
    assert(stateChanges == 2 && "expected 2 transitions, no-op should be silent");

    // First transition is Continuous → Halted.
    assert(field(*halt, "symbol") == "7");
    assert(field(*halt, "from")   == "Continuous");
    assert(field(*halt, "to")     == "Halted");

    setObSink(nullptr);
}

void test_metrics_counter_wired_in_journal_commits() {
    // Drive a few journal commits and verify the
    // journal_entries_committed_total counter advances. This is the
    // pull-style complement to the push-style events tested above —
    // both should be available simultaneously.
    auto path = std::filesystem::temp_directory_path() /
        ("metrics_test_" + std::to_string(::getpid()) + ".log");
    std::filesystem::remove(path);

    auto& counter = MetricsRegistry::instance().counter(
        "journal_entries_committed_total", "");
    uint64_t before = counter.value();

    {
        Journal j(path.string(), Journal::SyncPolicy::Immediate, 1);
        for (int i = 0; i < 5; ++i) {
            j.logAddOrder(static_cast<OrderId>(i + 1), 1, 0,
                           Side::Buy, 1000, 10, OrderType::Limit);
        }
    }

    uint64_t after = counter.value();
    assert(after - before == 5 &&
           "expected counter to advance by 5 after 5 logged entries");

    // Prometheus exposition contains the counter line with TYPE annotation
    auto text = MetricsRegistry::instance().exportPrometheus();
    assert(text.find("# TYPE journal_entries_committed_total counter")
               != std::string::npos);
    assert(text.find("journal_entries_committed_total ") != std::string::npos);

    std::filesystem::remove(path);
}

void test_orders_accepted_rejected_counters() {
    auto& acc = MetricsRegistry::instance().counter("orders_accepted_total", "");
    auto& rej = MetricsRegistry::instance().counter("orders_rejected_total", "");
    uint64_t accBefore = acc.value();
    uint64_t rejBefore = rej.value();

    MatchingEngine engine;
    engine.addSymbol(11);
    engine.start();

    // Accepted: valid Limit order on a registered symbol.
    auto good = engine.submitOrder(11, /*id=*/1, /*pid=*/1, Side::Buy,
                                    1000, 5, OrderType::Limit);
    assert(good.isAccepted());

    // Rejected: same id resubmission → DuplicateOrderId.
    auto dup = engine.submitOrder(11, /*id=*/1, /*pid=*/1, Side::Buy,
                                   1000, 5, OrderType::Limit);
    assert(!dup.isAccepted());

    // Rejected: unknown symbol.
    auto unk = engine.submitOrder(99, /*id=*/2, /*pid=*/1, Side::Buy,
                                   1000, 5, OrderType::Limit);
    assert(!unk.isAccepted());

    engine.stop();

    // The pending state at the start of the run depends on what other
    // tests in this file submitted; the DELTA is what matters.
    assert(acc.value() - accBefore == 1 &&
           "expected 1 accepted order this run");
    assert(rej.value() - rejBefore == 2 &&
           "expected 2 rejected orders this run");
}

void test_no_events_after_unset() {
    CapturingSink cap;
    setObSink(&cap);
    obSink().log(obEvent("first"));
    setObSink(nullptr);
    obSink().log(obEvent("ignored_after_unset"));
    assert(cap.events.size() == 1);
    assert(cap.events[0].name == "first");
}

}  // namespace

int main() {
    test_null_sink_default();
    test_capturing_sink_records_in_order();
    test_engine_emits_start_and_stop();
    test_breaker_trip_emits_event();
    test_trading_state_change_event();
    test_metrics_counter_wired_in_journal_commits();
    test_orders_accepted_rejected_counters();
    test_no_events_after_unset();
    std::puts("StructuredLogTest passed");
    return 0;
}
