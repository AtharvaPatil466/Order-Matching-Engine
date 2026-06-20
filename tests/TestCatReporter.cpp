// TestCatReporter — verifies the engine-agnostic CAT lifecycle recorder.
//
// CatReporter is FORMAT/serialization only: it appends order-lifecycle
// events into a monotonically-sequenced, append-only log and renders that
// log as JSON Lines. These assert-based tests pin down the properties a
// downstream auditor relies on:
//   1. Records land in call order with strictly-increasing sequence
//      numbers starting at 1, and recordCount tracks the size.
//   2. A full lifecycle (new -> trade -> cancel) yields exactly the
//      expected record types and carries both trade legs.
//   3. toJsonl emits exactly recordCount() lines, each containing the
//      event-type string and the relevant ids.
//   4. clear() returns the reporter to the freshly-constructed state,
//      including rewinding the sequence counter to 1.

#include "CatReporter.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

using namespace OrderMatcher;

namespace {

// Count newline-terminated lines in a JSONL blob. JSONL terminates every
// record (including the last) with '\n', so the line count equals the
// number of '\n' characters.
size_t countLines(const std::string& s) {
    size_t lines = 0;
    for (char c : s) {
        if (c == '\n') ++lines;
    }
    return lines;
}

// Whether `needle` occurs anywhere in `haystack`.
bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void test_appends_in_call_order_with_monotonic_sequence() {
    CatReporter rep;
    assert(rep.recordCount() == 0);
    assert(rep.records().empty());

    rep.recordNewOrder(/*ts=*/100, /*id=*/10, /*pid=*/1, /*sym=*/7,
                        Side::Buy, /*px=*/1000, /*qty=*/50);
    rep.recordNewOrder(/*ts=*/110, /*id=*/11, /*pid=*/2, /*sym=*/7,
                        Side::Sell, /*px=*/1005, /*qty=*/40);
    rep.recordModify(/*ts=*/120, /*id=*/10, /*newQty=*/30);
    rep.recordCancel(/*ts=*/130, /*id=*/11);

    const auto& recs = rep.records();
    assert(recs.size() == 4);
    assert(rep.recordCount() == 4);

    // Sequence is assigned by the reporter, starts at 1, strictly
    // increases by 1, and follows call order regardless of caller ids.
    for (size_t i = 0; i < recs.size(); ++i) {
        assert(recs[i].sequence == i + 1);
    }
    assert(recs[0].sequence < recs[1].sequence);
    assert(recs[1].sequence < recs[2].sequence);
    assert(recs[2].sequence < recs[3].sequence);

    // Call-order fidelity: types and payloads match what was recorded.
    assert(recs[0].type == CatEventType::New);
    assert(recs[0].orderId == 10 && recs[0].participant == 1);
    assert(recs[0].symbol == 7 && recs[0].side == Side::Buy);
    assert(recs[0].price == 1000 && recs[0].quantity == 50);
    assert(recs[0].timestampNs == 100);

    assert(recs[1].type == CatEventType::New);
    assert(recs[1].side == Side::Sell && recs[1].orderId == 11);

    assert(recs[2].type == CatEventType::Modify);
    assert(recs[2].orderId == 10 && recs[2].quantity == 30);

    assert(recs[3].type == CatEventType::Cancel);
    assert(recs[3].orderId == 11);
}

void test_full_lifecycle_new_trade_cancel() {
    CatReporter rep;

    // A buy and a sell rest, then partially trade, then the remainder is
    // cancelled — the canonical lifecycle an audit trail must capture.
    rep.recordNewOrder(/*ts=*/200, /*buyId=*/100, /*pid=*/1, /*sym=*/3,
                        Side::Buy, /*px=*/2500, /*qty=*/100);
    rep.recordNewOrder(/*ts=*/201, /*sellId=*/200, /*pid=*/2, /*sym=*/3,
                        Side::Sell, /*px=*/2500, /*qty=*/60);
    rep.recordTrade(/*ts=*/202, /*tradeId=*/9000, /*buyOrderId=*/100,
                    /*sellOrderId=*/200, /*sym=*/3, /*px=*/2500, /*qty=*/60);
    rep.recordCancel(/*ts=*/203, /*id=*/100);

    const auto& recs = rep.records();
    assert(recs.size() == 4);

    // Exactly the expected sequence of record types.
    assert(recs[0].type == CatEventType::New);
    assert(recs[1].type == CatEventType::New);
    assert(recs[2].type == CatEventType::Trade);
    assert(recs[3].type == CatEventType::Cancel);

    // The Trade row carries both legs: buy in orderId, sell in
    // contraOrderId, plus the tradeId and execution price/qty.
    const auto& trade = recs[2];
    assert(trade.orderId == 100);        // buy (aggressor) leg
    assert(trade.contraOrderId == 200);  // sell (contra) leg
    assert(trade.tradeId == 9000);
    assert(trade.symbol == 3);
    assert(trade.price == 2500);
    assert(trade.quantity == 60);
    assert(trade.side == Side::Buy);     // orderId is the buy side

    // Non-trade rows leave the trade-only fields at zero.
    assert(recs[0].tradeId == 0 && recs[0].contraOrderId == 0);
    assert(recs[3].tradeId == 0 && recs[3].contraOrderId == 0);
}

void test_to_jsonl_line_count_and_content() {
    CatReporter rep;
    rep.recordNewOrder(/*ts=*/300, /*id=*/42, /*pid=*/5, /*sym=*/9,
                        Side::Sell, /*px=*/-1234, /*qty=*/7);
    rep.recordTrade(/*ts=*/301, /*tradeId=*/777, /*buyOrderId=*/42,
                    /*sellOrderId=*/43, /*sym=*/9, /*px=*/1234, /*qty=*/7);
    rep.recordCancel(/*ts=*/302, /*id=*/43);

    const std::string jsonl = rep.toJsonl();

    // Exactly one line per record.
    assert(countLines(jsonl) == rep.recordCount());
    assert(countLines(jsonl) == 3);

    // Every type string appears in the output.
    assert(contains(jsonl, "\"type\":\"New\""));
    assert(contains(jsonl, "\"type\":\"Trade\""));
    assert(contains(jsonl, "\"type\":\"Cancel\""));

    // Ids survive into the JSON, including both trade legs.
    assert(contains(jsonl, "\"orderId\":42"));
    assert(contains(jsonl, "\"contraOrderId\":43"));
    assert(contains(jsonl, "\"tradeId\":777"));

    // Sequence numbers are present and start at 1.
    assert(contains(jsonl, "\"sequence\":1"));
    assert(contains(jsonl, "\"sequence\":2"));
    assert(contains(jsonl, "\"sequence\":3"));

    // Side rendered as a string; signed price preserved verbatim.
    assert(contains(jsonl, "\"side\":\"Sell\""));
    assert(contains(jsonl, "\"price\":-1234"));

    // Empty reporter serializes to the empty string (no stray newline).
    CatReporter empty;
    assert(empty.toJsonl().empty());
}

void test_clear_resets_to_fresh_state() {
    CatReporter rep;
    rep.recordNewOrder(/*ts=*/400, /*id=*/1, /*pid=*/1, /*sym=*/1,
                        Side::Buy, /*px=*/500, /*qty=*/10);
    rep.recordCancel(/*ts=*/401, /*id=*/1);
    assert(rep.recordCount() == 2);

    rep.clear();
    assert(rep.recordCount() == 0);
    assert(rep.records().empty());
    assert(rep.toJsonl().empty());

    // After clear() the sequence counter must rewind: the next record is
    // sequence 1 again, indistinguishable from a fresh reporter.
    rep.recordNewOrder(/*ts=*/500, /*id=*/2, /*pid=*/1, /*sym=*/1,
                        Side::Buy, /*px=*/600, /*qty=*/20);
    assert(rep.recordCount() == 1);
    assert(rep.records()[0].sequence == 1);
    assert(rep.records()[0].orderId == 2);
}

}  // namespace

int main() {
    test_appends_in_call_order_with_monotonic_sequence();
    test_full_lifecycle_new_trade_cancel();
    test_to_jsonl_line_count_and_content();
    test_clear_resets_to_fresh_state();
    std::puts("TestCatReporter passed");
    return 0;
}
