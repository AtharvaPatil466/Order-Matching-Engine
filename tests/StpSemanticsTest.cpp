// StpSemanticsTest — C1. What OrderStatus::CancelledBySTP is allowed to mean.
//
// The C1 finding was not a crash but a misclassification: an order removed by
// self-trade prevention reported Filled at full quantity, so the OCO listener
// counted it as an execution, its group "won", and live sibling orders were
// cancelled — for an order that traded nothing. The defence against that
// returning is not a comment; it is these assertions.
//
// Step 1 pins the classification only. The matching-path behaviour that
// produces the status arrives in Step 2, the pro-rata and auction paths in
// Step 3, and the outbound protocol mapping in Step 4.

#include <gtest/gtest.h>

#include "AuditLog.h"
#include "Types.h"

using namespace OrderMatcher;

// ─── The core invariant: STP removal is not an execution ────────────────────

TEST(StpSemantics, CancelledBySTPIsNotAnExecution) {
    EXPECT_FALSE(isExecution(OrderStatus::CancelledBySTP))
        << "an STP-cancelled order traded nothing; counting it as an execution "
           "is exactly the C1 phantom fill";
    // The only two statuses that may ever count as a fill.
    EXPECT_TRUE(isExecution(OrderStatus::Filled));
    EXPECT_TRUE(isExecution(OrderStatus::PartiallyFilled));
    EXPECT_FALSE(isExecution(OrderStatus::Cancelled));
    EXPECT_FALSE(isExecution(OrderStatus::Rejected));
    EXPECT_FALSE(isExecution(OrderStatus::Accepted));
    EXPECT_FALSE(isExecution(OrderStatus::New));
}

// ─── Distinct from client-initiated cancellation ────────────────────────────

TEST(StpSemantics, CancelledBySTPIsDistinctFromCancelled) {
    EXPECT_NE(OrderStatus::CancelledBySTP, OrderStatus::Cancelled)
        << "the client did not ask for this cancellation; collapsing the two "
           "loses the only signal that STP fired";
    EXPECT_NE(OrderStatus::CancelledBySTP, OrderStatus::Filled);
    EXPECT_NE(OrderStatus::CancelledBySTP, OrderStatus::Rejected);
}

// ─── Terminal: the order is gone and will not update again ──────────────────

TEST(StpSemantics, CancelledBySTPIsTerminal) {
    EXPECT_TRUE(isTerminalStatus(OrderStatus::CancelledBySTP));
    // The audit trail must close the record out, not leave it open forever.
    AuditRecord r;
    r.finalStatus = OrderStatus::CancelledBySTP;
    EXPECT_TRUE(r.isTerminal())
        << "an STP-cancelled order never updates again; an audit record left "
           "non-terminal never closes";
}

TEST(StpSemantics, NonTerminalStatusesStayNonTerminal) {
    EXPECT_FALSE(isTerminalStatus(OrderStatus::New));
    EXPECT_FALSE(isTerminalStatus(OrderStatus::Accepted));
    // A partial fill is still working — the remainder rests.
    EXPECT_FALSE(isTerminalStatus(OrderStatus::PartiallyFilled));
}

// ─── The OCO gate is driven by isExecution, so STP cannot win a group ───────
//
// MatchingEngine::OcoBookListener::onOrderUpdate buffers an id only when
// isExecution(u.status). This asserts the predicate that gate depends on;
// the end-to-end "sibling is not cancelled" check lands in Step 2, once the
// matching path actually emits the status.

TEST(StpSemantics, OnlyExecutionsCanTriggerOco) {
    for (auto s : {OrderStatus::New, OrderStatus::Accepted, OrderStatus::Cancelled,
                   OrderStatus::CancelledBySTP, OrderStatus::Rejected}) {
        EXPECT_FALSE(isExecution(s))
            << "status " << static_cast<int>(s)
            << " must not be able to trigger an OCO group execution";
    }
}
