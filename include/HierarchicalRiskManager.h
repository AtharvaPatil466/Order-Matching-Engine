#pragma once

// HierarchicalRiskManager — multi-tier pre-trade risk limits.
//
// Tier-1 upgrade: engine-agnostic hierarchical risk control.
//
// Each trading participant (a ParticipantId) is mapped into a four-level
// risk hierarchy:
//
//   Trader   (the participant itself, most specific)
//     └─ Strategy
//          └─ Account
//               └─ Firm    (broadest, aggregates many traders)
//
// Limits (TierLimits) are configured independently at every tier and on
// every entity. A pre-trade check evaluates the incoming order against all
// four tiers the trader belongs to, from most specific (Trader) to broadest
// (Firm), and returns the FIRST breach found together with the tier that
// breached.
//
// The key feature is AGGREGATION. A firm's net position is the running sum
// across ALL traders mapped to that firm. onFill() propagates each executed
// fill's signed quantity delta (Buy adds, Sell subtracts) to the trader's
// strategy, account, AND firm entities. So an order that is fine for the
// trader's own limit can still be rejected because it would push the firm
// aggregate net position past the firm cap — reported as breachedTier=Firm.
//
// This component depends ONLY on Types.h and standard containers; it does
// not know about OrderBook, MatchingEngine, or any concrete order type. It
// is therefore reusable as a standalone pre-trade gate in front of any
// engine. (Note: it deliberately does NOT reuse OrderBook.h's flat
// RiskLimits struct, to keep this engine-agnostic — TierLimits is its own
// definition.)
//
// An unmapped participant (one for which mapParticipant() was never called)
// belongs to no tier and therefore has no limits: check() always allows it
// and onFill() is a no-op for it.

#include "Types.h"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace OrderMatcher {

// The four risk tiers, ordered from most specific to broadest. The numeric
// values double as indices into the per-trader entity-id array.
enum class RiskTier : uint8_t {
    Trader   = 0,
    Strategy = 1,
    Account  = 2,
    Firm     = 3
};

// Risk limits configured for a single entity at a single tier. A field set
// to 0 (or 0.0) means that particular limit is disabled / unlimited.
struct TierLimits {
    Quantity maxOrderSize     = 0;    // 0 = unlimited
    Price    maxOrderNotional = 0;    // 0 = unlimited; checked as price*qty
    Quantity maxNetPosition   = 0;    // 0 = unlimited; cap on |aggregate net position|
    double   maxPctFromRef    = 0.0;  // fat-finger: reject if |price-ref|/ref exceeds this (0 = off)
};

// Outcome of a pre-trade check.
struct RiskDecision {
    bool         allowed      = true;
    RejectReason reason       = RejectReason::None;   // RiskLimitBreached on a breach
    RiskTier     breachedTier = RiskTier::Trader;     // meaningful iff !allowed
};

class HierarchicalRiskManager {
public:
    // Number of tiers in the hierarchy.
    static constexpr std::size_t kNumTiers = 4;

    // Map a trader (ParticipantId) to its strategy / account / firm entity
    // ids. Re-mapping an already-mapped trader overwrites its mapping.
    void mapParticipant(ParticipantId trader, uint64_t strategyId,
                        uint64_t accountId, uint64_t firmId);

    // Configure the limits for one entity at one tier. The entityId is the
    // id that mapParticipant() associates with that tier (for the Trader
    // tier, the entityId is the ParticipantId itself).
    void setLimits(RiskTier tier, uint64_t entityId, const TierLimits& limits);

    // Pre-trade check across ALL FOUR tiers the trader belongs to:
    //   * maxOrderSize / maxOrderNotional against THIS order;
    //   * fat-finger: if referencePrice != 0 and maxPctFromRef > 0, reject
    //     when |price - referencePrice| / referencePrice > maxPctFromRef;
    //   * projected aggregate net position: (current tier net position
    //     +/- qty, Buy adds, Sell subtracts) magnitude must not exceed
    //     maxNetPosition.
    // Returns the FIRST breach found, checking from most-specific (Trader)
    // to broadest (Firm), and reports which tier breached. An unmapped
    // participant is always allowed.
    RiskDecision check(ParticipantId trader, Side side, Price price,
                       Quantity qty, Price referencePrice) const;

    // Commit an executed fill: update the running net position at EVERY tier
    // the trader belongs to (Buy += qty, Sell -= qty). No-op for an unmapped
    // participant.
    void onFill(ParticipantId trader, Side side, Price price, Quantity qty);

    // Inspect the running net position of an entity at a tier (for tests /
    // monitoring). Returns 0 for an entity that has never been filled.
    int64_t netPosition(RiskTier tier, uint64_t entityId) const;

private:
    // Per-tier state for a single entity: its configured limits and running
    // net position. Both live in the same map so a tier lookup is one probe.
    struct EntityState {
        TierLimits limits{};
        int64_t    netPosition = 0;
    };

    // The entity ids a trader belongs to, indexed by RiskTier value. Index 0
    // (Trader) holds the ParticipantId itself.
    using EntityIds = std::array<uint64_t, kNumTiers>;

    // Convert a signed quantity delta for a side: Buy is positive.
    static int64_t signedDelta(Side side, Quantity qty);

    // Evaluate one tier's limits against an order. Returns true on breach.
    static bool tierBreached(const EntityState& state, Side side, Price price,
                             Quantity qty, Price referencePrice);

    // trader -> entity ids for each tier.
    std::unordered_map<ParticipantId, EntityIds> participants_;

    // One limits+position map per tier, keyed by entity id.
    std::array<std::unordered_map<uint64_t, EntityState>, kNumTiers> tiers_;
};

}  // namespace OrderMatcher
