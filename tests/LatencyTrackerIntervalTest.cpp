// LatencyTrackerIntervalTest — recordInterval must COUNT a sub-tick operation.
//
// WHAT THIS PINS, AND WHY IT IS WORTH A TEST OF ITS OWN.
//
// recordInterval used to be:
//
//     if (endNs > startNs) record(endNs - startNs);
//
// which looks like an obviously-safe guard and is not. An operation that
// completes inside one clock tick has end == start, so it left NO SAMPLE AT
// ALL. That biases every percentile UPWARD, because the samples thrown away
// are exactly the fastest ones — the distribution's whole left tail silently
// disappears and the survivors are renumbered.
//
// This is not a rounding detail. Measured on this project's own cancel path on
// an Apple Silicon box (41 ns tick), 22-32% of samples were being dropped, so
// the published "P50" was really closer to a P66 of the true distribution, and
// the reported mean was 174 ns where counting the dropped samples gives 135 ns.
// Two independent investigations found it from different directions.
//
// The fix records 0 for a sub-tick span. The VALUE is not the point and is
// deliberately not what this test is about: the true duration lies somewhere in
// [0, tick) and the clock cannot say where. Sub-tick samples hold the lowest
// ranks whatever value they are given, so every percentile above the band is
// correctly ranked either way. THE COUNT was what was broken, so the count is
// what is pinned here.
//
// end < start stays unrecorded on purpose. nowNs() is monotonic, so time
// running backwards is a broken measurement rather than a fast one — it is
// counted separately so it cannot hide in the sub-tick figure.

#include "LatencyTracker.h"

#include <cassert>
#include <cstdio>

using namespace OrderMatcher;

int main() {
    // ── A sub-tick span is recorded, not dropped ────────────────────────────
    {
        LatencyTracker t;
        t.recordInterval(1000, 1000);   // end == start: completed within a tick
        assert(t.getCount() == 1 &&
               "sub-tick interval was dropped — percentiles are biased upward");
        assert(t.getSubTickCount() == 1 && "sub-tick sample was not counted as such");
        assert(t.getClockAnomalies() == 0 && "equal timestamps are not an anomaly");
    }

    // ── A normal span is recorded with its real value ───────────────────────
    {
        LatencyTracker t;
        t.recordInterval(1000, 1500);
        assert(t.getCount() == 1);
        assert(t.getSubTickCount() == 0 && "a resolvable span is not sub-tick");
        assert(t.getMax() == 500 && "recorded value must be the real duration");
    }

    // ── A backwards clock is NOT recorded, and is reported separately ───────
    {
        LatencyTracker t;
        t.recordInterval(1500, 1000);   // end < start
        assert(t.getCount() == 0 &&
               "a backwards interval is a broken measurement, not a fast one");
        assert(t.getClockAnomalies() == 1 && "backwards interval was not flagged");
        assert(t.getSubTickCount() == 0 &&
               "a backwards interval must not be filed as sub-tick");
    }

    // ── The counts survive a mix, and the fast samples are not lost ─────────
    //
    // The regression shape: with the old guard this tracker reported 2 samples
    // and a P50 of 500 ns. Counting the sub-tick ops makes it 5 samples whose
    // median genuinely sits in the sub-tick band — which is the honest answer
    // for a path where most operations outrun the clock.
    {
        LatencyTracker t;
        t.recordInterval(0, 0);
        t.recordInterval(0, 0);
        t.recordInterval(0, 0);
        t.recordInterval(0, 500);
        t.recordInterval(0, 900);
        assert(t.getCount() == 5 && "sub-tick samples must be counted");
        assert(t.getSubTickCount() == 3);
        assert(t.getMax() == 900);
        assert(t.getP50() == 0 &&
               "with 3 of 5 samples sub-tick the median IS sub-tick; reporting a "
               "resolvable number here would mean the fast samples were dropped");
    }

    // ── mergeFrom and reset carry the new counters ──────────────────────────
    //
    // Merging is how the engine builds its aggregate view across worker
    // threads. A merge that forgot these would re-introduce the bias at exactly
    // the point the numbers get published.
    {
        LatencyTracker a, b;
        a.recordInterval(0, 0);
        a.recordInterval(1500, 1000);
        b.recordInterval(0, 0);
        b.recordInterval(0, 700);
        a.mergeFrom(b);
        assert(a.getSubTickCount() == 2 && "mergeFrom dropped sub-tick counts");
        assert(a.getClockAnomalies() == 1 && "mergeFrom dropped anomaly counts");
        assert(a.getCount() == 3);

        a.reset();
        assert(a.getSubTickCount() == 0 && a.getClockAnomalies() == 0 &&
               a.getCount() == 0 && "reset left stale counters behind");
    }

    // ── deltaFrom gives the WINDOW, not the run so far ──────────────────────
    //
    // The engine's per-thread trackers are never reset, so a per-window sample
    // of the aggregate is cumulative-since-start. SustainedLoadTest reported
    // exactly that and the artifact looked like warmup: percentiles decaying
    // monotonically as the population grew.
    {
        LatencyTracker t;
        t.recordInterval(0, 100);
        t.recordInterval(0, 100);
        LatencyTracker snapshot = t;      // window boundary

        t.recordInterval(0, 900);         // only this belongs to the window
        LatencyTracker window = t.deltaFrom(snapshot);

        assert(window.getCount() == 1 &&
               "delta must contain only what was recorded after the snapshot");
        assert(window.getP50() >= 512 &&
               "window percentile must reflect the 900ns sample, not the 100ns "
               "ones that preceded the snapshot");
        assert(t.getCount() == 3 && "deltaFrom must not mutate the source");
        // min/max cannot be recovered by subtraction and must not pretend to be.
        assert(window.getMax() == 0 && "delta max is not recoverable — must be 0");
    }

    // A delta against a mismatched snapshot saturates at zero rather than
    // wrapping the unsigned subtraction into an enormous phantom count.
    {
        LatencyTracker small, big;
        small.recordInterval(0, 100);
        big.recordInterval(0, 100);
        big.recordInterval(0, 100);
        LatencyTracker d = small.deltaFrom(big);
        assert(d.getCount() == 0 && "unsigned underflow was not guarded");
    }

    std::puts("LatencyTrackerIntervalTest passed");
    return 0;
}
