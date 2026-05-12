#pragma once

// BatchRiskValidator — batch risk check with configurable batch size.
//
// Roadmap Phase 1, Week 2: OTR and Risk Check Optimization
//
// Instead of checking one order against risk limits, we accumulate a
// batch of orders and validate them in a tight loop. This improves
// instruction-level parallelism and cache locality. The batch size
// is configurable but defaults to 8.
//
// Usage:
//   BatchRiskValidator<8> validator;
//   validator.enqueue(price, qty, side, participantState);
//   // ... more orders ...
//   auto results = validator.validateBatch();
//   // results[i] = true means order i passed risk checks

#include "ParticipantRiskState.h"
#include "Types.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace OrderMatcher {

template <size_t BatchSize = 8>
class BatchRiskValidator {
public:
    struct PendingCheck {
        Price    price{0};
        Quantity qty{0};
        Side     side{Side::Buy};
        const ParticipantRiskState* state{nullptr};
        bool     occupied{false};
    };

    BatchRiskValidator() = default;

    // Enqueue a single order for batch validation.
    // Returns the slot index, or -1 if the batch is full.
    int enqueue(Price price, Quantity qty, Side side,
                const ParticipantRiskState* state) {
        if (count_ >= BatchSize) {
            return -1;
        }
        auto& slot = pending_[count_];
        slot.price = price;
        slot.qty = qty;
        slot.side = side;
        slot.state = state;
        slot.occupied = true;
        return static_cast<int>(count_++);
    }

    // Validate all enqueued orders in a tight loop.
    // Returns a fixed-size array of results. Only indices [0, count_) are valid.
    std::array<bool, BatchSize> validateBatch() {
        std::array<bool, BatchSize> results{};

        // Tight loop — compiler can unroll and exploit ILP
        for (size_t i = 0; i < count_; ++i) {
            const auto& check = pending_[i];
            if (!check.state) {
                results[i] = true;  // No risk state = no limits
                continue;
            }
            results[i] = check.state->checkOrder(check.price, check.qty, check.side);
        }

        return results;
    }

    // Validate a single order immediately (bypass batching).
    static bool validateSingle(Price price, Quantity qty, Side side,
                               const ParticipantRiskState* state) {
        if (!state) return true;
        return state->checkOrder(price, qty, side);
    }

    // Reset the batch for reuse.
    void reset() {
        count_ = 0;
        // No need to clear the array — we track count_
    }

    size_t count() const { return count_; }
    bool full() const { return count_ >= BatchSize; }
    bool empty() const { return count_ == 0; }

    static constexpr size_t batchSize() { return BatchSize; }

private:
    std::array<PendingCheck, BatchSize> pending_{};
    size_t count_{0};
};

// ─── OTR Key: composite participant×symbol lookup ───────────────────────
//
// Roadmap Phase 1, Week 2: OTR via FlatHashMap with composite key.
// The key is a 64-bit composite of participant ID (high 32) and
// symbol ID (low 32). The hash is the identity function since
// participant IDs are already well-distributed uint64_t values
// combined with uint32_t symbol IDs.

struct OTRKey {
    uint64_t value;

    OTRKey() : value(0) {}
    OTRKey(ParticipantId participant, SymbolId symbol)
        : value((static_cast<uint64_t>(participant) << 32) |
                static_cast<uint64_t>(symbol)) {}

    bool operator==(const OTRKey& other) const { return value == other.value; }
    bool operator!=(const OTRKey& other) const { return value != other.value; }
};

struct OTRKeyHash {
    size_t operator()(const OTRKey& key) const {
        // Multiplicative hash (Knuth's golden ratio)
        return static_cast<size_t>(key.value * 0x9E3779B97F4A7C15ULL);
    }
};

}  // namespace OrderMatcher
