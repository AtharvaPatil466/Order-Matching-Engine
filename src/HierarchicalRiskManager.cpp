// HierarchicalRiskManager — implementation.
//
// See include/HierarchicalRiskManager.h for the design rationale. The core
// idea: limits and running net positions live in one map per tier, keyed by
// entity id. A trader's mapping records the entity id it belongs to at each
// tier. check() walks the tiers most-specific first and returns the first
// breach; onFill() applies the signed fill delta to every tier the trader
// belongs to, which is what makes a firm's net position the aggregate across
// all of its traders.

#include "HierarchicalRiskManager.h"

#include <cmath>
#include <cstdlib>

namespace OrderMatcher {

int64_t HierarchicalRiskManager::signedDelta(Side side, Quantity qty) {
    const int64_t q = static_cast<int64_t>(qty);
    return (side == Side::Buy) ? q : -q;
}

void HierarchicalRiskManager::mapParticipant(ParticipantId trader,
                                             uint64_t strategyId,
                                             uint64_t accountId,
                                             uint64_t firmId) {
    EntityIds ids{};
    ids[static_cast<std::size_t>(RiskTier::Trader)]   = trader;
    ids[static_cast<std::size_t>(RiskTier::Strategy)] = strategyId;
    ids[static_cast<std::size_t>(RiskTier::Account)]  = accountId;
    ids[static_cast<std::size_t>(RiskTier::Firm)]     = firmId;
    participants_[trader] = ids;
}

void HierarchicalRiskManager::setLimits(RiskTier tier, uint64_t entityId,
                                        const TierLimits& limits) {
    auto& table = tiers_[static_cast<std::size_t>(tier)];
    table[entityId].limits = limits;  // preserves any existing netPosition
}

bool HierarchicalRiskManager::tierBreached(const EntityState& state, Side side,
                                           Price price, Quantity qty,
                                           Price referencePrice) {
    const TierLimits& lim = state.limits;

    // --- Max single order size ---------------------------------------------
    if (lim.maxOrderSize > 0 && qty > lim.maxOrderSize) {
        return true;
    }

    // --- Max order notional (price * qty) ----------------------------------
    // Guard against negative prices and overflow by computing in long double
    // for the comparison; notionals here are well within long double's exact
    // integer range for realistic prices/quantities.
    if (lim.maxOrderNotional > 0) {
        const long double notional =
            static_cast<long double>(price) * static_cast<long double>(qty);
        if (notional > static_cast<long double>(lim.maxOrderNotional)) {
            return true;
        }
    }

    // --- Fat-finger: distance from reference price -------------------------
    if (referencePrice != 0 && lim.maxPctFromRef > 0.0) {
        const double ref  = static_cast<double>(referencePrice);
        const double diff = std::fabs(static_cast<double>(price) - ref);
        const double pct  = diff / std::fabs(ref);
        if (pct > lim.maxPctFromRef) {
            return true;
        }
    }

    // --- Projected aggregate net position ----------------------------------
    if (lim.maxNetPosition > 0) {
        const int64_t projected = state.netPosition + signedDelta(side, qty);
        const uint64_t magnitude =
            (projected >= 0) ? static_cast<uint64_t>(projected)
                             : static_cast<uint64_t>(-projected);
        if (magnitude > lim.maxNetPosition) {
            return true;
        }
    }

    return false;
}

RiskDecision HierarchicalRiskManager::check(ParticipantId trader, Side side,
                                            Price price, Quantity qty,
                                            Price referencePrice) const {
    RiskDecision decision;  // allowed = true by default

    auto pit = participants_.find(trader);
    if (pit == participants_.end()) {
        // Unmapped participant: no hierarchy, no limits.
        return decision;
    }

    const EntityIds& ids = pit->second;

    // Walk tiers from most specific (Trader = 0) to broadest (Firm = 3) and
    // return the first breach.
    for (std::size_t t = 0; t < kNumTiers; ++t) {
        const auto& table = tiers_[t];
        auto eit = table.find(ids[t]);
        if (eit == table.end()) {
            continue;  // no limits configured for this entity at this tier
        }
        if (tierBreached(eit->second, side, price, qty, referencePrice)) {
            decision.allowed      = false;
            decision.reason       = RejectReason::RiskLimitBreached;
            decision.breachedTier = static_cast<RiskTier>(t);
            return decision;
        }
    }

    return decision;
}

void HierarchicalRiskManager::onFill(ParticipantId trader, Side side,
                                     Price /*price*/, Quantity qty) {
    auto pit = participants_.find(trader);
    if (pit == participants_.end()) {
        return;  // unmapped: nothing to update
    }

    const EntityIds& ids = pit->second;
    const int64_t delta = signedDelta(side, qty);

    // Propagate the position delta to EVERY tier the trader belongs to. This
    // is what aggregates per-trader fills up into account/firm totals. An
    // entity with no configured limits still accrues a net position so that
    // its limits can be turned on later and reflect history.
    for (std::size_t t = 0; t < kNumTiers; ++t) {
        tiers_[t][ids[t]].netPosition += delta;
    }
}

int64_t HierarchicalRiskManager::netPosition(RiskTier tier,
                                             uint64_t entityId) const {
    const auto& table = tiers_[static_cast<std::size_t>(tier)];
    auto eit = table.find(entityId);
    return (eit == table.end()) ? 0 : eit->second.netPosition;
}

}  // namespace OrderMatcher
