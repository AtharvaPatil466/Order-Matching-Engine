"""The signed-off operating point (PREREGISTRATION §5), as code, once.

Why this exists. §5 fixes the event model's operating point before any
confirmatory run. Every confirmatory driver must run at exactly those values,
and a value typed twice is a value that can drift, so every driver imports
them from here and nothing else. The test (tests/test_operating_point.py) pins
each one against §5's table.

What §5 fixes, and where each value comes from:
  path        JumpDiffusionPath.from_calibration(jump_fit_v2): public updates
              every 10 ms, diffusive sigma 0.242 bp/sqrt(s), 0.200 Laplace jumps
              per s with b = 0.705 bp. v0 = 1e4 / tick_bp = 630,927, so one
              engine unit is the venue's 0.10 USD tick. Read from the
              calibration files through facts_event_model.tape_config, not
              retyped.
  investors   the facts lane's ReservationNoiseTrader at the tape's 16.28
              crossings/s (§5 prints 16.3), lognormal sizes with median 3 lots
              and sigma_ln 2.80, private-value scale 0.52 bp. The arrival rate
              is fixed at the one that gives 16.28 crossings/s against the BCS
              half-spread, as in facts_event_model, and is NOT re-solved per
              arm: the investor stream must be identical across arms (common
              random numbers), whatever the maker quotes.
  quote       6,180 lots in aggregate (§5; the tape's 6.182 BTC rounded as §5
              signed it), split across M makers with the remainder to slot 0.
  snipes      snipe size = round(ratio x 6,180) lots, ratio from the paper
              band {0.0049, 0.0097} or the corrected tape band {0.052, 0.18}
              (§4's first amendment). Each prediction states its own ratios.
  snipers     100 us total latency (50 us to observe + 50 us to order), with
              U{0..80 us} race jitter per message burst. The jitter applies to
              every racing firm, makers included: race_win_probability(lag, eps,
              k), which P2 and P7 test, assumes each racing message draws it.
              Investors are not racing firms and get none.
  fees        maker 0 / taker 1.7 bp (Binance USD-M VIP 9). A sniper fires only
              when the stale quote is worth more than its taker fee, so its
              threshold is the taker fee of whatever schedule a run uses.
  run         30 simulated minutes; n = 100 seeds per cell unless a prediction
              states otherwise.

What §5 leaves to each prediction: maker count and latencies, sniper count k,
snipe ratio, fees (P5's grid), and the maker's policy. build_market takes
exactly those. Its default maker is the facts lane's: one risk-neutral
StrategicMaker at the BCS zero-profit half-spread, as fast as the snipers.

Not in the operating point: facts_event_model's SlowInformedInvestor. §5 lists
the market's participants and it is not among them (P6 adds its own), so it is
left out here. Logged as an ambiguity for §4.

Seeds, as facts_event_model: path `seed`, investors [seed, 1], scheduler (tie
shuffles and per-agent jitter streams) `seed`. So any two arms of one seed
share the path, the investor arrivals and their private values.
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path
from typing import NamedTuple, Sequence

_ROOT = Path(__file__).resolve().parents[1]
for _d in ("build", "agents", "simulation", "metrics", "analysis", "calibration", "experiments"):
    _p = str(_ROOT / _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)

import bcs_engine as be  # noqa: E402
import facts_event_model as facts  # noqa: E402
from accounting import FeeSchedule  # noqa: E402
from bcs_theory import laplace_half_spread  # noqa: E402
from event_noise import ReservationNoiseTrader  # noqa: E402
from event_scheduler import EventScheduler  # noqa: E402
from event_sniper import EventSniper  # noqa: E402
from fundamental_paths import JumpDiffusionPath  # noqa: E402
from strategic_maker import MakerPolicy, StrategicMaker  # noqa: E402

CONFIRMATORY_STATUS = "confirmatory — pre-registration signed off 2026-10-12 (6c46c01)"
RESULTS_DIR = _ROOT / "results" / "experiments"

NS_PER_S, NS_PER_US = 1_000_000_000, 1_000
_TAPE = facts.tape_config()

# --- §5's values ---------------------------------------------------------------
PATH = dict(_TAPE["path"])                     # jump_fit_v2 at 10 ms, v0 = 630,927
V0 = PATH["v0"]
BP = V0 * 1e-4                                 # engine units per bp
TICK_BP = _TAPE["tick_bp"]
UPDATE_INTERVAL_NS = int(PATH["update_interval_ms"] * 1_000_000)

INVESTOR_RATE_PER_S = _TAPE["noise"]["trade_rate_per_s"]    # 16.28 crossings/s
INVESTOR_MEDIAN_LOTS = _TAPE["noise"]["median_lots"]        # 3 lots of 0.001 BTC
INVESTOR_SIZE_SIGMA_LN = _TAPE["noise"]["size_sigma_ln"]    # 2.80
INVESTOR_EPS_BP = _TAPE["noise"]["eps_bp"]                  # 0.52 bp

QUOTE_LOTS = 6_180
PAPER_BAND = (0.0049, 0.0097)
TAPE_BAND = (0.052, 0.18)
BANDS = {"paper": PAPER_BAND, "tape": TAPE_BAND}            # §4: verdicts read on "paper"
K_GRID = (1, 2, 3, 5, 6, 7, 8, 13, 21)                      # §2, exp6_incidence_surface.K_GRID

SNIPER_LATENCY_NS = 100 * NS_PER_US                         # total: observe + order
SNIPER_ONE_WAY_NS = SNIPER_LATENCY_NS // 2
JITTER_NS = 80 * NS_PER_US                                  # race jitter eps

FEES = FeeSchedule(maker_bps=0.0, taker_bps=facts.VIP9_TAKER_BP)
HORIZON_S = 1800
N_SEEDS = 100
SNAPSHOT_MS = facts.CFG["snapshot_ms"]

# Bootstrap convention (§2): 95% percentile, 2,000 resamples, seed 0.
BOOT_SEED, N_BOOTSTRAP = 0, 2000

# The facts lane's maker: BCS's zero-profit half-spread at the fitted path, as
# fast as the snipers (lag 0). Predictions override both.
BCS_HALF_SPREAD_BP = laplace_half_spread(INVESTOR_RATE_PER_S, PATH["jump_intensity_per_s"],
                                         PATH["jump_scale_bp"])
MAKER_ONE_WAY_NS = SNIPER_ONE_WAY_NS
INVESTOR_ARRIVAL_PER_S = INVESTOR_RATE_PER_S / facts._crossing_probability(
    BCS_HALF_SPREAD_BP, INVESTOR_EPS_BP)

MAKER, SNIPER, NOISE = facts.MAKER, facts.SNIPER, facts.NOISE   # participant id blocks


def snipe_lots(ratio: float) -> int:
    """Snipe size for a snipe/quote ratio: round(ratio x the aggregate quote), at least 1."""
    if ratio <= 0:
        raise ValueError(f"snipe ratio must be positive, got {ratio}")
    return max(1, round(ratio * QUOTE_LOTS))


def split_qty(total: int, n: int) -> list[int]:
    """Aggregate depth held fixed across n makers, remainder to slot 0 (exp1_primary)."""
    base = total // n
    return [base + (total - base * n if i == 0 else 0) for i in range(n)]


def default_policy(qty: int = QUOTE_LOTS, **overrides) -> MakerPolicy:
    """The facts lane's maker at the BCS half-spread, in engine units (ticks)."""
    return MakerPolicy(half_spread=BCS_HALF_SPREAD_BP * BP, qty=qty, **overrides)


class Market(NamedTuple):
    agents: list
    path: object
    race_jitter_ns: dict      # pid -> eps, every racing firm
    makers: list
    snipers: list
    noise: object


def build_market(seed: int, *, maker_latencies_ns: Sequence[int] = (MAKER_ONE_WAY_NS,),
                 k: int = 3, snipe_ratio: float = PAPER_BAND[0], fees: FeeSchedule = FEES,
                 policies: Sequence[MakerPolicy] | None = None,
                 horizon_s: float = HORIZON_S, sniper_one_way_ns: int = SNIPER_ONE_WAY_NS,
                 jitter_ns: int = JITTER_NS) -> Market:
    """One run's market at §5's operating point.

    maker_latencies_ns: one entry per maker, its ONE-WAY latency (observe =
    order), as facts_event_model and exp15 use it; a maker's total latency is
    twice this. policies: one MakerPolicy per maker; None gives each the
    default policy with the aggregate quote split across them. k = 0 is the
    no-sniper baseline: the same market with the snipers removed.
    """
    m = len(maker_latencies_ns)
    if m < 1 or k < 0:
        raise ValueError(f"need >= 1 maker and k >= 0, got {m} makers, k = {k}")
    if policies is None:
        policies = [default_policy(q) for q in split_qty(QUOTE_LOTS, m)]
    if len(policies) != m:
        raise ValueError(f"{len(policies)} policies for {m} makers")
    path = JumpDiffusionPath.from_calibration(PATH, int(horizon_s * NS_PER_S), seed=seed)
    makers = [StrategicMaker(MAKER + i, p, path=path, md_latency_ns=lat, order_latency_ns=lat)
              for i, (p, lat) in enumerate(zip(policies, maker_latencies_ns))]
    snipers = [EventSniper(SNIPER + j, qty=snipe_lots(snipe_ratio),
                           threshold=fees.taker_bps * BP,
                           md_latency_ns=sniper_one_way_ns, order_latency_ns=sniper_one_way_ns)
               for j in range(k)]
    noise = ReservationNoiseTrader(NOISE, arrival_rate_per_s=INVESTOR_ARRIVAL_PER_S,
                                   eps_scale=INVESTOR_EPS_BP * BP, v0=V0,
                                   qty=INVESTOR_MEDIAN_LOTS,
                                   size_sigma_ln=INVESTOR_SIZE_SIGMA_LN, seed=[seed, 1])
    jitter = {a.participant_id: jitter_ns for a in makers + snipers} if jitter_ns else {}
    return Market(makers + snipers + [noise], path, jitter, makers, snipers, noise)


def simulate(seed: int, market: Market | None = None, **market_kw):
    """One EventSimResult on a fresh engine; builds the market unless given one."""
    mk = market or build_market(seed, **market_kw)
    h = be.EngineHarness()
    h.add_symbol(1)
    h.start(disable_circuit_breaker=True)
    try:
        return EventScheduler(h, 1, seed=seed, snapshot_every_ns=SNAPSHOT_MS * 1_000_000,
                              race_jitter_ns=mk.race_jitter_ns).run(mk.agents, mk.path)
    finally:
        h.stop()


def edge_by_participant(trades, path) -> dict:
    """Each participant's fills marked at the true V at execution (exp10.edge_pnl), USD.

    V is a martingale, so this is the P&L's expectation without the end mark's
    inventory-carry noise. Zero-sum across participants.
    """
    out: dict[int, float] = {}
    for tr in trades:
        e = (path.value_at(tr.t_ns) - tr.price) * tr.quantity / be.PRICE_PRECISION
        out[tr.buyer_id] = out.get(tr.buyer_id, 0.0) + e
        out[tr.seller_id] = out.get(tr.seller_id, 0.0) - e
    return out


# --- §2's verdict rules ------------------------------------------------------------

def band_verdict(lower: float, upper: float, band_lo: float, band_hi: float) -> str:
    """§2: HIT if the 95% interval lies inside the band, MISS if disjoint, else INCONCLUSIVE."""
    if band_lo <= lower and upper <= band_hi:
        return "HIT"
    if upper < band_lo or lower > band_hi:
        return "MISS"
    return "INCONCLUSIVE"


def equivalence(a, b, margin: float, seed: int = BOOT_SEED) -> dict:
    """§2's TOST for "invariant" predictions, on mean(a)/mean(b) - 1 paired by seed.

    HIT if the 90% bootstrap interval lies inside +-margin (TOST at alpha =
    0.05), MISS if the 95% interval is disjoint from it ("moving by more than
    the margin"), INCONCLUSIVE otherwise.
    """
    import numpy as np
    a, b = np.asarray(a, float), np.asarray(b, float)
    if a.shape != b.shape or a.size < 2:
        raise ValueError("equivalence needs paired samples of equal length >= 2")
    idx = np.random.default_rng(seed).integers(0, a.size, size=(N_BOOTSTRAP, a.size))
    r = a[idx].mean(1) / b[idx].mean(1) - 1
    lo90, hi90, lo95, hi95 = np.percentile(r, [5, 95, 2.5, 97.5])
    if -margin <= lo90 and hi90 <= margin:
        verdict = "HIT"
    elif hi95 < -margin or lo95 > margin:
        verdict = "MISS"
    else:
        verdict = "INCONCLUSIVE"
    return {"ratio_minus_1": float(a.mean() / b.mean() - 1), "ci90": [float(lo90), float(hi90)],
            "ci95": [float(lo95), float(hi95)], "margin": margin, "verdict": verdict}


def all_hit(verdicts) -> str:
    """A conjunction of verdicts: MISS if any part misses, HIT only if all hit."""
    v = list(verdicts)
    return "MISS" if "MISS" in v else ("HIT" if v and all(x == "HIT" for x in v) else "INCONCLUSIVE")


def wilson(k: int, n: int, z: float = 1.959963984540054) -> dict:
    """Wilson score interval for k successes in n (§2: proportions use Wilson)."""
    if n <= 0:
        return {"mean": math.nan, "lower": 0.0, "upper": 1.0, "k": k, "n": n}
    p = k / n
    den = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / den
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / den
    return {"mean": p, "lower": max(0.0, centre - half), "upper": min(1.0, centre + half),
            "k": k, "n": n}


# --- resumable cells ---------------------------------------------------------------

class CellLog:
    """Append-only JSONL checkpoint, one line per finished cell or run.

    The usage-limit and session restarts are real, so every confirmatory
    driver writes each unit of work as soon as it finishes and, on restart,
    skips the keys already logged. A torn last line (killed mid-write) is
    ignored and its unit re-run.
    """

    def __init__(self, path: Path):
        self.path = Path(path)
        self.done: dict[str, object] = {}
        if self.path.exists():
            for line in self.path.read_text().splitlines():
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                self.done[rec["key"]] = rec["value"]

    def __contains__(self, key: str) -> bool:
        return key in self.done

    def get(self, key: str):
        return self.done[key]

    def put(self, key: str, value) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("a") as fh:
            fh.write(json.dumps({"key": key, "value": value}) + "\n")
        self.done[key] = value


def confirmatory_paths(name: str, out_dir: Path | None = None) -> tuple[Path, Path]:
    """(summary JSON, checkpoint JSONL) for a confirmatory driver `name`."""
    d = Path(out_dir) if out_dir else RESULTS_DIR
    return d / f"{name}.json", d / "checkpoints" / f"{name}.jsonl"


def run_cached(fn, tasks: dict, log: CellLog, workers: int = 1, progress=print) -> dict:
    """{key: fn(*args)} for every task, skipping keys `log` already holds.

    tasks: {key (str): args tuple}. fn must be a module-level function (the
    pool pickles it). Each result is logged the moment it lands, so a killed
    batch loses at most the runs in flight. workers == 1 runs inline.
    """
    from concurrent.futures import ProcessPoolExecutor, as_completed
    todo = {k: a for k, a in tasks.items() if k not in log}
    done = 0
    if todo and workers == 1:
        for k, a in todo.items():
            log.put(k, fn(*a))
            done += 1
    elif todo:
        with ProcessPoolExecutor(max_workers=workers) as pool:
            futs = {pool.submit(fn, *a): k for k, a in todo.items()}
            for f in as_completed(futs):
                log.put(futs[f], f.result())
                done += 1
                if done % 200 == 0:
                    progress(f"  {done}/{len(todo)} runs ({len(tasks) - len(todo)} resumed)")
    return {k: log.get(k) for k in tasks}


def smoke_status(n_seeds: int, horizon_s: float, n_required: int = N_SEEDS,
                 horizon_required: float = HORIZON_S) -> str:
    """CONFIRMATORY_STATUS only at the pre-registered sizes; anything smaller is a smoke run."""
    if n_seeds >= n_required and horizon_s >= horizon_required:
        return CONFIRMATORY_STATUS
    return (f"smoke — NOT confirmatory: {n_seeds} seeds x {horizon_s:g} s against the "
            f"pre-registered {n_required} x {horizon_required:g} s")
