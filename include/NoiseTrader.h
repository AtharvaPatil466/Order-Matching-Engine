#pragma once

#include "SimulationParticipant.h"
#include "MatchingEngine.h"
#include "Types.h"

#include <deque>
#include <limits>
#include <random>
#include <cstdint>

namespace OrderMatcher {

// ----------------------------------------------------------------------------
// NoiseTrader — submits random limit orders on a Poisson arrival process.
//
// Behaviour per tick
//   • With probability lambdaPerTick, submit a new limit order:
//       – Side:     random buy or sell
//       – Price:    mid ± U[1, maxSpreadTicks] ticks
//               (if no mid is available, uses a configurable fallback price)
//       – Quantity: U[minQty, maxQty]
//       – TIF:      GTC
//   • With probability cancelProb, cancel the participant's oldest resting order.
//
// Reproducibility:  std::mt19937 seeded at construction; deterministic given seed.
// ----------------------------------------------------------------------------
class NoiseTrader : public SimulationParticipant {
public:
    struct Config {
        double   lambdaPerTick  = 0.3;    // Poisson arrival rate (prob per tick)
        double   cancelProb     = 0.15;   // Probability of cancelling oldest order each tick
        int64_t  maxSpreadTicks = 10;     // Half-spread: price offset drawn from U[1, maxSpreadTicks]
        Quantity minQty         = 1;
        Quantity maxQty         = 100;
        Price    fallbackMid    = 100 * PRICE_PRECISION; // Used when book has no quotes
    };

    // Constructor with default config
    explicit NoiseTrader(ParticipantId pid, uint64_t seed = 42)
        : pid_(pid), cfg_(), rng_(seed) {}

    // Constructor with explicit config
    NoiseTrader(ParticipantId pid, Config cfg, uint64_t seed = 42)
        : pid_(pid), cfg_(cfg), rng_(seed) {}

    ParticipantId id() const override { return pid_; }

    void onTick(uint64_t  /*tickNum*/,
                Price     bestBid,
                Price     bestAsk,
                Quantity  /*bidDepth*/,
                Quantity  /*askDepth*/) override
    {
        if (!engine_) return;

        // ── Cancel oldest resting order (probabilistic) ──────────────────
        if (!restingOrders_.empty()) {
            std::bernoulli_distribution cancelDist(cfg_.cancelProb);
            if (cancelDist(rng_)) {
                OrderId toCancel = restingOrders_.front();
                restingOrders_.pop_front();
                engine_->submitCancel(symbol_, toCancel);
            }
        }

        // ── Maybe submit a new order ─────────────────────────────────────
        std::bernoulli_distribution arriveDist(cfg_.lambdaPerTick);
        if (!arriveDist(rng_)) return;

        // Normalise: getBestAsk() returns MAX_PRICE when there are no asks.
        // Treat MAX_PRICE as "no ask" (same as 0 for bids).
        constexpr Price NO_ASK = std::numeric_limits<Price>::max();
        if (bestAsk == NO_ASK) bestAsk = 0;

        // Compute mid price
        Price mid = 0;
        if (bestBid > 0 && bestAsk > 0) {
            mid = (bestBid + bestAsk) / 2;
        } else if (bestBid > 0) {
            mid = bestBid;
        } else if (bestAsk > 0) {
            mid = bestAsk;
        } else {
            mid = cfg_.fallbackMid;
        }

        // Random side
        std::bernoulli_distribution sideDist(0.5);
        Side side = sideDist(rng_) ? Side::Buy : Side::Sell;

        // Random offset in ticks (each tick = 1 unit of Price; PRICE_PRECISION
        // encodes 4 decimal places, but simulation uses whole-unit ticks here)
        std::uniform_int_distribution<int64_t> offsetDist(1, cfg_.maxSpreadTicks);
        int64_t offset = offsetDist(rng_);

        Price price = (side == Side::Buy) ? (mid - offset) : (mid + offset);
        if (price <= 0) price = 1;  // guard against non-positive price

        // Random quantity
        if (cfg_.minQty < 1) cfg_.minQty = 1;
        if (cfg_.maxQty < cfg_.minQty) cfg_.maxQty = cfg_.minQty;
        std::uniform_int_distribution<Quantity> qtyDist(cfg_.minQty, cfg_.maxQty);
        Quantity qty = qtyDist(rng_);
        if (qty == 0) qty = 1;

        OrderId oid = nextOrderId_++;
        auto result = engine_->submitOrder(symbol_, oid, pid_, side, price, qty,
                                           OrderType::Limit,
                                           /*stopPrice=*/0,
                                           /*displayQty=*/0,
                                           TimeInForce::GTC);
        if (result.isAccepted()) {
            restingOrders_.push_back(oid);
        }
    }

private:
    ParticipantId          pid_;
    Config                 cfg_;
    std::mt19937           rng_;
    OrderId                nextOrderId_{1'000'000};  // Namespace orders away from other participants
    std::deque<OrderId>    restingOrders_;           // FIFO of our live order IDs
};

} // namespace OrderMatcher
