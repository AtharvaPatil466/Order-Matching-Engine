# BCS Study — Research Plan

Sequenced work to move the paper from a calibrated bracketing exercise to a
quantitative result. Ordering is load-bearing: each step is a precondition for
the one below it, and §5 is the item the rest exists to make interpretable.

Status as of 2026-07-23: steps 1–5 all open. Environment calibration and the
calibrated re-run of Exps 1–4 are complete (paper §3.7, §4.6).

---

## 1. Long-interval rent re-analysis — DONE 2026-07-23, negative result

**Concession to resolve.** §4.4 concedes that long-interval batch rent is
"either unmeasurably small or measurably absent" because per-seed dispersion
swamps the point estimate on the 50,000-tick operating-point grid.

**Approach.** Do not pair against the no-HFT baseline — its rent is zero by
construction. Pair *across clearing intervals*: same fundamental path, same
agent seeds, difference each batch cell against the **continuous** arm, which
is the only long-grid cell with a usable interval. Fundamental volatility is
the dominant variance term and is common to both arms, so it differences out.

**Anchor on the long grid (`exp4_batch_long.json`), not the calibrated
3,000-tick grid.** The calibrated grid's cells are already tight and have no
measurement problem; the open concession lives on the long grid.

**Outcome — the approach does not work, and cannot.** Differencing interval-500
rent against continuous seed-by-seed cut the standard error from 1055 to 1028,
a 2.6% improvement. It cannot do better for any anchor arm, because
`mean(c) + mean(x - c) == mean(x)` identically — the decomposition reassembles
into the original statistic, leaving the point estimate (−260.3) and its
variance unchanged. The premise failed: the dispersion is arm-specific, not
common-mode. Continuous rent is tightly determined (s.e. 64.5 about a mean of
578.1) while interval-500 rent is not (1055.3), so subtracting a near-constant
removes nothing. Variance comes from which clearings catch fundamental moves,
not from the path.

**What it did yield.** Pairing is powerless on levels but informative on
*contrasts*. Rent is significantly below continuous at intervals 5, 10 and 25
(−748 [−1356, −161], −830 [−1617, −43], −772 [−1472, −82]; all exclude zero,
while the unpaired levels are each indistinguishable from zero), and at
interval 1 is statistically indistinguishable from continuous (−6.2
[−72.1, 58.7], seed correlation 0.86) — clearing every tick does not attenuate
the race. Both folded into §4.4, along with the failed reduction, since a
concession that survives an honest attempt to remove it is stronger than one
merely asserted.

**Precondition — verified 2026-07-23.** `agents/fundamental_value.py:30` gives
`FundamentalValueProcess` a private `np.random.default_rng(seed)`, drawn
unconditionally per tick in `step()` (line 36). Agents hold separate
generators (`seed*1000+i`, `seed*2000+j`). Tick count is fixed by
`duration_us/dt_us`, so the fundamental path is bit-identical across arms for
a given seed however order flow diverges. Per-seed values are already stored
in each cell's `treatment_rows`, so this is a re-analysis of data on disk.

## 2. Two framing corrections — DONE 2026-07-23

- **Lead with the conservation identity; let TLA+ support it.** Done. The
  abstract now opens on the closed decomposition, states the residual check as
  the unconditional guarantee, and demotes the model check to support with its
  two limits (bounded scope; spec not source) stated in the same sentence
  rather than deferred to §1.2.
- **`Auction.tla` state count.** No action needed — the paper had *already*
  taken the honest option. §1.2 and §3.1 both explicitly decline to report a
  count "we cannot verify." Confirmed that the count is genuinely
  unrecoverable: `spec/states/` holds only a `.DS_Store`, the
  `Auction_TTrace_*.bin` files are counterexample traces from the deliberate
  `AuctionBroken.cfg` non-vacuity runs rather than state-count records, and
  `docs/Verification.md` records no exhaustive-run count for `Auction.tla`
  either. Recovering one would require re-running TLC.

**Unplanned finding — the paper was understating its own verification.**
`docs/Verification.md` records the headline `MatchingEngine.tla` run (which
*includes* the matching/cross action) as `MatchingEngine4.cfg`, `MaxOrders=4`:
368,192,427 states generated, **171,187,419 distinct**, BFS depth 11, empty
queue (complete exploration), zero violations, 2026-07-12. §1.2 was instead
citing the *fast* `MatchingEngine.cfg` (`MaxOrders=3`, 1.26e6 distinct) as the
verification bound — two orders of magnitude low — and asserted that "the
matching-inclusive model is necessarily smaller because adding execution forces
tighter bounds," which the 368M-state matching-inclusive run contradicts
outright. Both §1.2 and the §3.6 spec table now cite MaxOrders=4 with the
MaxOrders=3 default identified as the routine check, and the false parenthetical
is gone. This was exactly the class of claim step 2 existed to catch.

**Open, needs adjudication — conservation residual figures do not reconcile.**
The paper quotes a treatment residual of 2.78e-12 and baseline −5.39e-12 (§4.1,
restated §1.3 and §6). The stored `results/experiments/exp1_primary.json` has
treatment 5.82e-12 and baseline −1.14e-11. The abstract separately quoted
1.6e-13, inconsistent with all of the above; that figure has been dropped, since
an abstract does not need the digits and the claim ("indistinguishable from
floating-point noise") is unaffected. The body figures are left as-is pending a
decision on which run is authoritative — these are machine-zero residuals whose
exact digits vary run to run, so this is a reproducibility-hygiene issue rather
than a substantive one, but a referee re-running the harness would hit it.

## 3. Depth-depletion and replenishment measurement — IN PROGRESS

**3a. Depletion episodes and recovery — DONE 2026-07-23.**
`calibration/depth_replenishment.py`, results in
`results/calibration/depth_replenishment.json`.

Coverage was the binding constraint, not sample size. Snapshot cadence is a
clean 100 ms whenever the collector is up (median inter-snapshot gap 0.102 s),
but hourly coverage is bimodal: an hour file holds either ~35,290 rows or a few
hundred (median hour ~3,200), for ~34% overall book coverage. Since an episode
spans seconds and cannot be measured across a hole, the scan uses complete
hours only — 172 of 849, about 7.2 days of contiguous 10 Hz book. Partial hours
are counted and reported, never mixed in.

Result: 171,052 episodes (994.5/h), 0.6% censored at 30 s. Recovery to 50% of
a 30 s trailing-median baseline: p25 0.10 s, median 0.31 s, p75 0.92 s, p90
2.55 s. Median trough is 4.9% of baseline, so these are genuine collapses.

**The number step 4 needs.** The sim's single maker carries
`mm_latency_us=3000`, i.e. 300 ms of quote staleness at the 1 tick = 100 ms
convention. Real aggregate half-recovery is a median of 310 ms — effectively
identical — but p25 sits at the 0.10 s single-snapshot floor, so at least a
quarter of real episodes replenish faster than the data resolves. A lone maker
with 300 ms staleness cannot produce that fast quartile; competing makers can.
This converts "a single maker absorbs the entire race" from assertion into a
measured gap, and gives step 4 a calibration target.

**3b. Separate consumption from cancellation — DONE 2026-07-23.**
`calibration/trade_book_join.py`, results in
`results/calibration/trade_book_join.json`. Applies two corrections rather than
assuming them away: aggTrade sweep de-fragmentation (consecutive
`agg_trade_id`s sharing timestamp and aggressor side regrouped into one taker
order, repairing the numerator bias) and aggressor attribution via
`is_buyer_maker`.

**Only 32.0% of depletion episodes are consumption-driven**; the other
two-thirds are cancellations. So 3a was indeed conflating two phenomena — but
separating them barely moves the half-life. Recovery to 50% of baseline:
consumption p25 0.20 s / median 0.82 s / p90 6.63 s; cancellation p25 0.31 s /
median 0.82 s / p90 7.24 s. Identical medians. **Replenishment speed does not
much depend on why depth vanished**, which is a null worth reporting and
weakens the premise that the separation mattered for step 4's calibration
target.

**The ratio measurement is the payoff.** Taker size over *contemporaneous* top
depth on the consumed side, with the placebo quantifying dilution:

| conditioning | p50 | p90 | p99 | n |
|---|---|---|---|---|
| unconditional | 0.0072 | 0.393 | 18.0 | 2,269,256 |
| race proxy (trade ≤0.2 s after a top-of-book move) | 0.0283 | 4.24 | 290 | 390,540 |
| placebo (same window, no move) | 0.0057 | 0.208 | 2.18 | 1,849,504 |

The conditioning genuinely enriches: race-proxy p50 is 5x the placebo, p90 20x,
p99 133x. The dilution is bounded, not merely disclaimed.

**This inverts which calibrated arm is realistic.** The naive
ratio-of-medians estimate (0.003 / 6.182 = 0.0005) was wrong by ~14x; measured
per-trade against contemporaneous depth it is 0.0072 unconditional and 0.0283
under the race proxy. The sim's robustness arm sits at 0.024 — inside that
range. The sim's MAIN arm sits at 1.0, i.e. 35-140x larger than anything
observed. Note the denominator objection does not bite for the sim as built:
its single maker supplies the entire top of book, so `hft_qty/quote_qty` and
"taker size / aggregate top depth" are the same object. Which means the
incidence regime relevant to real BTC is plausibly the one where the maker
GAINS and noise traders bear the transfer — the opposite of the paper's
headline framing. Step 5 must therefore sweep ratios centred near 0.01-0.03,
not near 1.0.

**3c. Both problems resolved — 2026-07-23.**

*Hour attrition: explained, benign.* 32 of the 63 book days have no trades
directory at all — a 2026-05-17 to 2026-06-17 era when the book collector ran
but the trade poller did not (the `aggTrade` WebSocket was region-gated for the
host IP; the REST poller came later). 87 of the 173 complete book hours fall in
that window. Conversely 88 days have trades but no book, the earlier trade-only
era. 86 hours is therefore the correct denominator for any book-and-trade join,
not evidence of a defect.

*Episode-definition drift: fixed at the root.* The two modules had each
implemented the depletion loop, and they disagreed — 3a advanced past the
recovery, 3b advanced a single index, so a sustained depletion counted as one
event in 3a and as one event per snapshot-below-threshold in 3b. Rather than
align two copies, `depth_replenishment.iter_episodes` is now the single
canonical generator and `trade_book_join` consumes it, so the two cannot drift
again. Recovery quantiles now agree exactly: p25 0.10 s, median 0.31 s, p90
2.65 s in both.

**Corrected numbers — the figures in the previous commit were computed under
the buggy dedup and should not be used.** Consumption-driven share falls from
32.0% to **17.5%**: roughly one depletion in six is trade-driven, the rest are
cancellations. Median recovery falls from 0.82 s to 0.31 s. Consumption-driven
episodes now recover slightly *faster* than cancellation-driven ones (median
0.31 s against 0.41 s), which is the sensible direction — depth taken by a
trade is replaced, depth withdrawn by choice is not — and reverses the "no
difference" null reported before, though the gap is one snapshot and should not
be leaned on.

The ratio measurements are unaffected: they key off trades and contemporaneous
depth, not episode boundaries, so all three conditionings are unchanged.

### Original scoping notes

**Why it comes before the multi-maker model.** Competing makers cannot be
designed without knowing what real replenishment looks like. The same
depth-depletion definition also unblocks the Hawkes real-data leg (§5.3), so
one piece of tooling closes two open items.

**Definition.** A depletion event is top-k depth falling below a percentile
floor; measure the depth-recovery profile and its half-life. Replenishment,
not maker headcount, is the mechanically load-bearing quantity — and maker
count `N` is not observable anyway, since Binance's depth stream publishes
quantity per level with no order counts.

**Deliverable is a bounded region, not a ratio.** Two known mis-specifications
prevent a point estimate of snipe-to-quote ratio:

- *Denominator.* The sim's `quote_qty` is one maker's entire exposed quote;
  real top-of-book depth is an aggregate. At N makers the sim-equivalent ratio
  is `0.003 / (6.182/N)`; N ≈ 50 lands exactly on the small-snipe arm's 0.024,
  while N = 1 gives 0.0005. Opposite qualitative regimes.
- *Numerator.* Median trade size is retail flow. Race orders sweep, and live
  nearer the p99 of 2.01 BTC. Worse, Binance aggregates fills per taker order
  *per price level*, so multi-level sweeps fragment into one record per level —
  the tape systematically under-measures exactly the orders that matter.

Neither replenishment nor `N` recovers the exposure of the *particular* maker
being sniped, so the ratio stays a swept parameter. That is acceptable because
§5 sweeps it: the job here is to bound where real BTC plausibly sits on that
surface, with the proxy and its dilution stated.

**Resolution limit.** Book snapshots are 100 ms; the race is microseconds.
Any "aggressive trade shortly after a top-of-book move" filter is
dilution-limited by roughly three orders of magnitude — report as a bound, not
a measurement. Bound the dilution by conditioning identically on windows
following *non-informative* events (trades that did not move the top) and
differencing. This reuses the §4.5 placebo arm, but note its validation was
established on simulated gaps and does **not** transfer automatically to real
data; re-establish it before leaning on it.

## 4. Competing makers with heterogeneous latencies

Parameterized from (3). This is the binding constraint on external validity: a
single latency-disadvantaged maker has no competitor to replenish a cleared
quote, so it absorbs the entire race, which is why §4.6 reports the calibrated
4.42 bp as an upper bound.

**Do not overclaim the payoff.** This removes the single-maker absorption bias.
It does not make the rent an estimate: the race structure (`hft_latency_us`,
`hft_race_noise_us`) remains a swept design parameter, since measuring races
needs failed-order message data of the kind Aquilina, Budish and O'Neill (2022)
obtained from a regulator (§3.7). The result is a calibrated environment with
competitive liquidity supply and a *designed* race.

## 5. The (ratio, k) incidence surface — the finding

Sweep snipe-to-quote ratio against HFT count and map where the incidence of
the transfer flips. §4.6 shows incidence is ratio-dependent at fixed k (at
0.024 the maker gains for k ≤ 5 and loses by k ≥ 8; at 0.0005 with many HFTs
nothing has been run). Two ratio points and one k-sweep do not locate a
boundary in a two-dimensional space.

**Must follow (4).** The maker's gain at small snipe sizes is plausibly itself
a single-maker artifact — it is the sole liquidity supplier monetizing flow its
own widening created. Mapping the surface before (4) would produce a boundary
that is an artifact of the thing (4) exists to remove.

**Cost.** ~6 ratios × 8 k values × 100 seeds ≈ 5k runs. The calibrated k-sweep
did 800 runs in ~7 minutes, so the surface is roughly an hour. (4) is the
expensive item, not this.

**Why it is the point.** "Rent is extracted from the market maker" restates
BCS. "Who bears the latency tax depends on snipe-to-depth ratio and competition
count, and here is the regime boundary" is in neither BCS nor Aquilina, Budish
and O'Neill, and follows from apparatus that already exists.

---

## Target

arXiv q-fin.TR plus a microstructure workshop. A field journal would want (4)
and (5) landed first.
