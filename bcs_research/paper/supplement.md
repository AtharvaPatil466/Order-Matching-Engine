# Supplement to "Who Bears the Latency Tax?"

<div align="center">

**Atharva Patil**

</div>

This supplement holds material moved out of the main text after review. Section S1 collects results that were demoted, each with the reason. Section, table and figure labels prefixed "S" are internal to this supplement; unprefixed ones (§4.2, Table 1) point to the main paper.

## S1. Demoted results

### S1.1 Experiment 1: the positive rent and the 3×3 grid

*Why demoted.* At the operating point a positive HFT rent is positive by construction: a sniper fires only when the fundamental is further from the maker's stale quote than its transaction cost (zero here), and the maker's three-tick latency guarantees such gaps occur. A positive rent therefore checks the harness rather than testing a hypothesis about the market. The noise-trader share of the transfer, previously reported as an 18%/82% split against the maker, is dropped as a headline; the underlying deltas are in Table S1.

Within the conserved frame of §4.3, HFT entry extracts a rent of 34.70 [26.25, 44.80] per run. Maker PnL changes by −28.50 [−38.31, −20.35] relative to the matched no-HFT baseline (from 101.27 [93.28, 110.72] to 72.78 [71.05, 74.38]) and noise-trader PnL by −6.21 [−7.87, −4.61]. Liquidity gaps rise from 2.84 [2.47, 3.26] in the baseline to 9.70 [9.15, 10.27] in treatment, a delta of 6.86 [6.20, 7.52]; the baseline is not gap-free, reflecting quote churn with no HFT present. Lagged Kyle's λ, the price impact per unit of signed flow (Kyle, 1985), rises about sevenfold, from 0.200 [0.184, 0.216] to 1.372 [1.361, 1.383].

Varying the maker's adverse sensitivity and spread decay over {0.05, 0.1, 0.2} × {0.05, 0.1, 0.2} with $n=100$ seeds per cell, HFT rent is significantly positive and the zero-sum identity holds in all nine cells, with rent point estimates from 16.56 to 46.63 (`exp1_hardening.json`, `calibration`).

**Table S1.** Experiment 1 welfare and microstructure estimates at the operating point ($n=100$ seeds; point estimate with 95% bootstrap CI). PnL, rent and residual in simulation currency (§3.4).

| Quantity | Treatment | No-HFT baseline | Treatment − baseline |
|---|---|---|---|
| HFT rent | 34.70 [26.25, 44.80] | — | — |
| MM PnL | 72.78 [71.05, 74.38] | 101.27 [93.28, 110.72] | -28.50 [-38.31, -20.35] |
| NT PnL | -107.48 [-116.84, -99.26] | -101.27 [-110.72, -93.28] | -6.21 [-7.87, -4.61] |
| Zero-sum residual | 2.78e-12 [-3.34e-12, 9.11e-12] | -5.39e-12 [-2.16e-11, 1.05e-11] | — |
| Liquidity gaps (count per run) | 9.70 [9.15, 10.27] | 2.84 [2.47, 3.26] | 6.86 [6.20, 7.52] |
| Lagged Kyle's $\lambda$ | 1.372 [1.361, 1.383] | 0.200 [0.184, 0.216] | — |

### S1.2 Liquidity gaps grow with the number of snipers; the channel is unidentified

*Why demoted.* The review asked that the liquidity-gap channel be identified or the result deleted. It has not been identified, so the result is reported here and the main text keeps one sentence on it.

Liquidity gaps (§3.3) rise steadily with the number of HFTs $k$ at the operating point, from 2.84 [2.47, 3.26] at $k=0$ to 47.47 [46.22, 48.85] at $k=21$, about 16.7-fold. The growth is close to linear in $k$ (fit over $k \geq 1$: slope 2.13, intercept 3.74, $R^2=0.997$; `exp3_hft_count.json`). The maker's depth-thinning rule — its resting size shrinks as its spread widens — is the obvious mechanical explanation, and it is not the explanation: re-running the sweep with the rule off and quote size held constant ($n=100$, `exp3_hft_count_fixed_depth.json`) leaves the growth unchanged, gaps running from 3.61 at $k=1$ to 47.18 at $k=21$ with a fitted slope of 2.18 ($R^2=0.998$). Thinning adds about one and a half gaps at small $k$ and nothing to the slope. Nor is the growth a count of snipes: HFT fills stay flat (252 to 242 at fixed depth) while gaps rise thirteenfold, the book is sampled once per tick, and losing HFT orders are IOC and leave nothing behind. We report the growth as a regularity with an unexplained mechanism.

Gaps also move with the batch-clearing interval. At the operating point the per-tick auction produces about 137 gaps per run against 9.7 under continuous clearing (`exp4_batch_auction.json`). At calibrated parameters the batch arm is non-monotone in the interval, the per-tick auction being the worst cell on both rent elimination and gaps (Figure S1).

![Figure S1. Calibrated batch arm ($n=100$): percentage of continuous-clearing HFT rent eliminated (left axis) and liquidity gaps per run (right axis, count) against the batch clearing interval in ticks (log scale); the dotted line is the continuous arm's gap count (53).](../results/figures/fig6c_batch_nonmonotone.png)

### S1.3 Per-maker winners by latency configuration

*Why demoted.* These are cell counts over one grid. Neighbouring cells share seeds and differ by one step in HFT count or snipe ratio, so a count of cells is not a count of independent replications, and the review rejected it as evidence. The main text replaces the count with a held-out prediction test of who wins (§4.2, to be added). The table below is kept as a description of the grid.

Each maker's PnL delta is bootstrapped separately at the eighteen tape-band cells (ratios 0.0049 and 0.0097 × nine HFT counts), $n=100$, maximum residual $5.57\times10^{-10}$ (`exp7b_per_maker_pnl.json`, `exp7d_latency_permutation_values.json`). A verdict is "gains" or "pays" at 95% bootstrap confidence.

**Table S2.** Tape-band cells (of 18) in which each maker gains or pays, by latency configuration.

| makers' latencies (ticks) | fastest gains | middle gains | slowest pays |
|---|---|---|---|
| 0, 5 (zero-latency leader) | 18 | — | 18 |
| 0, 3, 5 (zero-latency leader) | 18 | 18 | 18 |
| 1, 5 (calibrated) | 14 | — | 15 |
| 2, 6 | 11 | — | 14 |
| 1, 4 | 15 | — | 17 |
| 1, 3, 5 (calibrated) | 4 | 18 | 13 |
| 1, 4, 7 | 6 | 18 | 14 |
| 2, 3, 4 | 6 | 3 | 3 |

With a zero-latency leader the ordering is monotone in latency. With latencies spaced as measured replenishment spaces them, it is not. At two makers the faster usually gains, but the order reverses at thirteen or more snipers. At three makers without a zero-latency leader the fastest never wins consistently: the middle maker gains in all eighteen cells under {1, 3, 5} and {1, 4, 7}, while under the closely spaced {2, 3, 4} the slowest gains most often, in 10 of 18, and the middle maker pays in 14. Neither speed rank nor a particular tick value predicts the winner — three ticks wins under {1, 3, 5} and mostly pays under {2, 3, 4}. Across the eight rows, 136 of the 144 cells have at least one maker gaining and one paying and 4 have every maker paying; the slowest maker pays in 112. (The results file's own `is_redistribution` flag, which also requires the aggregate not to be maker-gains, marks 126.)

### S1.4 The Hawkes endogeneity diagnostic: a placebo-validated null

*Why demoted.* A null at an operating point whose arrival processes are close to Poisson by construction says little about the mechanism, and the diagnostic is about the liquidity-gap channel demoted in S1.2.

The diagnostic asks whether the liquidity gaps are preceded by rising order-flow self-excitation — whether they are reflexive cascades rather than direct consequences of adverse-selection widening. We estimate the branching ratio of a self-exciting Hawkes process on simulated order flow (Filimonov and Sornette, 2012) in 200-tick windows labelled pre-gap or normal, on both the trade and quote-update processes, with a placebo arm whose gap labels carry no information. Each arm is 10 simulations of 50,000 ticks at the operating point, with a 50σ shock every 2,500 ticks (`exp5_redesigned.json`, `config`). The windowing, labelling, and placebo construction are in the replication materials.

**Table S3.** Hawkes branching ratios $n(t)$ in pre-gap and normal windows.

| Arm / process | pre mean $n(t)$ | normal mean $n(t)$ | $p$ (one-sided) | rank-biserial | $n_{\text{pre}}$ / $n_{\text{normal}}$ (windows) |
|---|---|---|---|---|---|
| Treatment / trades | 0.0162 | 0.0167 | 0.258 | 0.012 | 1,416 / 3,614 |
| Treatment / quotes | ≈0 | ≈0 | 0.937 | −0.029 | 1,260 / 3,206 |
| Placebo / trades | 0.0111 | 0.0168 | 0.895 | −0.038 | 373 / 8,614 |
| Placebo / quotes | ≈0 | ≈0 | 0.491 | 0.001 | 335 / 7,707 |

The result is a null, and the placebo shows the pipeline does not manufacture endogeneity where none exists. Pre-gap and normal branching ratios are indistinguishable on trade arrivals ($p=0.258$, effect size 0.012), and the quote-update process shows essentially no self-excitation anywhere, as expected when the maker's requotes track an exogenous random walk. At this operating point the gaps are not preceded by rising self-excitation. This is not evidence against reflexivity in real markets, where self-excitation is well documented: this market's arrival processes are close to Poisson by construction, with branching ≈0.017 on trades, some 50× below criticality, leaving little endogeneity to detect.

A real-data leg would run the same pipeline on the BTC tape. It needs one new component, a depth-depletion definition of a gap suited to a book that never empties (e.g., top-k depth falling below a percentile floor), after which the windowing, labelling, and placebo machinery apply unchanged.

## References (cited only in this supplement)

Filimonov, V., & Sornette, D. (2012). Quantifying Reflexivity in Financial Markets: Toward a Prediction of Flash Crashes. *Physical Review E*, 85(5), 056108.

Kyle, A. S. (1985). Continuous Auctions and Insider Trading. *Econometrica*, 53(6), 1315–1335.
