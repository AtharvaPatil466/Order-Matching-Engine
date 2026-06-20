#pragma once

#include "SimulationParticipant.h"
#include "MatchingEngine.h"
#include "EventListener.h"
#include "OrderBook.h"
#include "Types.h"

#include <cmath>
#include <limits>
#include <random>
#include <cstdint>

namespace OrderMatcher {

// ----------------------------------------------------------------------------
// MarketMaker — runs an Avellaneda-Stoikov inventory-adjusted quoting model.
//
// Each tick:
//   1. Cancel previous resting bid and ask (if any).
//   2. Compute reservation price:
//        r = mid − γ · inventory · σ² · T
//   3. Compute half-spread:
//        half = (γ · σ² · T / 2) + ln(1 + γ/κ) / 2
//      (κ = order-arrival intensity, default 1.5)
//   4. Post bid at r − half  and ask at r + half.
//   5. If |inventory| > maxInventory, widen spread by inventoryPenalty ticks
//      on the side that would increase inventory.
//
// Fill tracking via EventListener:
//   MarketMaker::Listener is registered on the book and accumulates the net
//   signed inventory from own trades.
// ----------------------------------------------------------------------------
class MarketMaker : public SimulationParticipant {
public:
    struct Config {
        double   gamma              = 0.1;   // Risk-aversion parameter γ
        double   sigma              = 0.02;  // Volatility estimate σ (fraction)
        double   T                  = 1.0;   // Time horizon T (ticks)
        double   kappa              = 1.5;   // Order-arrival intensity κ
        int64_t  maxInventory       = 500;   // Absolute position limit
        int64_t  inventoryPenalty   = 5;     // Extra ticks widening per side when |inv| > max
        Quantity quoteQty           = 50;    // Quote size (each side)
        Price    fallbackMid        = 100 * PRICE_PRECISION; // Used when no mid exists
        Price    minHalfSpread      = 1;     // Floor on half-spread in ticks
    };

    // Inner EventListener that records fills for this market maker.
    // Registered on the book via setEventListener().  Only trades whose
    // buyerId or sellerId matches pid_ are counted.
    class FillListener : public EventListener {
    public:
        explicit FillListener(ParticipantId pid) : pid_(pid) {}

        void onTrade(const Trade& t) override {
            if (t.buyerId == pid_) {
                inventory_ += static_cast<int64_t>(t.quantity);
            } else if (t.sellerId == pid_) {
                inventory_ -= static_cast<int64_t>(t.quantity);
            }
        }

        int64_t inventory() const { return inventory_; }
        void    resetInventory()  { inventory_ = 0; }

    private:
        ParticipantId pid_;
        int64_t       inventory_ = 0;
    };

    // Constructor with default config
    explicit MarketMaker(ParticipantId pid, uint64_t seed = 42)
        : pid_(pid), cfg_(), rng_(seed), fillListener_(pid) {}

    // Constructor with explicit config
    MarketMaker(ParticipantId pid, Config cfg, uint64_t seed = 42)
        : pid_(pid), cfg_(cfg), rng_(seed), fillListener_(pid) {}

    ParticipantId id() const override { return pid_; }

    // Call once after the engine and symbol have been injected, before run().
    // SimulationDriver calls this automatically via the friend relationship in
    // SimulationParticipant — but we override the injection point here via
    // onFirstTick() detection inside onTick().
    void registerListener() {
        if (!engine_) return;
        OrderBook* book = engine_->getOrderBook(symbol_);
        if (book) {
            book->setEventListener(&fillListener_);
        }
    }

    void onTick(uint64_t  /*tickNum*/,
                Price     bestBid,
                Price     bestAsk,
                Quantity  /*bidDepth*/,
                Quantity  /*askDepth*/) override
    {
        if (!engine_) return;

        // Register listener on first tick (engine_ is set by driver before tick 0)
        if (!listenerRegistered_) {
            registerListener();
            listenerRegistered_ = true;
        }

        // ── Cancel previous quotes ───────────────────────────────────────
        if (activeBidId_ != 0) {
            engine_->submitCancel(symbol_, activeBidId_);
            activeBidId_ = 0;
        }
        if (activeAskId_ != 0) {
            engine_->submitCancel(symbol_, activeAskId_);
            activeAskId_ = 0;
        }

        // Normalise: getBestAsk() returns MAX_PRICE when there are no asks.
        constexpr Price NO_ASK = std::numeric_limits<Price>::max();
        if (bestAsk == NO_ASK) bestAsk = 0;

        // ── Compute mid price ────────────────────────────────────────────
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

        // ── Avellaneda-Stoikov reservation price ─────────────────────────
        int64_t inventory = fillListener_.inventory();
        // r = mid − γ · q · σ² · T    (all arithmetic in floating point, then
        // convert back to integer ticks)
        double  r         = static_cast<double>(mid)
                            - cfg_.gamma * static_cast<double>(inventory)
                              * cfg_.sigma * cfg_.sigma * cfg_.T;

        // Half-spread: (γ · σ² · T / 2) + ln(1 + γ/κ) / 2
        double halfSpreadF = (cfg_.gamma * cfg_.sigma * cfg_.sigma * cfg_.T / 2.0)
                             + std::log(1.0 + cfg_.gamma / cfg_.kappa) / 2.0;

        // Convert half-spread to integer ticks (round up, enforce floor)
        Price halfSpread = static_cast<Price>(halfSpreadF * PRICE_PRECISION);
        if (halfSpread < cfg_.minHalfSpread) halfSpread = cfg_.minHalfSpread;

        // ── Inventory penalty: widen on the over-exposed side ────────────
        Price bidPenalty = 0, askPenalty = 0;
        if (std::abs(inventory) > cfg_.maxInventory) {
            if (inventory > 0) {
                // Long: widen ask (make selling cheaper), widen bid (discourage buying more)
                bidPenalty = cfg_.inventoryPenalty;
            } else {
                // Short: widen bid, discourage further selling
                askPenalty = cfg_.inventoryPenalty;
            }
        }

        Price bidPrice = static_cast<Price>(r) - halfSpread - bidPenalty;
        Price askPrice = static_cast<Price>(r) + halfSpread + askPenalty;

        // Sanity guards: prices must be positive and crossed quotes are invalid
        if (bidPrice <= 0) bidPrice = 1;
        if (askPrice <= bidPrice + 1) askPrice = bidPrice + 1;

        Quantity qty = cfg_.quoteQty;
        if (qty == 0) qty = 1;

        // ── Post fresh quotes ────────────────────────────────────────────
        OrderId bidId = nextOrderId_++;
        auto bidResult = engine_->submitOrder(symbol_, bidId, pid_, Side::Buy,
                                              bidPrice, qty,
                                              OrderType::Limit,
                                              /*stopPrice=*/0,
                                              /*displayQty=*/0,
                                              TimeInForce::GTC);
        if (bidResult.isAccepted()) {
            activeBidId_ = bidId;
        }

        OrderId askId = nextOrderId_++;
        auto askResult = engine_->submitOrder(symbol_, askId, pid_, Side::Sell,
                                              askPrice, qty,
                                              OrderType::Limit,
                                              /*stopPrice=*/0,
                                              /*displayQty=*/0,
                                              TimeInForce::GTC);
        if (askResult.isAccepted()) {
            activeAskId_ = askId;
        }
    }

    // Read-only access to fill listener for testing / driver snapshotting.
    const FillListener& fillListener() const { return fillListener_; }
    int64_t             inventory()    const { return fillListener_.inventory(); }

private:
    ParticipantId  pid_;
    Config         cfg_;
    std::mt19937   rng_;               // reserved for future stochastic extensions
    FillListener   fillListener_;
    bool           listenerRegistered_ = false;

    OrderId nextOrderId_ = 2'000'000;  // Namespace away from other participants
    OrderId activeBidId_ = 0;
    OrderId activeAskId_ = 0;
};

} // namespace OrderMatcher
