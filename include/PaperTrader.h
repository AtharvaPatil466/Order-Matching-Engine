#pragma once
// PaperTrader.h — header-only signal-driven paper trading loop.
// Subscribes to microstructure snapshots, generates composite signals via
// SignalGenerator, and submits IOC limit orders when |signal| > threshold.
// Position and P&L are tracked analytically; no external state required.

#include "MatchingEngine.h"
#include "SignalGenerator.h"
#include "EventListener.h"
#include "Types.h"
#include <cmath>
#include <cstdint>

namespace OrderMatcher {

struct PaperTraderStats {
    int64_t  position;         // signed net shares (+ = long, - = short)
    double   realizedPnl;      // closed P&L in price units
    double   unrealizedPnl;    // mark-to-market P&L vs. average cost
    double   totalPnl;         // realized + unrealized
    size_t   tradeCount;       // number of fills received
    size_t   tickCount;        // number of ticks processed
    double   lastSignal;       // last composite signal value
};

class PaperTrader {
public:
    // engine       : live or simulated MatchingEngine
    // symbol       : the symbol to trade
    // pid          : participant ID used for paper orders (must not clash with real traders)
    // buyThreshold : minimum positive signal to go long  (default 0.20)
    // sellThreshold: minimum negative signal to go short (default 0.20)
    // orderSize    : IOC quantity per signal event
    // maxPosition  : absolute position cap (no new orders if |position| >= maxPosition)
    explicit PaperTrader(MatchingEngine& engine,
                         SymbolId symbol,
                         ParticipantId pid       = 9999,
                         double buyThreshold     = 0.20,
                         double sellThreshold    = 0.20,
                         Quantity orderSize      = 100,
                         Quantity maxPosition    = 1000)
        : engine_(engine), symbol_(symbol), pid_(pid),
          buyThreshold_(buyThreshold), sellThreshold_(sellThreshold),
          orderSize_(orderSize), maxPosition_(maxPosition)
    {}

    // Process one microstructure tick.
    // Records the snapshot in the signal history, computes a composite signal,
    // and optionally submits a buy or sell IOC order.
    // Returns the composite signal value used for this tick.
    double onTick(const MicrostructureSnapshot& snap) {
        signals_.recordSnapshot(snap);
        Signal sig = SignalGenerator::compositeSignal(snap);
        lastSignal_ = sig.value;
        ++tickCount_;

        bool shouldBuy  = sig.value >  buyThreshold_  && sig.confidence > 0.1
                          && position_ < static_cast<int64_t>(maxPosition_);
        bool shouldSell = sig.value < -sellThreshold_ && sig.confidence > 0.1
                          && position_ > -static_cast<int64_t>(maxPosition_);

        if (shouldBuy || shouldSell) {
            Side side = shouldBuy ? Side::Buy : Side::Sell;
            submitAndCapture(side, snap.midPrice);
        }

        return sig.value;
    }

    int64_t  position()     const { return position_; }
    double   realizedPnl()  const { return realizedPnl_; }
    double   unrealizedPnl(double currentMid) const {
        if (position_ == 0) return 0.0;
        return static_cast<double>(position_) * (currentMid - avgCostBasis_);
    }
    double   totalPnl(double currentMid) const {
        return realizedPnl_ + unrealizedPnl(currentMid);
    }

    PaperTraderStats stats(double currentMid) const {
        return {
            position_, realizedPnl_, unrealizedPnl(currentMid),
            totalPnl(currentMid), tradeCount_, tickCount_, lastSignal_
        };
    }

    void reset() {
        position_     = 0;
        realizedPnl_  = 0.0;
        avgCostBasis_ = 0.0;
        tradeCount_   = 0;
        tickCount_    = 0;
        lastSignal_   = 0.0;
        signals_.reset();
        nextOrderId_  = 10000;
    }

private:
    void submitAndCapture(Side side, double midHint) {
        // Build an aggressive limit price that should fill as IOC.
        OrderBook* bk = engine_.getOrderBook(symbol_);
        Price limitPx = 0;
        if (bk) {
            if (side == Side::Buy) {
                Price ask = bk->getBestAsk();
                limitPx = (ask > 0 && ask < std::numeric_limits<Price>::max())
                              ? (ask + 1) : toPrice(midHint * 1.01);
            } else {
                Price bid = bk->getBestBid();
                limitPx = (bid > 1) ? (bid - 1) : toPrice(midHint * 0.99);
            }
        }
        if (limitPx <= 0) limitPx = toPrice(midHint);

        OrderId oid = nextOrderId_++;

        // Capture fills via scoped EventListener
        struct FillCapture : EventListener {
            OrderId watchId;
            Side    side;
            double  fillQty{0.0};
            double  wsum{0.0};
            void onTrade(const Trade& t) override {
                bool mine = (side == Side::Buy)
                    ? (t.buyOrderId  == watchId)
                    : (t.sellOrderId == watchId);
                if (!mine) return;
                double fq = static_cast<double>(t.quantity);
                fillQty += fq;
                wsum    += fq * toDouble(t.price);
            }
        };
        FillCapture cap;
        cap.watchId = oid;
        cap.side    = side;

        if (bk) bk->setEventListener(&cap);
        engine_.submitOrder(symbol_, oid, pid_, side, limitPx, orderSize_,
                            OrderType::IOC);
        engine_.waitForDrain();
        if (bk) bk->setEventListener(nullptr);

        if (cap.fillQty > 0.0) {
            double avgFill = cap.wsum / cap.fillQty;
            applyFill(side, cap.fillQty, avgFill);
        }
    }

    void applyFill(Side side, double qty, double price) {
        ++tradeCount_;
        int64_t signedQty = (side == Side::Buy)
            ? static_cast<int64_t>(qty) : -static_cast<int64_t>(qty);

        if (position_ == 0) {
            // Opening a new position
            avgCostBasis_ = price;
            position_ += signedQty;
        } else if ((position_ > 0) == (signedQty > 0)) {
            // Adding to existing position
            double totalQty = std::fabs(static_cast<double>(position_)) + qty;
            avgCostBasis_ = (std::fabs(static_cast<double>(position_)) * avgCostBasis_
                             + qty * price) / totalQty;
            position_ += signedQty;
        } else {
            // Reducing or flipping position
            double closeQty = std::min(qty, std::fabs(static_cast<double>(position_)));
            double closeSign = (position_ > 0) ? 1.0 : -1.0;
            realizedPnl_ += closeQty * closeSign * (price - avgCostBasis_);
            double remaining = qty - closeQty;
            position_ += signedQty;
            if (position_ == 0) {
                avgCostBasis_ = 0.0;
            } else if (remaining > 0.0) {
                // Flipped: new position at new price
                avgCostBasis_ = price;
            }
        }
    }

    MatchingEngine& engine_;
    SymbolId        symbol_;
    ParticipantId   pid_;
    double          buyThreshold_;
    double          sellThreshold_;
    Quantity        orderSize_;
    Quantity        maxPosition_;

    int64_t         position_{0};
    double          realizedPnl_{0.0};
    double          avgCostBasis_{0.0};
    size_t          tradeCount_{0};
    size_t          tickCount_{0};
    double          lastSignal_{0.0};
    OrderId         nextOrderId_{10000};
    SignalGenerator signals_;
};

} // namespace OrderMatcher
