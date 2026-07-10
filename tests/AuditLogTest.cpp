// AuditLogTest — order lifecycle audit trail (include/AuditLog.h).
//
// Proves:
//   1. A full lifecycle (submit -> accept -> fills -> final Filled) is captured
//      with every fill's price/qty/counterparty and rdtsc+wall timestamps.
//   2. Rejected and cancelled lifecycles record the right terminal state and
//      reject reason.
//   3. Records are queryable by order_id AND participant_id.
//   4. The backing file is genuinely append-only: one JSON line per event,
//      surviving a reopen, never rewritten.

#include "AuditLog.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

using namespace OrderMatcher;

static int tests_passed = 0;
#define SECTION(name) std::cout << "  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while (0)

static size_t countLines(const std::string& path) {
    std::ifstream in(path);
    size_t n = 0;
    std::string line;
    while (std::getline(in, line)) if (!line.empty()) ++n;
    return n;
}

// ── Test 1: full fill lifecycle ─────────────────────────────────────────────
static void test_full_fill_lifecycle() {
    SECTION("full fill lifecycle: submit/accept/fills/final");
    const std::string path = "/tmp/audit_test_fill.jsonl";
    std::remove(path.c_str());

    AuditLog log(path);
    assert(log.isOpen());

    // Buy 100 @ 100.00, symbol 7, participant 42.
    log.recordSubmit(1, 42, 7, Side::Buy, OrderType::Limit, 1000000, 100);
    log.recordAccept(1);
    log.recordFill(1, 1000000, 40, /*cpty=*/99, /*cptyOrder=*/500);
    log.recordFill(1, 1000000, 60, /*cpty=*/99, /*cptyOrder=*/501);
    log.recordFinal(1, OrderStatus::Filled);

    AuditRecord r;
    assert(log.findByOrder(1, r));
    assert(r.orderId == 1 && r.participantId == 42 && r.symbolId == 7);
    assert(r.side == Side::Buy && r.type == OrderType::Limit);
    assert(r.price == 1000000 && r.quantity == 100);
    assert(r.submit.set() && r.accept.set() && r.finalTime.set());
    assert(r.rejectReason == RejectReason::None);
    assert(r.fills.size() == 2);
    assert(r.fills[0].qty == 40 && r.fills[0].counterparty == 99 &&
           r.fills[0].counterpartyOrderId == 500);
    assert(r.fills[1].qty == 60 && r.fills[1].counterpartyOrderId == 501);
    assert(r.filledQty() == 100);
    assert(r.finalStatus == OrderStatus::Filled && r.isTerminal());

    // 5 events => 5 append-only lines.
    log.flush();
    assert(countLines(path) == 5);

    std::remove(path.c_str());
    PASS();
}

// ── Test 2: reject lifecycle ────────────────────────────────────────────────
static void test_reject_lifecycle() {
    SECTION("reject lifecycle records reason + terminal state");
    const std::string path = "/tmp/audit_test_reject.jsonl";
    std::remove(path.c_str());

    AuditLog log(path);
    log.recordSubmit(2, 42, 7, Side::Sell, OrderType::Limit, 1010000, 50);
    log.recordReject(2, RejectReason::RiskLimitBreached);

    AuditRecord r;
    assert(log.findByOrder(2, r));
    assert(r.finalStatus == OrderStatus::Rejected && r.isTerminal());
    assert(r.rejectReason == RejectReason::RiskLimitBreached);
    assert(!r.accept.set());          // never accepted
    assert(r.fills.empty());
    assert(r.finalTime.set());

    log.flush();
    assert(countLines(path) == 2);    // submit + reject
    std::remove(path.c_str());
    PASS();
}

// ── Test 3: cancel lifecycle after partial fill ─────────────────────────────
static void test_cancel_after_partial() {
    SECTION("cancel after partial fill");
    AuditLog log;  // in-memory only

    log.recordSubmit(3, 7, 1, Side::Buy, OrderType::Limit, 990000, 200);
    log.recordAccept(3);
    log.recordFill(3, 990000, 75, 8, 900);
    log.recordFinal(3, OrderStatus::Cancelled);

    AuditRecord r;
    assert(log.findByOrder(3, r));
    assert(r.finalStatus == OrderStatus::Cancelled && r.isTerminal());
    assert(r.filledQty() == 75);
    assert(r.fills.size() == 1);
    PASS();
}

// ── Test 4: query by participant ────────────────────────────────────────────
static void test_query_by_participant() {
    SECTION("query by participant returns all their orders");
    AuditLog log;

    log.recordSubmit(10, 100, 1, Side::Buy,  OrderType::Limit, 1000000, 10);
    log.recordSubmit(11, 100, 1, Side::Sell, OrderType::Limit, 1000000, 10);
    log.recordSubmit(12, 200, 1, Side::Buy,  OrderType::Limit, 1000000, 10);
    log.recordAccept(10);
    log.recordAccept(11);
    log.recordAccept(12);

    auto p100 = log.findByParticipant(100);
    assert(p100.size() == 2);
    auto p200 = log.findByParticipant(200);
    assert(p200.size() == 1);
    assert(p200[0].orderId == 12);
    auto pNone = log.findByParticipant(999);
    assert(pNone.empty());
    assert(log.recordCount() == 3);
    PASS();
}

// ── Test 5: append-only file survives reopen ────────────────────────────────
static void test_append_only_survives_reopen() {
    SECTION("append-only file survives reopen (never truncated)");
    const std::string path = "/tmp/audit_test_reopen.jsonl";
    std::remove(path.c_str());

    {
        AuditLog log(path);
        log.recordSubmit(20, 1, 1, Side::Buy, OrderType::Limit, 1000000, 10);
        log.recordAccept(20);
        log.flush();
    }
    assert(countLines(path) == 2);

    // Reopen in append mode: prior lines must remain, new lines add on.
    {
        AuditLog log(path);
        log.recordSubmit(21, 1, 1, Side::Sell, OrderType::Limit, 1000000, 10);
        log.recordFinal(21, OrderStatus::Cancelled);
        log.flush();
    }
    assert(countLines(path) == 4);    // 2 old + 2 new — nothing overwritten

    std::remove(path.c_str());
    PASS();
}

int main() {
    std::cout << "\n=== Order Lifecycle Audit Trail Tests ===" << std::endl;
    test_full_fill_lifecycle();
    test_reject_lifecycle();
    test_cancel_after_partial();
    test_query_by_participant();
    test_append_only_survives_reopen();
    std::cout << "\n--- " << tests_passed << " passed ---\n" << std::endl;
    return 0;
}
