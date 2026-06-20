#pragma once

// ResearchDashboard.h — header-only orchestration layer.
// Aggregates all microstructure research components into one MicrostructureReport.

#include "CalibrationPipeline.h"
#include "ImpactModel.h"
#include "Journal.h"
#include "MatchingEngine.h"
#include "MicrostructureMetrics.h"
#include "OrderBook.h"
#include "OrderFlowAnalytics.h"
#include "ResearchHarness.h"
#include "SignalGenerator.h"
#include "SpreadDecomposition.h"
#include "Types.h"
#include "VPINCalculator.h"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace OrderMatcher {

// Alias used in buildReport signature.
using SpreadDecomposition_GH = GlostenhHarrisResult;

// ── MicrostructureReport ─────────────────────────────────────────────────────

struct MicrostructureReport {
    // Source metadata
    uint64_t tradesAnalyzed{0};
    double   adv{0.0};

    // Spread metrics
    double effectiveSpread{0.0};
    double realizedSpread{0.0};
    double rollSpread{0.0};
    double adverseSelectionFraction{0.0};

    // Flow metrics
    double orderFlowImbalance{0.0};
    double kylesLambda{0.0};
    double amihudRatio{0.0};
    double vpin{0.0};
    double orderToTradeRatio{0.0};

    // Impact model
    double acEta{0.0};
    double acGamma{0.0};
    double impactModelR2{0.0};

    // Composite signal
    double compositeSignal{0.0};
    double signalConfidence{0.0};

    // ── Serialization ────────────────────────────────────────────────────────

    std::string toJson() const {
        std::ostringstream o;
        o << std::fixed << std::setprecision(6);
        o << "{";
        o << "\"trades_analyzed\":"   << tradesAnalyzed           << ",";
        o << "\"adv\":"               << safe(adv)                 << ",";
        o << "\"effective_spread\":"  << safe(effectiveSpread)     << ",";
        o << "\"realized_spread\":"   << safe(realizedSpread)      << ",";
        o << "\"roll_spread\":"       << safe(rollSpread)          << ",";
        o << "\"adverse_selection_fraction\":"
                                      << safe(adverseSelectionFraction) << ",";
        o << "\"order_flow_imbalance\":"
                                      << safe(orderFlowImbalance)  << ",";
        o << "\"kyles_lambda\":"      << safe(kylesLambda)         << ",";
        o << "\"amihud_ratio\":"      << safe(amihudRatio)         << ",";
        o << "\"vpin\":"              << safe(vpin)                 << ",";
        o << "\"order_to_trade_ratio\":"
                                      << safe(orderToTradeRatio)   << ",";
        o << "\"ac_eta\":"            << safe(acEta)               << ",";
        o << "\"ac_gamma\":"          << safe(acGamma)             << ",";
        o << "\"impact_model_r2\":"   << safe(impactModelR2)       << ",";
        o << "\"composite_signal\":"  << safe(compositeSignal)     << ",";
        o << "\"signal_confidence\":" << safe(signalConfidence);
        o << "}";
        return o.str();
    }

    std::string toCsv(bool header = true) const {
        static const char* kHeader =
            "trades_analyzed,adv,effective_spread,realized_spread,"
            "roll_spread,adverse_selection_fraction,order_flow_imbalance,"
            "kyles_lambda,amihud_ratio,vpin,order_to_trade_ratio,"
            "ac_eta,ac_gamma,impact_model_r2,composite_signal,signal_confidence";

        std::ostringstream o;
        o << std::fixed << std::setprecision(6);

        if (header) {
            o << kHeader << "\n";
        }

        o << tradesAnalyzed        << ","
          << safe(adv)             << ","
          << safe(effectiveSpread) << ","
          << safe(realizedSpread)  << ","
          << safe(rollSpread)      << ","
          << safe(adverseSelectionFraction) << ","
          << safe(orderFlowImbalance) << ","
          << safe(kylesLambda)     << ","
          << safe(amihudRatio)     << ","
          << safe(vpin)            << ","
          << safe(orderToTradeRatio) << ","
          << safe(acEta)           << ","
          << safe(acGamma)         << ","
          << safe(impactModelR2)   << ","
          << safe(compositeSignal) << ","
          << safe(signalConfidence);

        return o.str();
    }

private:
    // Clamp NaN/Inf to 0.0 before serializing.
    static double safe(double v) {
        return (std::isfinite(v)) ? v : 0.0;
    }
};

// ── ResearchDashboard ─────────────────────────────────────────────────────────

class ResearchDashboard {
public:
    // Build a report from pre-computed components.
    static MicrostructureReport buildReport(
        const MicrostructureMetrics&  metrics,
        const OrderFlowAnalytics&     flowAnalytics,
        const VPINCalculator&         vpinCalc,
        const SpreadDecomposition_GH& spreadDecomp,
        const CalibrationResult&      calibration,
        const MicrostructureSnapshot& snapshot,
        double                        adv,
        uint64_t                      tradesAnalyzed)
    {
        MicrostructureReport r;

        r.tradesAnalyzed = tradesAnalyzed;
        r.adv            = adv;

        r.effectiveSpread          = metrics.effectiveSpread();
        r.realizedSpread           = metrics.realizedSpread();
        r.orderFlowImbalance       = metrics.orderFlowImbalance();
        r.kylesLambda              = metrics.kylesLambda();
        r.amihudRatio              = metrics.amihudRatio();

        r.rollSpread               = flowAnalytics.rollSpread();
        r.orderToTradeRatio        = flowAnalytics.orderToTradeRatio();

        r.vpin                     = vpinCalc.vpin();

        r.adverseSelectionFraction = spreadDecomp.adverseSelectionFraction;

        r.acEta                    = calibration.params.eta;
        r.acGamma                  = calibration.params.gamma;
        r.impactModelR2            = 0.0;   // r2 not preserved in ACParams

        Signal sig = SignalGenerator::compositeSignal(snapshot);
        r.compositeSignal  = sig.value;
        r.signalConfidence = sig.confidence;

        return r;
    }

    // Convenience overload: runs the full pipeline on a journal.
    static MicrostructureReport fromJournal(
        const std::string& journalPath,
        SymbolId           symbol,
        double             adv = 0.0)
    {
        MatchingEngine engine;
        engine.addSymbol(symbol);
        // Use startAsync as required; stopAsync() is guaranteed via RAII below.
        engine.startAsync(1, 1024);

        // RAII guard: always call stopAsync() on exit, even under exception.
        struct StopGuard {
            MatchingEngine& eng;
            bool fired{false};
            void stop() { if (!fired) { eng.stopAsync(); fired = true; } }
            ~StopGuard() { stop(); }
        } guard{engine};

        // Drain and switch to synchronous mode for deterministic replay so
        // that harness callbacks are delivered inline (no cross-thread races).
        guard.stop();
        engine.start();

        MicrostructureMetrics  metrics;
        OrderFlowAnalytics     flowAnalytics;
        VPINCalculator         vpinCalc;

        std::vector<TradeRecord> tradeRecords;
        std::vector<double>      mids;
        double                   lastMid  = 0.0;
        double                   prevPrice = 0.0;
        uint64_t                 tradeCount = 0;

        ResearchHarness harness(engine);

        harness.onBookUpdate([&](SymbolId sym, Price mid,
                                  Quantity /*bid*/, Quantity /*ask*/)
        {
            if (sym == symbol) {
                lastMid = toDouble(mid);
                mids.push_back(lastMid);
            }
        });

        harness.onTrade([&](const Trade& t) {
            if (t.symbolId != symbol) return;

            double price = toDouble(t.price);
            double qty   = static_cast<double>(t.quantity);
            double mid   = lastMid;

            // MicrostructureMetrics
            MicrostructureMetrics::TradeEvent ev{};
            ev.price       = t.price;
            ev.side        = t.aggressorSide;
            ev.qty         = t.quantity;
            ev.midpoint    = static_cast<Price>(mid * PRICE_PRECISION);
            ev.timestampNs = 0;
            metrics.record(ev);

            // OrderFlowAnalytics
            flowAnalytics.recordTradePrice(price);
            flowAnalytics.recordTradeEvent();
            flowAnalytics.recordOrderEvent();  // each trade implies at least one order

            // VPINCalculator
            vpinCalc.recordTrade(price, qty, (prevPrice > 0.0) ? prevPrice : price);

            // SpreadDecomposition inputs
            double signedQty = (t.aggressorSide == Side::Buy) ? qty : -qty;
            tradeRecords.push_back({price, signedQty});

            prevPrice = price;
            ++tradeCount;
        });

        harness.replay(journalPath);

        // Align mids to trades.size() + 1 for buildSpreadObservations.
        while (mids.size() <= tradeRecords.size()) mids.push_back(lastMid);
        if (mids.size() > tradeRecords.size() + 1) mids.resize(tradeRecords.size() + 1);
        if (lastMid == 0.0) for (double m : mids) if (m != 0.0) { lastMid = m; break; }
        for (double& m : mids) if (m == 0.0) m = lastMid;

        SpreadDecomposition_GH spreadDecomp{};
        if (!tradeRecords.empty()) {
            auto obs = buildSpreadObservations(tradeRecords, mids);
            spreadDecomp = fitGlostenhHarris(obs);
        }

        double effectiveAdv = (adv > 0.0) ? adv : static_cast<double>(tradeCount);
        CalibrationResult calibration =
            calibrateFromJournal(engine, symbol, journalPath, effectiveAdv);

        MicrostructureSnapshot snapshot{};
        snapshot.vpin               = vpinCalc.vpin();
        snapshot.orderFlowImbalance = metrics.orderFlowImbalance();
        snapshot.rollSpread         = flowAnalytics.rollSpread();
        snapshot.kylesLambda        = metrics.kylesLambda();
        snapshot.arrivalRate        = flowAnalytics.arrivalRatePerUs();
        snapshot.midPrice           = lastMid;
        snapshot.prevMidPrice       = lastMid;

        return buildReport(metrics, flowAnalytics, vpinCalc,
                           spreadDecomp, calibration, snapshot,
                           effectiveAdv, tradeCount);
    }
};

} // namespace OrderMatcher
