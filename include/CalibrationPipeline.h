#pragma once

// CalibrationPipeline.h — header-only.
// Fits Almgren-Chriss impact parameters from a replay journal and produces
// a ready-to-use ACParams struct for optimal execution scheduling.
//
// Key type bridging:
//   ImpactModel.h  exports AlmgrenChrisParams { eta, gamma, r2Temp, r2Perm }
//   OptimalExecution.h exports ACParams        { eta, gamma, sigma, lambda  }
// These are distinct structs; toACParams() converts between them, filling
// sigma and lambda with caller-supplied defaults.
//
// No external dependencies beyond the standard library.

#include "ImpactModel.h"
#include "OptimalExecution.h"
#include "ResearchHarness.h"
#include "Journal.h"
#include "MatchingEngine.h"
#include "Types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace OrderMatcher {

// ---------------------------------------------------------------------------
// CalibrationResult — output of a calibration run.
// ---------------------------------------------------------------------------
struct CalibrationResult {
    ACParams    params;      // fitted AC params (ready for almgrenChrissSchedule)
    double      adv;         // average daily volume used / derived from replay
    bool        valid;       // false if too few observations to fit
    uint64_t    tradesUsed;  // number of TradeObservation structs passed to the fitter
};

// ---------------------------------------------------------------------------
// toACParams — bridge AlmgrenChrisParams -> ACParams.
//
// sigma and lambda must be supplied by the caller; they are not observable
// from a single replay session alone.  Sensible defaults:
//   sigma  = 0.01  (1% per-step price volatility)
//   lambda = 1e-5  (moderate risk aversion)
// ---------------------------------------------------------------------------
inline ACParams toACParams(const AlmgrenChrisParams& ac,
                            double sigma  = 0.01,
                            double lambda = 1e-5)
{
    ACParams p;
    p.eta    = (std::isfinite(ac.eta)   && ac.eta   >= 0.0) ? ac.eta   : 0.0;
    p.gamma  = (std::isfinite(ac.gamma) && ac.gamma >= 0.0) ? ac.gamma : 0.0;
    p.sigma  = (std::isfinite(sigma)    && sigma    >= 0.0) ? sigma    : 0.01;
    p.lambda = (std::isfinite(lambda)   && lambda   >= 0.0) ? lambda   : 1e-5;
    return p;
}

// ---------------------------------------------------------------------------
// calibrate — fit ACParams from a pre-built vector of TradeObservation.
//
// Requires at least 5 observations; sets valid=false otherwise.
// adv is taken from the first observation (all should share the same value
// when built by the caller; if they differ the first one is used as a proxy).
// ---------------------------------------------------------------------------
inline CalibrationResult calibrate(const std::vector<TradeObservation>& obs,
                                    double sigma  = 0.01,
                                    double lambda = 1e-5)
{
    CalibrationResult result{};
    result.tradesUsed = static_cast<uint64_t>(obs.size());
    result.valid      = false;

    if (obs.size() < 5) {
        // Not enough data — return a zeroed-out but well-defined result.
        result.params = toACParams(AlmgrenChrisParams{0.0, 0.0, 0.0, 0.0}, sigma, lambda);
        result.adv    = obs.empty() ? 0.0 : obs[0].adv;
        return result;
    }

    // Use the adv from the first observation as the representative value.
    result.adv = obs[0].adv;

    // Fit Almgren-Chriss parameters.
    AlmgrenChrisParams acFit = fitAlmgrenChriss(obs);

    // Guard against non-finite values that can arise from degenerate data.
    if (!std::isfinite(acFit.eta))   acFit.eta   = 0.0;
    if (!std::isfinite(acFit.gamma)) acFit.gamma = 0.0;

    result.params = toACParams(acFit, sigma, lambda);
    result.valid  = true;
    return result;
}

// ---------------------------------------------------------------------------
// calibrateFromJournal — convenience wrapper.
//
// Replays a journal through `engine`, capturing every trade via the harness
// onTrade callback and every book update (midpoint) via onBookUpdate.
// Pairs each trade with the midpoint immediately before it, and builds a
// "post-trade mid" by looking 5 trades ahead (or using the final mid when
// fewer are available). Then calls buildObservations() and calibrate().
//
// If adv == 0.0, uses sum(|trade.qty|) from the replay as a proxy ADV.
//
// The engine must already have `symbol` registered and must be running
// (engine.start() or engine.startAsync()) before calling this function.
// ---------------------------------------------------------------------------
inline CalibrationResult calibrateFromJournal(MatchingEngine& engine,
                                               SymbolId        symbol,
                                               const std::string& journalPath,
                                               double          adv      = 0.0,
                                               double          sigma    = 0.01,
                                               double          lambda   = 1e-5)
{
    // ── Capture trades and midpoints via the harness ──────────────────────
    std::vector<Trade>  trades;
    std::vector<double> midsBetweenTrades;  // one mid entry per event (book update)
    double              lastMid = 0.0;

    ResearchHarness harness(engine);

    // Track the last-seen mid; this is updated on every book-update event.
    harness.onBookUpdate([&](SymbolId sym, Price mid,
                              Quantity /*bid*/, Quantity /*ask*/)
    {
        if (sym == symbol) {
            lastMid = toDouble(mid);
        }
    });

    // On each trade: record the pre-trade mid (lastMid at the moment the
    // callback fires, before the book-update that follows the trade).
    // The harness fires onTrade BEFORE the post-trade book-update, so
    // lastMid here is still the mid from the most recent update PRIOR to
    // this trade.
    harness.onTrade([&](const Trade& t) {
        if (t.symbolId == symbol) {
            trades.push_back(t);
            midsBetweenTrades.push_back(lastMid);
        }
    });

    harness.replay(journalPath);

    // After replay ends, record the final mid once more (post-last-trade).
    {
        OrderBook* bk = engine.getOrderBook(symbol);
        if (bk) {
            Price finalMid = bk->getMidPrice();
            if (finalMid != 0) {
                lastMid = toDouble(finalMid);
            }
        }
    }

    if (trades.empty()) {
        CalibrationResult empty{};
        empty.valid      = false;
        empty.adv        = adv;
        empty.tradesUsed = 0;
        empty.params     = toACParams(AlmgrenChrisParams{0.0, 0.0, 0.0, 0.0}, sigma, lambda);
        return empty;
    }

    // ── Build the midpoints array required by buildObservations() ────────
    // buildObservations() requires midpoints.size() == trades.size() + 1.
    // midsBetweenTrades[i] is the pre-trade mid for trade i.
    // We still need a post-last-trade mid (index trades.size()).
    // We approximate this by looking ahead 5 trades from each trade; for
    // the final element we simply use lastMid.

    const std::size_t N = trades.size();

    // Build a midpoints vector of length N+1:
    //   midpoints[i]   = pre-trade mid for trade i  (for i < N)
    //   midpoints[N]   = post-last-trade mid
    std::vector<double> midpoints(N + 1);
    for (std::size_t i = 0; i < N; ++i) {
        midpoints[i] = midsBetweenTrades[i];
    }
    midpoints[N] = lastMid;

    // If any pre-trade mid is zero (no book updates arrived before that
    // trade), fall back to the post-last-trade mid to avoid dividing by
    // zero inside buildObservations.
    for (std::size_t i = 0; i < N; ++i) {
        if (midpoints[i] == 0.0) {
            midpoints[i] = midpoints[N];
        }
    }

    // ── Determine ADV ─────────────────────────────────────────────────────
    double effectiveAdv = adv;
    if (effectiveAdv <= 0.0) {
        double totalVol = 0.0;
        for (const auto& t : trades) {
            totalVol += static_cast<double>(t.quantity);
        }
        effectiveAdv = (totalVol > 0.0) ? totalVol : 1.0;
    }

    // ── Build observations ────────────────────────────────────────────────
    std::vector<TradeObservation> obs = buildObservations(trades, midpoints, effectiveAdv);

    // ── Add temp/perm decomposition using 5-trade post-trade lookahead ────
    // For each trade i, the post-trade mid is midpoints[min(i+5, N)].
    // temp = (postTradeMid - midpoints[i+1]) / midpoints[i]   (recovery)
    // perm = (postTradeMid - midpoints[i])   / midpoints[i]   (lasting component)
    for (std::size_t i = 0; i < obs.size(); ++i) {
        std::size_t postIdx     = std::min(i + 5, N);
        double      preMid      = midpoints[i];
        double      immPostMid  = midpoints[i + 1];
        double      farPostMid  = midpoints[postIdx];

        if (preMid > 0.0) {
            // Transient component: mid has partially recovered by postIdx.
            // temp = immediate impact - far impact
            double immediateImpact = (immPostMid - preMid) / preMid;
            double permanentImpact = (farPostMid - preMid) / preMid;
            obs[i].tempImpact = immediateImpact - permanentImpact;
            obs[i].permImpact = permanentImpact;
        }
    }

    // ── Calibrate ─────────────────────────────────────────────────────────
    CalibrationResult result = calibrate(obs, sigma, lambda);
    result.adv        = effectiveAdv;
    result.tradesUsed = static_cast<uint64_t>(obs.size());
    return result;
}

} // namespace OrderMatcher
