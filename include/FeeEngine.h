#pragma once

// FeeEngine — engine-agnostic maker-taker fee/rebate accrual.
//
// Computes per-fill maker and taker fees from a basis-points schedule and
// accrues a running net total per participant. Kept deliberately decoupled
// from the matching engine and order book: it depends only on the scalar
// aliases in Types.h so it can be unit-tested and reused (clearing, billing,
// reporting) without dragging in book/engine internals.
//
// Fee model:
//   notional = price * qty
//   fee      = llround(notional * bps / 10000.0)
// Maker bps are typically negative (a rebate paid TO the maker for providing
// liquidity); taker bps are typically positive (a fee charged to the taker for
// removing liquidity). Both fields are signed so either side can be a charge
// or a credit. Each side resolves its own schedule independently: a
// per-participant tier override if one is set, otherwise the venue default.
//
// The notional is widened to double before the bps multiply so a large
// price*qty product (each is 64-bit) does not overflow int64 mid-computation.
// Accrued totals stay in int64 minor currency units; double is used only for
// the rounding arithmetic, never as the system of record.

#include "Types.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace OrderMatcher {

struct FeeSchedule {
    double makerBps = 0.0;  // basis points on notional; negative = rebate paid to the maker
    double takerBps = 0.0;  // basis points on notional; positive = fee charged to the taker
};

struct FillFees {
    int64_t makerFee = 0;   // signed (negative = rebate to maker)
    int64_t takerFee = 0;   // signed (positive = fee charged to taker)
};

class FeeEngine {
public:
    void setDefaultSchedule(const FeeSchedule& s) {
        defaultSchedule_ = s;
    }

    // Tier override: a participant with an explicit schedule uses it for both
    // its maker and taker activity in place of the venue default.
    void setParticipantSchedule(ParticipantId p, const FeeSchedule& s) {
        schedules_[p] = s;
    }

    // Compute the fees for ONE fill and ACCRUE them to each side's running
    // total. Each side uses its own resolved schedule. Returns the per-fill
    // fees so the caller can attach them to a trade/execution report.
    FillFees applyFill(ParticipantId maker, ParticipantId taker, Price price, Quantity qty) {
        // Widen via double so the product cannot overflow int64 for large
        // price*qty before the basis-points scaling is applied.
        const double notional = static_cast<double>(price) * static_cast<double>(qty);

        FillFees fees;
        fees.makerFee = llround(notional * scheduleFor(maker).makerBps / 10000.0);
        fees.takerFee = llround(notional * scheduleFor(taker).takerBps / 10000.0);

        accrued_[maker] += fees.makerFee;
        accrued_[taker] += fees.takerFee;
        return fees;
    }

    // Net accrued for a participant: taker fees add (charges), maker rebates
    // subtract (credits). Unknown participants have accrued nothing.
    int64_t accruedFee(ParticipantId p) const {
        auto it = accrued_.find(p);
        return it == accrued_.end() ? 0 : it->second;
    }

    void resetParticipant(ParticipantId p) {
        accrued_.erase(p);
    }

    void resetAll() {
        accrued_.clear();
    }

private:
    // A participant-specific tier overrides the venue default entirely.
    const FeeSchedule& scheduleFor(ParticipantId p) const {
        auto it = schedules_.find(p);
        return it == schedules_.end() ? defaultSchedule_ : it->second;
    }

    FeeSchedule defaultSchedule_{};
    std::unordered_map<ParticipantId, FeeSchedule> schedules_;
    std::unordered_map<ParticipantId, int64_t> accrued_;
};

}  // namespace OrderMatcher
