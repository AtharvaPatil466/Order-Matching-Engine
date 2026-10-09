# Pre-registration: the event-driven rebuild of the BCS research program

**Status: DRAFT — pending author sign-off.** Nothing below may be edited after
the first run of any experiment it covers except through the Deviations log
(§4). The git commit that adds this file is its timestamp; it must precede the
first commit of any result file under `results/` produced by the event model.

Theory references are to Budish, Cramton & Shim (2015, QJE 130(4)), cited as
BCS with their equation and proposition numbers, and to
`analysis/bcs_theory.py`, which implements every closed form used here.
Formulas marked *derived* are ours, not BCS's.

## 1. Why pre-register, and what counts

A reviewer asked that each new experiment carry predictions fixed before it
runs, with hits and misses reported. A pre-registration in which every outcome
counts as a hit is worthless, so each prediction below names a direction, a
magnitude wherever theory supplies one, a test, and the outcome that would be
a **miss**. Several predictions say the paper's current headlines will *not*
survive the rebuild (§3); those are the ones most worth being wrong about in
public.

## 2. Conventions that apply to every experiment

- **Operating point.** The event model's operating point (latencies, jitter,
  arrival and jump rates, jump scale, quote and snipe sizes, run length) is
  fixed in the sign-off table (§5) before any run. Predictions written as
  formulas of it are fixed *now*; only the numbers they evaluate to wait for
  the table.
- **Seeds and pairing.** n = 100 seeds per cell unless stated. Arms of one
  experiment share seeds, so the fundamental path and noise arrivals are
  common random numbers; effects are paired per-seed differences.
- **Intervals.** 95% percentile bootstrap of the mean, 2,000 resamples,
  seed 0 (`metrics/bootstrap.py`). Proportions use Wilson intervals.
- **Primary vs secondary.** Each experiment has one primary hypothesis
  (marked **primary**). The 11 primaries are one family, Holm-adjusted at
  α = 0.05. Secondaries use unadjusted 95% intervals and are reported as such.
- **Equivalence.** "No effect" and "invariant" predictions are tested by TOST
  with the stated margin, not by failing to reject.
- **Classification.** Every prediction ends as exactly one of:
  - **HIT** — the decision rule is met;
  - **MISS** — the stated miss condition is met;
  - **INCONCLUSIVE** — neither (the interval straddles the boundary).
  Inconclusive results are never counted as hits. If more than a third of the
  primaries are inconclusive, the program is underpowered and the writeup says
  so before reporting anything else.
- **Magnitude bands.** "Within ±x of the prediction" means: HIT if the 95%
  interval lies inside the band, MISS if it is disjoint from it, otherwise
  INCONCLUSIVE. Not "the interval contains the prediction": across a dozen
  cells that rule fails a correct model about half the time by chance. In a
  miss line, "outside ±x" means the interval is disjoint from the band.
- **Tape band.** Where a prediction counts "maker-gains cells at the tape
  band", it means the paper's 18 cells: snipe/quote ratios {0.0049, 0.0097} ×
  HFT counts {1, 2, 3, 5, 6, 7, 8, 13, 21} (`exp6_incidence_surface.K_GRID`),
  a cell counting as maker-gains when the maker's PnL delta against
  the matched no-HFT baseline has a 95% interval above zero (§4.1's rule).

## 3. What we expect to happen to the current paper's claims

| Paper claim (draft §) | Prediction under the event model | Experiment |
|---|---|---|
| Single-maker inversion: the maker gains at the tape-measured ratio (§4.1) | **Does not survive** once the maker cancels on signal at ≤ 1 ms latency: ≤ 2 of 18 tape-band cells maker-gains | P2 (H2d), P7 (H7d) |
| Total rent flat in HFT count k when one snipe clears the quote (§4.3, Exp 3) | Holds only for a maker slower than every sniper; a maker as fast as the snipers wins 1/(k+1) of races, so rent grows as k/(k+1) | P2 (H2b) |
| Liquidity gaps linear in k, ≈ 2.1k + 3.7 (§4.3) | **Does not survive**: gaps(k=21)/gaps(k=3) < 2, against 4.9 in the tick model | P2 (H2c) |
| Within-class redistribution, who pays set by latency spacing not rank (§4.2) | Redistribution survives but becomes **monotone in latency**: the slowest maker pays in ≥ 80% of held-out cells, including closely spaced sets | P10, P8 (H8c) |
| Interval-1 batch backfire (§4.3, Exp 4) | **Does not survive**: no batch interval is significantly worse for the maker than continuous trading | P9 (H9c) |
| Batching recovers maker welfare (§4.3) | Survives, and the residual is bounded by BCS footnote 34's exposure | P9 (H9a) |
| "No cost of speed is modelled" (§4.3) | With a cost of speed and free entry the transfer becomes waste: ≥ 85% of the rent is spent on speed at the pre-set cost | P4 |

## 4. Deviations log

Any change to a hypothesis, metric, test, operating point or sample size after
sign-off is recorded here with the date, the reason, and whether any result of
the affected experiment had been seen. Rows are never deleted.

| Date | Experiment | Change | Results seen? | Reason |
|---|---|---|---|---|
| — | — | — | — | — |

## 5. Sign-off

Filled in by the author before the first event-model run; until then the
numerical evaluations in §6 are pending, the formulas are not.

| Item | Value |
|---|---|
| Event-model operating point (latencies, jitter ε, λ_noise, λ_jump, Laplace scale b, diffusion σ, quote and snipe sizes, run length) | *pending* |
| Random-walk comparison cadence and per-update σ (P3) | *pending* |
| Cost of speed grid (P4), fee grid (P5), reservation-value law (P6), A–S parameters (P7), strategy grid (P8), batch intervals (P9), Sobol ranges (P11) | *pending; defaults below apply if left blank* |
| Signed by / date | *pending* |

## 6. The experiments

Notation: λ_i investor (noise) arrival rate, λ_j signal-jump rate, J the
absolute jump size, b its Laplace scale (|jump| ~ Exp(mean b)), h a quoted
half-spread, k the number of snipers, N = k + 1 the number of racing firms
when the liquidity provider (LP) is as fast as the snipers, ε the scheduler's
per-message jitter width, lag the LP's latency minus the snipers'.

---

### P1. BCS replication

The full protocol, parameters and tolerances are in
`paper/bcs_replication_spec.md`; this entry fixes the predictions.

- **Hypothesis.** Configured to BCS's assumptions (one LP, N − 1 snipers,
  equal latency, public Laplace jump signal, one-unit Poisson investors, no
  fees), the event model recovers BCS's equilibrium.
- **Predictions (magnitudes from BCS eq. 3 and Prop 1).**
  - **primary** — race-loss share = (N − 1)/N for N ∈ {2, 3, 5, 10}, each
    within ±0.02 absolute (40 seeds × 600 s per cell, the spec's sizing);
  - empirical indifference half-spread ĥ (LP profit = mean sniper profit)
    within ±3% of h* = b·W(λ_j/λ_i) in all 12 cells, and independent of N
    (slope of ĥ on (N − 1)/N: 95% interval inside ±3% of h*);
  - empirical LP breakeven ĥ₀ (LP profit = 0) within ±3% of
    h₀(N) = b·W(((N − 1)/N)·λ_j/λ_i);
  - mean profit per successful snipe within ±3% of b (Laplace memorylessness);
  - batch arm: zero snipes at ε = 0; at ε > 0, stale-quote executions per jump
    match (ε/τ)(N − 1)/(2(N + 1)) (derived refinement of BCS footnote 34)
    within the Poisson interval.
- **Test.** As in the spec, §5.
- **Miss.** Any primary cell whose interval is disjoint from its band; or
  ĥ trending in N (slope interval disjoint from ±3% of h*).
  We expect every continuous-market cell to be a HIT; a miss here would mean
  the scheduler, not BCS, is wrong, and every later experiment waits on it.

### P2. Cancel-vs-snipe race incidence under the event clock

- **Hypothesis.** On a nanosecond clock the maker's cancel and the snipers'
  orders genuinely race, so the maker's sniping loss is set by its race-win
  probability, not by a fixed staleness window.
- **Predictions.**
  - **primary (H2a)** — the share of snipe-able jumps on which the maker's
    cancel arrives first equals `race_win_probability(lag, ε, k)` =
    ((ε − lag)/ε)^(k+1)/(k+1) for lag ≤ ε, 0 beyond (*derived*), for
    k ∈ {1, 3, 8, 21} and lag/ε ∈ {0, 0.25, 0.5, 1.5}: within ±0.02 absolute.
    At lag 0 that is 1/(k + 1):
    0.50, 0.25, 0.11, 0.045.
  - **H2b** — for a fixed-spread maker that cancels on every signal (the
    replication spec's LP), with snipe size = quote size, total sniper rent at
    lag 0 grows as k/(k + 1): rent(k)/rent(21) = (k/(k + 1))/(21/22), so 0.52 at k = 1 and
    0.79 at k = 3, each within ±10%. At lag > ε it is flat: every
    rent(k)/rent(21) in [0.9, 1.1] (TOST). The tick model's flat rent (paper
    Exp 3) is the slow-maker special case.
  - **H2c** — liquidity gaps do not grow linearly in k: gaps(21)/gaps(3) < 2
    at both lags (tick model: 47.47/9.70 = 4.9).
  - **H2d** — the paper's adverse-selection maker, re-implemented to cancel
    on each observed signal update, shows the single-maker inversion only
    when slow. Tape-band maker-gains cells: ≤ 2 of 18 at maker latency
    ≤ 1 ms; the count is non-decreasing over maker latencies
    {0.01, 0.1, 1, 10, 100, 300} ms; ≥ 12 of 18 at 300 ms (the tick model's
    3 ticks). Reason: the inversion is the maker's widening cutting the
    baseline adverse selection noise traders inflict on stale quotes, and
    that baseline scales with λ_noise × maker staleness.
- **Metric.** Per-jump race outcomes from the scheduler's arrival log; HFT
  PnL; gap counts (§3.3 definition, unchanged); maker PnL delta.
- **Test.** Wilson intervals (H2a); bootstrap ratio intervals (H2b, H2c);
  the §4.1 cell rule (H2d).
- **Miss.** H2a: any cell whose interval is disjoint from ±0.02 of the formula.
  H2b: rent ratio at k = 1, lag 0 above 0.65 (i.e. still near flat).
  H2c: gaps(21)/gaps(3) ≥ 3. H2d: ≥ 6 of 18 maker-gains cells at
  ≤ 1 ms, or fewer than 6 at 300 ms.

### P3. Jump-diffusion vs random-walk fundamental

- **Hypothesis.** BCS's rent comes from jumps exceeding the half-spread
  (Prop 5: the prize depends on the jump law). At matched quadratic
  variation, fat-tailed Laplace jumps and a Gaussian random walk produce
  different sniping economics, and the difference is predictable.
- **Predictions.**
  - **primary (H3a)** — with the BCS LP at a fixed half-spread h and equal
    latency, the ratio of sniper rent rates (Laplace jumps / random walk) at
    matched variance rate λ_j·2b² = λ_rw·σ_u² equals
    [λ_j·b·e^(−h/b)] / [λ_rw·2(σ_u·φ(h/σ_u) − h·(1 − Φ(h/σ_u)))] within ±10%.
    At b = 1000 ticks, λ_j = 10/s, h = 567 ticks this is 0.87, 1.30 and
    ≈ 2,900 for random-walk cadences of 10, 100 and 1,000 updates/s: the
    comparison is dominated by the random walk's update cadence, which the
    sign-off must fix.
  - **H3b** — mean profit per successful snipe is b (±5%) at every LP spread
    in the sweep under Laplace jumps (memorylessness), but falls with h under
    the random walk (≈ σ_u²/h for h ≫ σ_u): its slope on h is negative with
    interval excluding 0. This is the signature that tells the two
    fundamentals apart in the race data.
  - **H3c** — for the paper's maker, the tape-band maker-gains count is
    strictly lower under Laplace jump-diffusion than under the random walk
    at matched variance. Reason: widening by Δh cuts Laplace sniping loss by
    the factor e^(−Δh/b), far less than the Gaussian tail it cuts under a
    random walk, so the widening defence that produces the inversion pays
    less.
- **Test.** Bootstrap ratio interval (H3a); per-snipe bootstrap and OLS
  slope with seed-clustered bootstrap (H3b); cell counts (H3c).
- **Miss.** H3a outside ±10%; H3b
  Laplace per-snipe profit varying with h (slope interval excluding 0) or
  random-walk slope ≥ 0; H3c count under jumps ≥ count under the random walk.

### P4. Cost of speed, free entry and measured waste

- **Hypothesis.** BCS Prop 3–4: with a cost of speed c per unit time and free
  entry, the arms-race prize is spent on speed (eq. 7: N*·c = λ_i·h*), and the
  spread, the prize and investors' cost are invariant to c (Prop 5(iii)).
- **Setup.** BCS LP at h* (spec geometry), all fast firms (LP included) pay
  c; firms enter one at a time while the next entrant's measured profit
  interval lies above c.
- **Predictions.**
  - **primary (H4a)** — entry stops at N* = floor(λ_i·h*/c) firms (eq. 7 with
    integer N, §VI.D). At the spec's ratio-1 cell (λ_i = λ_j = 10/s,
    b = 1000, h* = 567.1, rent 5,671 ticks/s) and c = rent/5.5 = 1,031
    ticks/s: N* = 5, the 5th firm earning +103 ticks/s and a 6th −86. Measured
    waste N*·c = 5,156 ticks/s = 0.909 of the rent, within ±5%.
  - **H4b** — across c ∈ {rent/3.5, rent/5.5, rent/10.5}, waste/rent is
    floor(R/c)·c/R = 0.857, 0.909, 0.952 (±0.03), while the LP's investor
    revenue and total sniping rent are invariant (TOST ±5%).
  - **H4c** — against the paper's maker (slower than every sniper), free
    entry dissipates at least (1 − c/R) of the rent, and the noise-trader
    toll is invariant to c (TOST ±10%).
- **Miss.** H4a: entry stopping at N ≠ 5 with the marginal-profit intervals
  excluding the prediction, or waste/rent outside 0.909 ± 0.05. H4b: rent or
  investor revenue moving by more than 5% across c. H4c: toll moving more
  than 10%.

### P5. Fee schedules: zero, taker-heavy, maker-rebate

- **Hypothesis (derived from BCS eqs. 1–3 with per-unit fees).** With taker
  fee f_t (snipers and investors) and maker fee f_m (negative = rebate), an
  indifference-setting LP's economics depend only on its net half-spread
  h′ = h − f_m and the total fee F = f_t + f_m. For Laplace jumps:
  λ_i·h′ = λ_j·e^(−(h′+F)/b)·[b + F(N − 1)/N]. Two consequences: the fee
  *split* is neutral, and a positive total fee brings back the
  N-dependence BCS's eq. 3 lacks.
- **Predictions.**
  - **primary (H5a)** — holding the LP's quotes fixed, a taker fee f_t scales
    snipes per jump by e^(−f_t/b) and leaves net profit per successful snipe
    at b, each within ±5% (memorylessness: only snipes worth more than the fee
    are fired, and they are worth b more on average).
  - **H5b** — with the LP's spread re-solved under each schedule by P1's
    procedure, the all-in investor half-spread h + f_t is equal across
    schedules with the same F (TOST ±3%); at λ_i = λ_j = 10/s, b = 1000 it is
    567.1, 648.8, 811.0 ticks for F = 0, 100, 300 at N = 2 and 567.1, 658.8,
    837.0 at N = 5 (±3%).
  - **H5c** — the paper's mechanical maker does not re-price, so neutrality
    fails for it: maker-rebate and taker-heavy schedules each give at least
    as many tape-band maker-gains cells as zero fees, and sniper rent falls
    by at least the factor e^(−f_t/b).
- **Miss.** H5a outside ±5%; H5b all-in spreads differing by more than 3%
  across splits at equal F, or missing the tabulated values by more than 3%;
  H5c fewer maker-gains cells under either fee schedule than under zero fees.

### P6. Heterogeneous noise traders with endogenous participation, plus a slow informed investor

- **Hypothesis.** BCS footnote 37: with elastic demand, sniping's wider
  spreads destroy gains from trade, not only transfer them. Separately,
  BCS §VI.D: information slower than the quote-setter's is worthless in a
  continuous book.
- **Setup.** Each investor draws a gain from trade v ~ Exp(mean v̄) and trades
  only if the all-in half-spread it faces is below v. The slow informed
  investor sees the signal with latency longer than the maker's requote
  latency and trades toward it.
- **Predictions.**
  - **primary (H6a)** — HFT presence lowers participation by the factor
    e^(−(h̄_HFT − h̄_0)/v̄), with h̄ the realised mean quoted half-spreads with
    and without HFTs (±10%), and creates a deadweight loss (forgone
    λ_i·E[v·1{h̄_0 < v < h̄_HFT}]) whose interval excludes zero.
  - **H6b** — the slow informed investor's realised edge per trade, measured
    against V at fill time, is not positive: upper 95% bound ≤ +0.05·h̄.
  - **H6c** — the single-maker inversion depends on the participation
    elasticity e = h·g(h)/(1 − G(h)) at the base spread: if the measured e ≤ 1,
    ≥ 15 of 18 tape-band cells stay maker-gains (consistent with the paper's
    17/18 at α = 1); if e ≥ 2, ≤ 9 of 18.
- **Miss.** H6a participation ratio outside ±10% or deadweight interval
  including zero; H6b lower bound of the edge above zero; H6c the count on
  the wrong side of its threshold.

### P7. Richer maker strategies

Avellaneda–Stoikov quoting, inventory skew, quote fading, multiple levels,
explicit cancels.

- **Predictions.**
  - **primary (H7a)** — a maker that cancels on every observed signal update
    loses, per snipe-able jump, the fraction 1 − `race_win_probability`(lag,
    ε, k) of what an otherwise identical non-cancelling maker loses
    (k/(k + 1) at lag 0, 1 at lag > ε), within ±0.03.
  - **H7b** — inventory skew δ shifts both quotes; by Jensen on the Laplace
    prize (*derived*) it raises expected sniping loss per jump by the factor
    E[cosh(δ/b)] over the realised skews relative to the unskewed maker at
    the same mean half-spread (1.005, 1.045, 1.128 at δ/b = 0.1, 0.3, 0.5):
    measured ratio within ±5% of that factor. Skew never reduces sniping loss.
  - **H7c** — on a multi-level ladder, per-unit sniping loss at level j
    relative to level 1 is e^(−(h_j − h_1)/b) (±15%); the BCS-consistent
    ladder widens with depth (Prop 2), and an equally spaced ladder
    over-supplies deep levels relative to it.
  - **H7d** — the paper's §5.2 conjecture, tested: an Avellaneda–Stoikov maker
    (inventory-risk spread, no adverse-fill response) shows no inversion,
    ≤ 2 of 18 tape-band maker-gains cells, and lower PnL with HFTs present
    than the adverse-selection maker at equal base spread (paired interval
    below zero).
  - **H7e** — quote fading reproduces the paper's leg pattern: revenue leg
    negative, sniping leg positive (both intervals excluding zero) in ≥ 6 of
    the 8 decomposed tape-band cells.
- **Miss.** H7a outside ±0.03; H7b ratio outside ±5% of E[cosh(δ/b)] or below
  1; H7c outside ±15%; H7d ≥ 6 maker-gains cells or A–S PnL not lower;
  H7e fewer than 6 of 8.

### P8. Endogenous makers

Best response over widening, decay, size and a no-quote option; Nash
iteration for M ≥ 2; a fixed cost of quoting with exit.

- **Predictions.**
  - **primary (H8a)** — the Nash quoted half-spread is non-increasing in M
    over {1, 2, 3, 5}, and at M = 5 it lies within 20% of the LP breakeven
    `half_spread(λ_i, λ_j·(N − 1)/N, J)` with N = k + 1. At M = 1 with
    inelastic demand the best response sits on the upper edge of the widening
    grid: the single maker's gain is monopoly pricing that sniping licenses,
    not a defence.
  - **H8b** — with a fixed cost of quoting F_q, the number of active makers
    falls in F_q and in k, and each active maker's equilibrium PnL interval
    contains a value in [F_q, F_q + the one-entrant profit increment].
  - **H8c** — the within-class split survives but its order becomes
    monotone in latency: the slowest maker has the lowest PnL delta in
    ≥ 90% of tape-band cells (the paper's spacing-dependent winner is
    predicted to be an artefact of the imposed rule).
  - **H8d** — a maker lagging every sniper by more than ε chooses no-quote,
    or rests behind the touch with < 5% of fills, in ≥ 2/3 of ratio-1 cells
    with k ≥ 3: it can never win a race, so quoting at the touch only buys
    sniping losses.
- **Miss.** H8a any M with a Nash spread significantly above that of M − 1,
  or M = 5 outside ±20%; H8c slowest maker not lowest in > 10% of cells;
  H8d fewer than 1/2 of those cells.

### P9. Batch-auction semantics with a tie-break sweep

- **Hypothesis.** BCS Prop 6–7: a uniform-price batch never lets a stale
  quote be sniped when the LP is as fast as the snipers; exposure only comes
  from jitter (footnote 34) or a slow LP (δ/τ). The engine's uncross differs
  from BCS's Walrasian clearing: it ranks candidate prices by volume, then
  imbalance, then distance to the reference (last clearing) price, then
  pressure. With one stale ask at A and ≥ 2 snipers bidding V′ > A, every
  candidate in [A, V′] ties on volume and imbalance, so the engine clears at
  A (nearest the old price) where BCS clear at V′ (Bertrand, Prop 6(iii)).
- **Predictions.**
  - **primary (H9a)** — for τ ∈ {1, 10, 100} ms with maker lag ≤ ε, the
    maker's sniping loss rate is below continuous trading's in every
    interval, and at or below the bound λ_j·(ε/(2τ))·prize(h) (BCS footnote
    34): upper interval bound ≤ 1.2 × bound.
  - **H9b** — with stale-price snipers the clearing price on exposure events
    is the stale quote under every tie-break order (zero difference across
    rules, deterministically). With price-competitive snipers (bidding
    V′ − 1 tick) and ≥ 2 in the batch, the reference-first cascade clears at
    the stale quote and pressure-first or Walrasian clearing at V′ − 1 tick.
    Aggregate maker PnL differs across tie-break rules by less than 1% of the
    continuous market's sniping loss whenever ε/τ ≤ 0.01.
  - **H9c** — no backfire: at no τ (including τ below the maker's latency)
    is the maker's loss under batching significantly greater than under
    continuous trading. With lag > τ the exposure caps at 1 and batching
    merely stops helping.
  - **H9d** — within-batch rationing (engine time priority vs random)
    changes which sniper is filled but not the maker's loss (TOST ±2%).
- **Miss.** H9a any interval with batch loss not below continuous, or above
  1.2 × the bound; H9b any rule difference on stale-price snipers, or
  > 1% aggregate difference; H9c any τ with batch loss significantly above
  continuous; H9d outside ±2%.

### P10. Held-out test: which maker pays

- **Hypothesis.** Who pays is predictable from latency rank once the race is
  modelled; the paper's spacing-dependent pattern is a tick-clock artefact.
- **Held-out configurations** (none among the paper's eight; ticks, 1 tick =
  100 ms on the event clock): M = 2: {1, 2}, {1, 3}, {3, 5}, {1, 7};
  M = 3: {1, 2, 3}, {1, 2, 6}, {1, 5, 6}, {3, 4, 7}, {2, 4, 8}. Tape-band
  cells, n = 100.
- **Predictor (fixed now).** "The slowest maker pays": in each cell, the
  maker with the most negative PnL delta is the one with the largest latency.
- **Predictions.**
  - **primary** — on the event model the rule is right in ≥ 80% of held-out
    cells, closely spaced sets ({1, 2}, {1, 3}, {1, 2, 3}) included, and the
    Spearman correlation of latency with PnL delta is negative in ≥ 80% of
    cells.
  - On the tick model (the paper's own), the rule is right in 60–95% of
    held-out cells overall (in-sample: 112/144 = 78%) but in < 50% of the
    closely spaced cells, generalising the paper's {2, 3, 4} reversal.
- **Test.** One-sided binomial against chance 1/M; the hit-rate bands above.
- **Miss.** Event model below 60%, or below 50% on the closely spaced sets;
  tick model overall hit rate below 50%, or closely spaced hit rate ≥ 70%.

### P11. Global parameter sensitivity

- **Method.** Sobol indices by Saltelli sampling, N_base = 512, over: maker
  adverse sensitivity and decay (×[0.25, 4], log-uniform), snipe/quote ratio
  [0.003, 1] log-uniform, k {1 … 21}, M {1 … 5}, λ_noise and λ_j (×[0.25, 4]),
  b (×[0.25, 4]), sniper latency level holding lag fixed (×[0.1, 10]), lag/ε
  [0, 2], c_speed [0, rent/3], fees F [0, 0.3b]. Outcomes: total sniper rent,
  maker PnL delta, noise-trader toll, gap rate. Index intervals by bootstrap.
- **Predictions.**
  - **primary (H11a)** — BCS Prop 5(iv)–(v): the first-order index of sniper
    latency level (lag fixed) is < 0.05 for total sniper rent, while λ_j and b
    together carry ≥ 0.5 of its variance. (c_speed enters only the waste
    outcome; its zero index on gross rent is mechanical and is not counted as
    a hit.)
  - **H11b** — for maker PnL delta, adverse sensitivity and snipe/quote ratio
    rank in the top three total-order indices, and interactions are strong:
    Σ total-order − Σ first-order ≥ 0.2 (the paper's "surface does not
    reduce to one dimension").
  - **H11c** — the noise-trader toll is nearly invariant to k: total-order
    index of k < 0.1 (the paper's factor-of-two toll across 63 cells).
- **Miss.** H11a latency-level index ≥ 0.1, or λ_j and b below 0.35 together;
  H11b either named parameter outside the top three, or interactions < 0.1;
  H11c index of k ≥ 0.2.
