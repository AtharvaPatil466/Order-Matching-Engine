#pragma once

// ContingencyManager — One-Cancels-Other (OCO) order linkage.
//
// Traders submit linked orders where the first to execute cancels the
// others. The classic case is a take-profit limit paired with a
// stop-loss: whichever triggers first cancels its sibling so the trader
// is never left holding both legs.
//
// This component is deliberately ENGINE-AGNOSTIC. It knows nothing about
// OrderBook, MatchingEngine, prices, sides, or matching. It is a pure
// registry plus decision function: the caller notifies it of executions
// and terminations, and it returns the OrderIds the caller must act on.
// Keeping linkage policy out of the hot matching path means the matching
// engine stays a single-responsibility component and OCO semantics can be
// model-checked (TLA+) and unit-tested in isolation.
//
// Semantics (exact):
//   - An OCO group links 2 or more orders by OrderId.
//   - The FIRST execution (first trade — partial OR full fill) of ANY
//     member wins: every OTHER still-live member of that group must be
//     cancelled. The group is then RESOLVED and has no further effect.
//   - A member terminated WITHOUT executing (cancel/reject/expiry) is
//     simply DEREGISTERED; remaining members stay live. A group that
//     drops to a single live member with no winner is dissolved — the
//     survivor is a normal order, NOT auto-cancelled.
//   - Idempotent: repeat notifications about an already-resolved or
//     already-removed order are safe no-ops.

#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OrderMatcher {

class ContingencyManager {
public:
    // Link a set of orders into a new OCO group and return its groupId.
    // Members are expected to be distinct and not already linked; duplicate
    // ids within the vector collapse to a single membership, and a group
    // that ends up with fewer than two live members carries no linkage
    // (a lone order is just a normal order). Groups with no usable members
    // are not registered and return 0 (never a valid groupId).
    uint64_t registerOco(const std::vector<OrderId>& members);

    // Two-leg convenience overload for the common take-profit/stop-loss pair.
    uint64_t registerOco(OrderId a, OrderId b);

    // Notify that `id` has had its first execution (the order "wins" its
    // group). Returns the sibling OrderIds the caller must now cancel.
    // Returns empty if `id` is unlinked, already removed, or its group is
    // already resolved (idempotent). The winning id and all returned
    // siblings are removed from the registry; the group is resolved.
    std::vector<OrderId> onExecuted(OrderId id);

    // Notify that `id` terminated WITHOUT winning (cancel/reject/expiry).
    // Deregisters `id` from its group; no siblings are cancelled. Safe to
    // call for an unknown or already-removed id (no-op).
    void onCanceled(OrderId id);

    // True if `id` currently belongs to a live (unresolved) OCO group.
    bool isLinked(OrderId id) const;

    // Number of groups that still have at least one live member. A group
    // disappears from this count once it is resolved (a winner executed)
    // or once all its members have been deregistered.
    size_t activeGroupCount() const;

private:
    struct Group {
        // Live members only. Removing the last member empties the group;
        // a single-member group is "dissolved" but harmless (its lone
        // survivor produces no siblings on execution).
        std::unordered_set<OrderId> members;
    };

    // Drop `id` from its group bookkeeping. If the group becomes empty it
    // is erased so activeGroupCount() reflects reality. Returns true if the
    // id was present. Shared by onExecuted and onCanceled.
    bool removeMember(OrderId id);

    // groupId 0 is reserved as "no group" so callers can treat a 0 return
    // from registerOco as "not linked". Real ids start at 1.
    uint64_t nextGroupId_{1};

    std::unordered_map<uint64_t, Group> groups_;
    // Reverse index: which group (if any) an order belongs to. An entry
    // exists iff the order is a live member of an unresolved group.
    std::unordered_map<OrderId, uint64_t> orderToGroup_;
};

}  // namespace OrderMatcher
