#!/usr/bin/env python3
"""Experiment 15 — held-out test of which competing maker pays (PREREGISTRATION P10).

Why this exists. The paper's within-class headline was that among competing
makers one gains while another pays, and that who pays is "set by how the
makers' latencies are spaced, not by speed rank". Its support was a count of
136/144 cells, which is pseudo-replicated: the cells share seeds and eight
latency configurations. A reviewer asked for a held-out prediction test
instead. P10 fixes a parameter-free predictor, "the slowest maker pays", and
a decision rule. Both are fixed in this file before any test configuration
is generated: the commit that adds this predictor precedes the commit that
adds the configuration generator.

Unit of replication: the CONFIGURATION. Each configuration gets its own block
of seeds; it contributes one hit or miss and one Spearman sign. Cells that
share seeds are never counted separately.

Decision rule (P10, read with PREREGISTRATION section 2's band convention:
a threshold on a proportion is met when the 95% Wilson interval clears it,
and a miss when the interval lies wholly on the wrong side):
  MISS          Wilson upper bound of the overall hit rate < 0.60, or of the
                closely spaced configurations' hit rate < 0.50;
  HIT           otherwise, if the Wilson lower bounds of the hit rate AND of
                the share of configurations with Spearman(latency, PnL delta)
                < 0 are both >= 0.80;
  INCONCLUSIVE  otherwise.
The point-estimate reading (rates >= 0.80) is reported beside it and never
replaces it. A one-sided exact test against chance (a configuration with M
makers hits by luck with probability 1/M) is reported too.
"""
from __future__ import annotations

import math

import numpy as np
from scipy import stats

HIT_BAND = 0.80          # P10: right in >= 80% of held-out cells
MISS_OVERALL = 0.60      # P10: miss below 60%
MISS_CLOSE = 0.50        # P10: miss below 50% on the closely spaced sets
Z95 = 1.959963984540054


# --- the predictor (P10, parameter-free) ------------------------------------

def predicted_payer(latencies) -> int:
    """Index of the maker P10 says pays: the one with the largest latency."""
    lat = list(latencies)
    if len(set(lat)) != len(lat):
        raise ValueError(f"latencies must be distinct, got {lat}")
    return int(np.argmax(lat))


def is_hit(latencies, deltas) -> bool:
    """The maker with the most negative PnL delta is the slowest one."""
    return int(np.argmin(deltas)) == predicted_payer(latencies)


def spearman(latencies, deltas) -> float:
    """Spearman rank correlation of latency with PnL delta (average ranks on ties)."""
    a, b = stats.rankdata(latencies), stats.rankdata(deltas)
    if np.ptp(a) == 0 or np.ptp(b) == 0:
        return 0.0
    return float(np.corrcoef(a, b)[0, 1])


# --- the statistics ---------------------------------------------------------

def wilson(successes: int, n: int, z: float = Z95) -> dict:
    """Wilson score interval for a proportion."""
    if n <= 0:
        return {"rate": float("nan"), "lower": 0.0, "upper": 1.0, "n": 0, "k": 0}
    p = successes / n
    den = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / den
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / den
    return {"rate": p, "lower": max(0.0, centre - half), "upper": min(1.0, centre + half),
            "n": n, "k": successes}


def chance_pvalue(hits: int, maker_counts) -> float:
    """P(at least `hits` hits) if each configuration with M makers hits w.p. 1/M.

    Exact Poisson-binomial upper tail, since configurations mix maker counts.
    """
    dist = np.array([1.0])
    for m in maker_counts:
        p = 1.0 / m
        dist = np.convolve(dist, [1 - p, p])
    return float(dist[hits:].sum())


def classify(per_config) -> dict:
    """P10's verdict over configurations.

    per_config: dicts with "latencies", "deltas" (one mean PnL delta per maker,
    same order) and "closely_spaced" (bool).
    """
    hits = [is_hit(c["latencies"], c["deltas"]) for c in per_config]
    neg = [spearman(c["latencies"], c["deltas"]) < 0 for c in per_config]
    close = [h for h, c in zip(hits, per_config) if c["closely_spaced"]]
    overall, rho, close_w = wilson(sum(hits), len(hits)), wilson(sum(neg), len(neg)), \
        wilson(sum(close), len(close))
    if overall["upper"] < MISS_OVERALL or (close and close_w["upper"] < MISS_CLOSE):
        verdict = "MISS"
    elif overall["lower"] >= HIT_BAND and rho["lower"] >= HIT_BAND:
        verdict = "HIT"
    else:
        verdict = "INCONCLUSIVE"
    point = ("MISS" if overall["rate"] < MISS_OVERALL or (close and close_w["rate"] < MISS_CLOSE)
             else "HIT" if overall["rate"] >= HIT_BAND and rho["rate"] >= HIT_BAND
             else "INCONCLUSIVE")
    return {
        "verdict": verdict,
        "verdict_point_estimate": point,
        "hit_rate": overall,
        "spearman_negative_share": rho,
        "closely_spaced_hit_rate": close_w,
        "chance_pvalue": chance_pvalue(sum(hits), [len(c["latencies"]) for c in per_config]),
        "chance_rate": float(np.mean([1 / len(c["latencies"]) for c in per_config])),
        "n_configs": len(per_config),
    }
