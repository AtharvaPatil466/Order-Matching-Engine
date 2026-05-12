#pragma once

// WashTradeDetector — beneficial-owner-based wash trade detection.
//
// Roadmap Phase 4, Week 13: Wash Trade Detection
//
// Each participant maps to one or more beneficial owners. Before
// executing a trade, check if both sides share any beneficial owner.
// If yes, flag the trade in the audit log and optionally reject.
//
// The check is a bitset intersection: represent each participant's
// owners as a 256-bit bitset, AND the bitsets, check for any set bit.
// This is O(1) and SIMD-friendly (4 × uint64_t AND + OR).

#include "Types.h"
#include "FlatHashMap.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace OrderMatcher {

// 256-bit bitset for beneficial owner tracking.
// Supports up to 256 distinct beneficial owner IDs.
struct OwnerBitset {
    std::array<uint64_t, 4> words{};  // 4 × 64 = 256 bits

    void set(uint8_t ownerId) {
        words[ownerId / 64] |= (1ULL << (ownerId % 64));
    }

    void clear(uint8_t ownerId) {
        words[ownerId / 64] &= ~(1ULL << (ownerId % 64));
    }

    bool test(uint8_t ownerId) const {
        return (words[ownerId / 64] & (1ULL << (ownerId % 64))) != 0;
    }

    // O(1) intersection check — returns true if any owner overlaps
    bool intersects(const OwnerBitset& other) const {
        // 4 ANDs + 3 ORs → single branch
        return ((words[0] & other.words[0]) |
                (words[1] & other.words[1]) |
                (words[2] & other.words[2]) |
                (words[3] & other.words[3])) != 0;
    }

    bool empty() const {
        return (words[0] | words[1] | words[2] | words[3]) == 0;
    }

    void reset() {
        words.fill(0);
    }
};

static_assert(sizeof(OwnerBitset) == 32, "OwnerBitset should be 32 bytes");

enum class WashTradeAction : uint8_t {
    Allow = 0,    // No wash trade concern
    Flag  = 1,    // Log but allow the trade
    Reject = 2    // Reject the trade
};

class WashTradeDetector {
public:
    WashTradeDetector() = default;

    // Register a beneficial owner for a participant.
    // Owner IDs are 0–255.
    void addBeneficialOwner(ParticipantId participant, uint8_t ownerId) {
        ownerMap_[participant].set(ownerId);
    }

    // Remove a beneficial owner mapping.
    void removeBeneficialOwner(ParticipantId participant, uint8_t ownerId) {
        auto* bitset = ownerMap_.find(participant);
        if (bitset) {
            bitset->clear(ownerId);
        }
    }

    // Set the full owner bitset for a participant (bulk load).
    void setOwners(ParticipantId participant, const OwnerBitset& owners) {
        ownerMap_[participant] = owners;
    }

    // Check if a trade between two participants is a potential wash trade.
    // Returns true if both sides share at least one beneficial owner.
    bool isWashTrade(ParticipantId buyer, ParticipantId seller) const {
        if (buyer == seller) return true;  // Trivially a wash trade

        const auto* buyerOwners = ownerMap_.find(buyer);
        const auto* sellerOwners = ownerMap_.find(seller);

        // If either participant has no registered owners, no wash trade
        if (!buyerOwners || !sellerOwners) return false;

        return buyerOwners->intersects(*sellerOwners);
    }

    // Full check with configurable action.
    WashTradeAction check(ParticipantId buyer, ParticipantId seller) const {
        if (!isWashTrade(buyer, seller)) {
            return WashTradeAction::Allow;
        }
        return defaultAction_;
    }

    // Set default action on wash trade detection.
    void setDefaultAction(WashTradeAction action) {
        defaultAction_ = action;
    }

    WashTradeAction getDefaultAction() const { return defaultAction_; }

    // Check if any owners are registered (detector is active).
    bool isActive() const { return !ownerMap_.empty(); }

private:
    FlatHashMap<ParticipantId, OwnerBitset> ownerMap_{256};
    WashTradeAction defaultAction_{WashTradeAction::Flag};
};

}  // namespace OrderMatcher
