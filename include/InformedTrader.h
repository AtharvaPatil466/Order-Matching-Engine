#pragma once

#include "SimulationParticipant.h"
#include "MatchingEngine.h"
#include "EventListener.h"
#include "OrderBook.h"
#include "Types.h"

#include <cstdint>
#include <limits>
#include <random>

namespace OrderMatcher {

// ----------------------------------------------------------------------------
// InformedTrader — trades against a private signal (fair value estimate).
//
// Behaviour per tick:
//   • If fairValue_ > bestAsk + signalThreshold  AND |position| < maxPosition:
//       – Submit an aggressive buy limit priced through the book (at fairValue_)
//   • If fairValue_ < bestBid - signalThreshold  AND |position| < maxPosition:
//       – Submit an aggressive sell limit priced through the book (at fairValue_)
//   • Otherwise: do nothing.
//
// Position tracking:
//   An inner EventListener accumulates the signed net position from own fills.
//
// Signal updates:
//   Call setSignal(Price fairValue) at any time between ticks to simulate news.
// ----------------------------------------------------------------------------
class InformedTrader : public SimulationParticipant {
public:
    struct Config {
        Price    signalThreshold = 2;         // Min edge before trading (in Price units)
        int64_t  maxPosition     = 200;       // Max absolute net position
        Quantity orderQty        = 50;        // Size of each aggressive order
        Price    fallbackMid     = 100 * PRICE_PRECISION; // Used when no quotes
    };

    // Inner EventListener tracking own fills.
    class FillListener : public EventListener {
    public:
        explicit FillListener(ParticipantId pid) : pid_(pid) {}

        void onTrade(const Trade& t) override {
            if (t.buyerId == pid_) {
                position_ += static_cast<int64_t>(t.quantity);
            } else if (t.sellerId == pid_) {
                position_ -= static_cast<int64_t>(t.quantity);
            }
        }

        int64_t position() const { return position_; }

    private:
        ParticipantId pid_;
        int64_t       position_ = 0;
    };

    // Constructor with default config
    explicit InformedTrader(ParticipantId pid, uint64_t seed = 42)
        : pid_(pid), cfg_(), rng_(seed), fillListener_(pid) {}

    // Constructor with explicit config
    InformedTrader(ParticipantId pid, Config cfg, uint64_t seed = 42)
        : pid_(pid), cfg_(cfg), rng_(seed), fillListener_(pid) {}

    ParticipantId id() const override { return pid_; }

    // Update the private valuation; safe to call between ticks.
    void setSignal(Price fairValue) { fairValue_ = fairValue; }
    Price signal() const           { return fairValue_; }

    int64_t position() const { return fillListener_.position(); }

    void onTick(uint64_t  /*tickNum*/,
                Price     bestBid,
                Price     bestAsk,
                Quantity  /*bidDepth*/,
                Quantity  /*askDepth*/) override
    {
        if (!engine_) return;

        // Register listener on first tick
        if (!listenerRegistered_) {
            OrderBook* book = engine_->getOrderBook(symbol_);
            if (book) {
                book->setEventListener(&fillListener_);
            }
            listenerRegistered_ = true;
        }

        // Normalise: getBestAsk() returns MAX_PRICE when there are no asks.
        constexpr Price NO_ASK = std::numeric_limits<Price>::max();
        if (bestAsk == NO_ASK) bestAsk = 0;

        // No signal set — stay flat
        if (fairValue_ == 0) return;

        int64_t pos = fillListener_.position();

        // Decide whether to trade
        if (bestAsk > 0 && fairValue_ > bestAsk + cfg_.signalThreshold) {
            // Underpriced: buy
            if (std::abs(pos) < cfg_.maxPosition || pos < 0) {
                submitAggressive(Side::Buy, fairValue_);
            }
        } else if (bestBid > 0 && fairValue_ < bestBid - cfg_.signalThreshold) {
            // Overpriced: sell
            if (std::abs(pos) < cfg_.maxPosition || pos > 0) {
                submitAggressive(Side::Sell, fairValue_);
            }
        }
    }

private:
    void submitAggressive(Side side, Price limitPrice) {
        if (limitPrice <= 0) limitPrice = 1;
        Quantity qty = cfg_.orderQty;
        if (qty == 0) qty = 1;

        OrderId oid = nextOrderId_++;
        engine_->submitOrder(symbol_, oid, pid_, side, limitPrice, qty,
                             OrderType::Limit,
                             /*stopPrice=*/0,
                             /*displayQty=*/0,
                             TimeInForce::GTC);
    }

    ParticipantId  pid_;
    Config         cfg_;
    std::mt19937   rng_;               // reserved for stochastic signal noise
    FillListener   fillListener_;
    bool           listenerRegistered_ = false;

    Price    fairValue_    = 0;        // 0 = no signal
    OrderId  nextOrderId_  = 3'000'000;
};

} // namespace OrderMatcher
