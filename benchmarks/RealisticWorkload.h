// RealisticWorkload — vendored, header-only order-flow generator for benchmarks.
//
// ZERO external dependencies. Shared by the realistic-flow, coordinated-omission
// and cold-cache benchmarks so they exercise the SAME realistic distribution
// instead of the seed=42 / 100%-marketable stream used by the PGO number.
//
// The stream models a single symbol with:
//   • Poisson arrivals            — inter-arrival gap ~ Exponential(lambda),
//                                    lambda in orders/sec (configurable).
//   • Power-law order sizes       — Pareto(alpha) sizes, clamped to [1, maxQty];
//                                    most orders small, a heavy tail of big ones.
//   • Random-walk mid price       — mid takes bounded ± steps each event.
//   • ~15% marketable submissions — priced to cross the opposing book (fill).
//   • ~65% of resting orders      — scheduled to be cancelled before they fill.
//
// The marketable fraction and resting-cancel fraction are enforced by
// construction (Bernoulli draws at generation time), so they are exact in
// expectation and deterministic for a given seed. The *realized* fill rate is
// still book-dependent (a marketable order only fills if liquidity rests), so
// the benchmarks measure and report the realized numbers rather than asserting
// the targets.
#pragma once

#include "Types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <random>
#include <vector>

namespace bench {

using namespace OrderMatcher;

struct WorkloadConfig {
    uint64_t seed          = 12345;
    size_t   newOrders     = 50000;  // number of NEW submissions (cancels extra)
    double   lambdaPerSec  = 100000; // Poisson arrival rate (orders/sec)
    double   marketableFrac = 0.15;  // fraction of submissions that cross → fill
    double   restingCancelFrac = 0.65; // fraction of resting orders cancelled
    double   paretoAlpha   = 1.5;    // power-law exponent for order sizes
    Quantity maxQty        = 10000;  // clamp for the heavy tail
    Price    startMid      = 10000000;
    Price    walkStep      = 250;    // max ± mid move per event
    Price    passiveOffset = 5000;   // how far passive orders rest from mid
    Price    crossOffset   = 5000;   // how far marketable orders reach across
    uint32_t minCancelDelay = 1;     // resting lifetime range, in stream slots
    uint32_t maxCancelDelay = 2000;
    ParticipantId numParticipants = 20;
};

struct WorkloadOp {
    enum class Kind : uint8_t { New, Cancel };
    Kind          kind;
    OrderId       orderId;
    ParticipantId participantId;
    Side          side;       // valid for New
    Price         price;      // valid for New
    Quantity      qty;        // valid for New
    bool          marketable; // valid for New (design intent, not a guarantee)
    uint64_t      gapNs;      // Poisson inter-arrival gap before this op
};

// Generates a deterministic op stream (New + interleaved Cancel) matching the
// configured distribution. Cancels for resting orders are scheduled a random
// number of stream slots into the future, modelling "cancelled before fill".
class WorkloadGenerator {
public:
    explicit WorkloadGenerator(const WorkloadConfig& cfg)
        : cfg_(cfg), rng_(cfg.seed) {
        generate();
    }

    const std::vector<WorkloadOp>& ops() const { return ops_; }
    size_t marketableCount() const { return marketableCount_; }
    size_t passiveCount()    const { return passiveCount_; }
    size_t scheduledCancels() const { return scheduledCancels_; }

private:
    // A pending cancel: fire once the stream slot reaches `slot`.
    struct PendingCancel {
        uint64_t slot;
        OrderId  id;
        bool operator>(const PendingCancel& o) const { return slot > o.slot; }
    };

    void generate() {
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        std::uniform_int_distribution<int64_t> walk(-cfg_.walkStep, cfg_.walkStep);
        std::uniform_int_distribution<ParticipantId> pid(1, cfg_.numParticipants);
        std::uniform_int_distribution<uint32_t> delayDist(cfg_.minCancelDelay,
                                                          cfg_.maxCancelDelay);

        ops_.reserve(cfg_.newOrders * 2);
        std::priority_queue<PendingCancel, std::vector<PendingCancel>,
                            std::greater<PendingCancel>> pending;

        Price mid = cfg_.startMid;
        OrderId nextId = 1;
        size_t placed = 0;
        uint64_t slot = 0;

        while (placed < cfg_.newOrders || !pending.empty()) {
            // Fire any cancels whose scheduled slot has arrived.
            while (!pending.empty() && pending.top().slot <= slot) {
                emitCancel(pending.top().id, pid(rng_), u01);
                pending.pop();
            }

            if (placed < cfg_.newOrders) {
                // Random-walk the mid within a sane floor.
                mid = std::max<Price>(cfg_.passiveOffset + 100, mid + walk(rng_));

                WorkloadOp op;
                op.kind = WorkloadOp::Kind::New;
                op.orderId = nextId++;
                op.participantId = pid(rng_);
                op.side = (rng_() & 1u) ? Side::Buy : Side::Sell;
                op.qty = paretoQty(u01);
                op.marketable = u01(rng_) < cfg_.marketableFrac;
                op.price = priceFor(op.side, op.marketable, mid);
                op.gapNs = poissonGapNs(u01);
                ops_.push_back(op);

                if (op.marketable) {
                    ++marketableCount_;
                } else {
                    ++passiveCount_;
                    // Schedule ~restingCancelFrac of resting orders for cancel.
                    if (u01(rng_) < cfg_.restingCancelFrac) {
                        pending.push({slot + delayDist(rng_), op.orderId});
                        ++scheduledCancels_;
                    }
                }
                ++placed;
            }
            ++slot;
        }
    }

    void emitCancel(OrderId id, ParticipantId pid,
                    std::uniform_real_distribution<double>& u01) {
        WorkloadOp op;
        op.kind = WorkloadOp::Kind::Cancel;
        op.orderId = id;
        op.participantId = pid;
        op.side = Side::Buy;
        op.price = 0;
        op.qty = 0;
        op.marketable = false;
        op.gapNs = poissonGapNs(u01);
        ops_.push_back(op);
    }

    // Marketable orders reach across the mid; passive orders rest away from it.
    Price priceFor(Side side, bool marketable, Price mid) const {
        Price off = marketable ? cfg_.crossOffset : cfg_.passiveOffset;
        if (side == Side::Buy) {
            return marketable ? mid + off : std::max<Price>(100, mid - off);
        }
        return marketable ? std::max<Price>(100, mid - off) : mid + off;
    }

    // Pareto(alpha) draw: qty = floor((1-U)^(-1/alpha)), clamped to [1, maxQty].
    Quantity paretoQty(std::uniform_real_distribution<double>& u01) {
        double u = 1.0 - u01(rng_); // in (0,1]
        double v = std::pow(u, -1.0 / cfg_.paretoAlpha);
        Quantity q = static_cast<Quantity>(v);
        if (q < 1) q = 1;
        if (q > cfg_.maxQty) q = cfg_.maxQty;
        return q;
    }

    // Exponential inter-arrival gap for a Poisson process of rate lambda.
    uint64_t poissonGapNs(std::uniform_real_distribution<double>& u01) {
        if (cfg_.lambdaPerSec <= 0.0) return 0;
        double u = 1.0 - u01(rng_); // avoid log(0)
        double gapSec = -std::log(u) / cfg_.lambdaPerSec;
        return static_cast<uint64_t>(gapSec * 1e9);
    }

    WorkloadConfig cfg_;
    std::mt19937_64 rng_;
    std::vector<WorkloadOp> ops_;
    size_t marketableCount_{0};
    size_t passiveCount_{0};
    size_t scheduledCancels_{0};
};

} // namespace bench
