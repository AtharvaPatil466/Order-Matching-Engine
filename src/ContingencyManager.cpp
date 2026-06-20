#include "ContingencyManager.h"

namespace OrderMatcher {

uint64_t ContingencyManager::registerOco(const std::vector<OrderId>& members) {
    // Build the live member set first so duplicate ids collapse and we can
    // size-check before allocating a groupId. We skip ids already linked to
    // another group: re-linking would silently override prior linkage, which
    // is almost certainly a caller bug, so we treat the first registration as
    // authoritative and ignore the conflicting id rather than corrupt state.
    Group group;
    for (OrderId id : members) {
        if (orderToGroup_.find(id) != orderToGroup_.end()) {
            continue;  // already linked elsewhere — do not steal it
        }
        group.members.insert(id);
    }

    // An OCO group needs at least two live members to mean anything. Fewer
    // than two carries no linkage, so we register nothing and signal "no
    // group" with the reserved id 0.
    if (group.members.size() < 2) {
        return 0;
    }

    const uint64_t groupId = nextGroupId_++;
    for (OrderId id : group.members) {
        orderToGroup_[id] = groupId;
    }
    groups_.emplace(groupId, std::move(group));
    return groupId;
}

uint64_t ContingencyManager::registerOco(OrderId a, OrderId b) {
    return registerOco(std::vector<OrderId>{a, b});
}

std::vector<OrderId> ContingencyManager::onExecuted(OrderId id) {
    auto it = orderToGroup_.find(id);
    if (it == orderToGroup_.end()) {
        // Unlinked, already removed, or in an already-resolved group:
        // nothing to cancel (idempotent no-op).
        return {};
    }

    const uint64_t groupId = it->second;
    auto groupIt = groups_.find(groupId);
    // The reverse index and the group table are maintained together, so a
    // dangling reverse entry should never happen; guard defensively anyway.
    if (groupIt == groups_.end()) {
        orderToGroup_.erase(it);
        return {};
    }

    // `id` wins the group. Every OTHER live member must be cancelled by the
    // caller. We collect the siblings, then tear the whole group down so the
    // group is RESOLVED and any later notification about it is a no-op.
    std::vector<OrderId> siblings;
    const auto& memberSet = groupIt->second.members;
    siblings.reserve(memberSet.size() > 0 ? memberSet.size() - 1 : 0);
    for (OrderId member : memberSet) {
        if (member != id) {
            siblings.push_back(member);
        }
        orderToGroup_.erase(member);
    }
    groups_.erase(groupIt);
    return siblings;
}

void ContingencyManager::onCanceled(OrderId id) {
    // A non-winning termination just removes the leg; siblings stay live.
    removeMember(id);
}

bool ContingencyManager::isLinked(OrderId id) const {
    return orderToGroup_.find(id) != orderToGroup_.end();
}

size_t ContingencyManager::activeGroupCount() const {
    return groups_.size();
}

bool ContingencyManager::removeMember(OrderId id) {
    auto it = orderToGroup_.find(id);
    if (it == orderToGroup_.end()) {
        return false;  // unknown / already removed — safe no-op
    }

    const uint64_t groupId = it->second;
    orderToGroup_.erase(it);

    auto groupIt = groups_.find(groupId);
    if (groupIt != groups_.end()) {
        groupIt->second.members.erase(id);
        // A group needs >= 2 live members to mean anything. The moment a
        // non-winning termination drops it below that, the group is
        // DISSOLVED: any lone survivor becomes a normal, unlinked order
        // (not auto-cancelled, and producing no siblings if it later
        // executes). We unlink the survivor and erase the group so both
        // isLinked() and activeGroupCount() reflect the dissolution.
        if (groupIt->second.members.size() < 2) {
            for (OrderId survivor : groupIt->second.members) {
                orderToGroup_.erase(survivor);
            }
            groups_.erase(groupIt);
        }
    }
    return true;
}

}  // namespace OrderMatcher
