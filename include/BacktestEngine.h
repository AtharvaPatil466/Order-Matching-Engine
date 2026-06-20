#pragma once

// BacktestEngine.h — header-only.
// Runs a complete backtest: given a position to unwind and calibrated
// Almgren-Chriss parameters, generates an AC-optimal schedule and executes
// it against a live MatchingEngine, measuring actual vs theoretical IS.
//
// No external dependencies beyond the standard library.

#include "CalibrationPipeline.h"
#include "EventListener.h"
#include "ExecutionAnalytics.h"
#include "MatchingEngine.h"
#include "OptimalExecution.h"
#include "OrderBook.h"
#include "Types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace OrderMatcher {

// ---------------------------------------------------------------------------
// BacktestConfig — parameters for a single backtest run.
// ---------------------------------------------------------------------------
struct BacktestConfig {
    double        positionToUnwind; // signed: positive = long to sell, negative = short to buy
    int           numSteps;         // number of execution slices (e.g. 10)
    double        tau;              // step duration in seconds (e.g. 60.0)
    double        lambda;           // risk-aversion parameter passed to AC schedule
    ParticipantId participantId;
    SymbolId      symbolId;
    OrderId       firstOrderId;     // IDs are allocated sequentially from here
};

// ---------------------------------------------------------------------------
// BacktestResult — output of BacktestEngine::run().
// ---------------------------------------------------------------------------
struct BacktestResult {
    ExecutionSchedule    schedule;        // theoretical AC-optimal schedule
    std::vector<double>  actualFills;     // qty actually filled at each step
    std::vector<double>  actualPrices;    // volume-weighted avg fill price at each step
    double               theoreticalIS;  // E[cost] from the schedule
    double               actualIS;       // measured IS vs arrival price
    double               trackingError;  // actualIS - theoreticalIS
    double               fillRate;       // total filled / positionToUnwind
    bool                 complete;       // true if fully filled
};

// ---------------------------------------------------------------------------
// BacktestEngine — executes an AC schedule against a live MatchingEngine.
// ---------------------------------------------------------------------------
class BacktestEngine {
public:
    explicit BacktestEngine(MatchingEngine& engine)
        : engine_(engine) {}

    // -----------------------------------------------------------------------
    // run — execute a single backtest pass.
    //
    // calibResult: from CalibrationPipeline::calibrate() or calibrateFromJournal()
    // arrivalMid:  mid price at the moment the parent order was submitted
    //              (in double-precision price units, i.e. the result of
    //               toDouble(book->getMidPrice()) at submission time)
    // -----------------------------------------------------------------------
    BacktestResult run(const BacktestConfig&    cfg,
                       const CalibrationResult& calibResult,
                       double                   arrivalMid)
    {
        BacktestResult result{};

        if (cfg.numSteps <= 0 || cfg.tau <= 0.0) {
            return result;
        }

        // ── 1. Build the AC-optimal schedule ─────────────────────────────
        ACParams schedParams = calibResult.params;
        // Override lambda from config so the caller controls risk aversion
        // independently from the calibration sigma.
        schedParams.lambda = cfg.lambda;

        result.schedule = almgrenChrissSchedule(
            cfg.positionToUnwind,
            cfg.numSteps,
            cfg.tau,
            schedParams);

        result.theoreticalIS = result.schedule.expectedCost;

        const std::size_t N = result.schedule.trades.size();
        result.actualFills.resize(N, 0.0);
        result.actualPrices.resize(N, 0.0);

        // Direction: positive position → selling, direction = +1
        //            negative position → buying,  direction = -1
        double direction = (cfg.positionToUnwind >= 0.0) ? 1.0 : -1.0;

        // ── 2. Execute each slice ─────────────────────────────────────────
        OrderId orderId = cfg.firstOrderId;

        for (std::size_t j = 0; j < N; ++j) {
            double targetQty = result.schedule.trades[j];
            if (targetQty == 0.0) {
                result.actualFills[j]  = 0.0;
                result.actualPrices[j] = 0.0;
                continue;
            }

            // The schedule trades for a long unwind are positive (selling).
            // Determine the side: selling if targetQty > 0, buying otherwise.
            bool selling    = (targetQty > 0.0);
            Side orderSide  = selling ? Side::Sell : Side::Buy;
            Quantity qty    = static_cast<Quantity>(std::fabs(targetQty) + 0.5);
            if (qty == 0) {
                result.actualFills[j]  = 0.0;
                result.actualPrices[j] = 0.0;
                continue;
            }

            // Determine an aggressive limit price that should fill
            // immediately against the opposite side.
            //   Sell: price = bestBid - 1 tick  (crosses into bids)
            //   Buy:  price = bestAsk + 1 tick  (crosses into asks)
            Price limitPrice = 0;
            {
                OrderBook* bk = engine_.getOrderBook(cfg.symbolId);
                if (bk) {
                    if (selling) {
                        Price bestBid = bk->getBestBid();
                        limitPrice   = (bestBid > 1) ? (bestBid - 1) : 1;
                    } else {
                        Price bestAsk = bk->getBestAsk();
                        limitPrice   = (bestAsk > 0) ? (bestAsk + 1)
                                                      : toPrice(1.0);
                    }
                }
            }

            // If no limit price could be determined (empty book), use a
            // wide but finite price so the order at least enters the book.
            if (limitPrice <= 0) {
                limitPrice = selling ? 1 : toPrice(1000000.0);
            }

            // Submit the aggressive limit order.
            engine_.submitOrder(
                cfg.symbolId,
                orderId,
                cfg.participantId,
                orderSide,
                limitPrice,
                qty,
                OrderType::Limit);

            ++orderId;

            // ── Collect fills via a scoped EventListener ──────────────
            // Install a temporary listener BEFORE submit so we capture the
            // trade callback that fires synchronously inside waitForDrain().
            struct FillCapture : EventListener {
                OrderId      watchId;
                bool         selling;
                double       fillQty{0.0};
                double       wsum{0.0};
                std::vector<FillRecord>* out;
                void onTrade(const Trade& t) override {
                    bool mine = selling ? (t.sellOrderId == watchId)
                                        : (t.buyOrderId  == watchId);
                    if (!mine) return;
                    double fq = static_cast<double>(t.quantity);
                    double fp = toDouble(t.price);
                    fillQty += fq;
                    wsum    += fq * fp;
                    FillRecord fr;
                    fr.price  = fp;
                    fr.qty    = selling ? -fq : fq;
                    fr.seqNum = t.sequenceNumber;
                    out->push_back(fr);
                }
            };
            FillCapture cap;
            cap.watchId = orderId - 1;
            cap.selling = selling;
            cap.out     = &fills_;

            OrderBook* bk = engine_.getOrderBook(cfg.symbolId);
            if (bk) bk->setEventListener(&cap);
            engine_.waitForDrain();
            if (bk) bk->setEventListener(nullptr);

            result.actualFills[j]  = cap.fillQty;
            result.actualPrices[j] = (cap.fillQty > 0.0) ? (cap.wsum / cap.fillQty) : 0.0;
        }

        // ── 3. Compute actual IS ──────────────────────────────────────────
        // actualIS = (mean(actualPrices) - arrivalMid) * direction
        // where direction = +1 for a sell program (long unwind).
        // For sells, a higher fill price than arrival is GOOD (negative cost);
        // we follow the AC convention: cost = (arrivalMid - avgFillPrice) * direction
        // for a sell, so that positive cost = slippage.
        double totalFilled = 0.0;
        double wsum        = 0.0;
        for (std::size_t j = 0; j < N; ++j) {
            totalFilled += result.actualFills[j];
            wsum        += result.actualFills[j] * result.actualPrices[j];
        }

        double avgFillPrice = (totalFilled > 0.0) ? (wsum / totalFilled) : arrivalMid;

        // IS = (avgFillPrice - arrivalMid) * direction
        // For a sell: direction = +1, so IS > 0 means we sold ABOVE arrival (good).
        // We follow OptimalExecution.h's convention where expectedCost is positive
        // when there is market impact (cost to the investor), so:
        //   actualIS = (arrivalMid - avgFillPrice) * direction   [cost perspective]
        if (arrivalMid > 0.0) {
            result.actualIS = (arrivalMid - avgFillPrice) * direction;
        } else {
            result.actualIS = 0.0;
        }

        result.trackingError = result.actualIS - result.theoreticalIS;

        double targetAbs = std::fabs(cfg.positionToUnwind);
        result.fillRate  = (targetAbs > 0.0) ? (totalFilled / targetAbs) : 0.0;
        result.complete  = (totalFilled >= targetAbs - 0.5);

        return result;
    }

    // -----------------------------------------------------------------------
    // fillHistory — returns all FillRecord instances captured during run().
    // -----------------------------------------------------------------------
    const std::vector<FillRecord>& fillHistory() const {
        // FillRecord is defined in ExecutionAnalytics.h; we store our own
        // local vector of the same type.
        return fills_;
    }

    void clearHistory() {
        fills_.clear();
    }

private:
    MatchingEngine&                            engine_;
    std::vector<FillRecord> fills_;
};

} // namespace OrderMatcher
