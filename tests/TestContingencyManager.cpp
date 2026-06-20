// TestContingencyManager — OCO (One-Cancels-Other) linkage semantics.
//
// What this proves about the engine-agnostic ContingencyManager:
//   1. A 2-leg group: executing one leg returns the other to cancel.
//   2. A 3-leg group: executing one leg returns the other two.
//   3. Cancel-before-fill deregisters a leg, and a later execution of a
//      surviving leg returns only the still-live siblings.
//   4. Once a group is resolved by a winner, any further onExecuted on any
//      member returns empty (the group has no further effect).
//   5. Repeat notifications (double-execute, double-cancel) are idempotent
//      no-ops.
//   6. onExecuted on an unlinked id returns empty and isLinked is false.
//   7. activeGroupCount tracks registration, resolution, and dissolution.

#include "ContingencyManager.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

using namespace OrderMatcher;

static int tests_passed = 0;

#define SECTION(name) std::cout << "\n  " << name << "..." << std::flush
#define PASS()        do { ++tests_passed; std::cout << " PASS" << std::endl; } while(0)

// Order-independent comparison: onExecuted returns siblings in unspecified
// order (backed by an unordered_set), so tests must not assume a sequence.
static std::vector<OrderId> sorted(std::vector<OrderId> v) {
    std::sort(v.begin(), v.end());
    return v;
}

static bool sameSet(std::vector<OrderId> a, std::vector<OrderId> b) {
    return sorted(std::move(a)) == sorted(std::move(b));
}

void test_two_leg_executes_returns_sibling() {
    SECTION("2-leg: executing one returns the other");

    ContingencyManager mgr;
    const OrderId takeProfit = 100;
    const OrderId stopLoss = 200;

    uint64_t gid = mgr.registerOco(takeProfit, stopLoss);
    assert(gid != 0);
    assert(mgr.isLinked(takeProfit));
    assert(mgr.isLinked(stopLoss));
    assert(mgr.activeGroupCount() == 1);

    std::vector<OrderId> toCancel = mgr.onExecuted(takeProfit);
    assert(toCancel.size() == 1);
    assert(toCancel[0] == stopLoss);

    // Group resolved: neither leg is linked any longer.
    assert(!mgr.isLinked(takeProfit));
    assert(!mgr.isLinked(stopLoss));
    assert(mgr.activeGroupCount() == 0);

    PASS();
}

void test_three_leg_executes_returns_other_two() {
    SECTION("3-leg: executing one returns the other two");

    ContingencyManager mgr;
    uint64_t gid = mgr.registerOco(std::vector<OrderId>{1, 2, 3});
    assert(gid != 0);
    assert(mgr.activeGroupCount() == 1);

    std::vector<OrderId> toCancel = mgr.onExecuted(2);
    assert(sameSet(toCancel, {1, 3}));

    // The winner is not among the siblings to cancel.
    assert(std::find(toCancel.begin(), toCancel.end(), 2) == toCancel.end());

    assert(!mgr.isLinked(1));
    assert(!mgr.isLinked(2));
    assert(!mgr.isLinked(3));
    assert(mgr.activeGroupCount() == 0);

    PASS();
}

void test_cancel_before_fill_then_execute_survivors() {
    SECTION("cancel-before-fill deregisters, then execute returns survivors");

    ContingencyManager mgr;
    mgr.registerOco(std::vector<OrderId>{10, 20, 30});
    assert(mgr.activeGroupCount() == 1);

    // Leg 20 is cancelled without ever executing: just deregistered.
    mgr.onCanceled(20);
    assert(!mgr.isLinked(20));
    assert(mgr.isLinked(10));
    assert(mgr.isLinked(30));
    // The group still has two live members, so it remains active.
    assert(mgr.activeGroupCount() == 1);

    // Now leg 10 executes: only the still-live sibling (30) is returned.
    std::vector<OrderId> toCancel = mgr.onExecuted(10);
    assert(toCancel.size() == 1);
    assert(toCancel[0] == 30);
    assert(mgr.activeGroupCount() == 0);

    PASS();
}

void test_single_survivor_is_dissolved_not_cancelled() {
    SECTION("group dropping to one live member dissolves (survivor is normal)");

    ContingencyManager mgr;
    mgr.registerOco(40, 50);
    assert(mgr.activeGroupCount() == 1);

    // Cancel one leg without a fill. The lone survivor must NOT be auto
    // cancelled and must produce no siblings if it later executes.
    mgr.onCanceled(40);
    assert(!mgr.isLinked(40));
    // Survivor is no longer meaningfully linked; the group is dissolved.
    assert(!mgr.isLinked(50));
    assert(mgr.activeGroupCount() == 0);

    std::vector<OrderId> toCancel = mgr.onExecuted(50);
    assert(toCancel.empty());

    PASS();
}

void test_resolved_group_second_execute_returns_empty() {
    SECTION("resolved group: a second onExecuted on any member returns empty");

    ContingencyManager mgr;
    mgr.registerOco(std::vector<OrderId>{7, 8, 9});

    std::vector<OrderId> first = mgr.onExecuted(7);
    assert(sameSet(first, {8, 9}));

    // The group is resolved. Re-executing the winner, or a former sibling,
    // yields nothing.
    assert(mgr.onExecuted(7).empty());
    assert(mgr.onExecuted(8).empty());
    assert(mgr.onExecuted(9).empty());
    assert(mgr.activeGroupCount() == 0);

    PASS();
}

void test_idempotent_repeat_notifications() {
    SECTION("repeat notifications (double-execute, double-cancel) are safe");

    ContingencyManager mgr;
    mgr.registerOco(std::vector<OrderId>{1000, 2000, 3000});

    // Double-cancel the same leg: second call is a no-op.
    mgr.onCanceled(2000);
    mgr.onCanceled(2000);
    assert(!mgr.isLinked(2000));
    assert(mgr.activeGroupCount() == 1);

    // Execute a survivor, then execute it again: second returns empty.
    std::vector<OrderId> toCancel = mgr.onExecuted(1000);
    assert(toCancel.size() == 1);
    assert(toCancel[0] == 3000);
    assert(mgr.onExecuted(1000).empty());

    // Cancelling an already-removed (won) leg is a harmless no-op.
    mgr.onCanceled(1000);
    mgr.onCanceled(3000);
    assert(mgr.activeGroupCount() == 0);

    PASS();
}

void test_unlinked_id_behaviour() {
    SECTION("onExecuted on unlinked id returns empty; isLinked is false");

    ContingencyManager mgr;
    assert(!mgr.isLinked(424242));
    assert(mgr.onExecuted(424242).empty());
    // Cancelling an unknown id is a no-op and must not create state.
    mgr.onCanceled(424242);
    assert(mgr.activeGroupCount() == 0);

    // Degenerate registrations carry no linkage and return the reserved 0.
    assert(mgr.registerOco(std::vector<OrderId>{}) == 0);
    assert(mgr.registerOco(std::vector<OrderId>{5}) == 0);
    assert(mgr.registerOco(std::vector<OrderId>{6, 6}) == 0);  // dup collapses
    assert(!mgr.isLinked(5));
    assert(!mgr.isLinked(6));
    assert(mgr.activeGroupCount() == 0);

    PASS();
}

void test_active_group_count_tracks_multiple_groups() {
    SECTION("activeGroupCount tracks multiple independent groups");

    ContingencyManager mgr;
    uint64_t g1 = mgr.registerOco(1, 2);
    uint64_t g2 = mgr.registerOco(std::vector<OrderId>{3, 4, 5});
    uint64_t g3 = mgr.registerOco(6, 7);
    // Distinct, non-zero group ids.
    assert(g1 != 0 && g2 != 0 && g3 != 0);
    assert(g1 != g2 && g2 != g3 && g1 != g3);
    assert(mgr.activeGroupCount() == 3);

    // Resolve one group via a winner.
    mgr.onExecuted(1);
    assert(mgr.activeGroupCount() == 2);

    // Dissolve another by cancelling down to a single survivor.
    mgr.onCanceled(6);
    assert(mgr.activeGroupCount() == 1);

    // The third group's executions are unaffected by the others.
    std::vector<OrderId> toCancel = mgr.onExecuted(4);
    assert(sameSet(toCancel, {3, 5}));
    assert(mgr.activeGroupCount() == 0);

    PASS();
}

int main() {
    std::cout << "\n=== ContingencyManager (OCO) Tests ===\n";

    test_two_leg_executes_returns_sibling();
    test_three_leg_executes_returns_other_two();
    test_cancel_before_fill_then_execute_survivors();
    test_single_survivor_is_dissolved_not_cancelled();
    test_resolved_group_second_execute_returns_empty();
    test_idempotent_repeat_notifications();
    test_unlinked_id_behaviour();
    test_active_group_count_tracks_multiple_groups();

    std::cout << "\n--- Results: " << tests_passed << " passed ---\n";
    std::cout << "\nAll ContingencyManager tests passed.\n\n";
    return 0;
}
