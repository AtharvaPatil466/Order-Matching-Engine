# Who Bears the Latency Tax? Incidence of the HFT Arms Race on a Model-Checked Limit Order Book

<div align="center">

**Atharva Patil**

Xavier Institute of Engineering

</div>

## Abstract

Budish, Cramton, and Shim show that the continuous limit order book's latency arms race imposes a tax; which participants bear it is a separate question, which we study on a C++ limit order book whose matching design is model-checked in TLA+ and whose welfare accounting closes to machine precision. At the snipe-to-depth ratio measured on 86 hours of BTCUSDT-perpetual data, the tax is redistributed within the liquidity-providing class — [TODO: headline number from the held-out prediction test] — and who pays is set by how the makers' latencies are spaced rather than by their speed rank. At the class level the answer depends on market structure: a single maker profits from the race, through smaller pick-off losses rather than higher noise revenue, while two or more competing makers restore the incidence Budish, Cramton, and Shim predict. A differential test of the engine against a Python re-implementation of its batch clearing rule found a tie-break divergence that had moved published batch-auction results.

## 1. Introduction

### 1.1 The arms-race argument

Budish, Cramton, and Shim (2015, hereafter BCS) argue that the continuous limit order book's serial processing creates a mechanical arbitrage on every public price move: whoever reacts first picks off liquidity providers' stale quotes before they can be revised. The resulting race over speed rather than price dissipates resources and taxes liquidity provision without improving price discovery, and BCS propose frequent batch auctions — discrete-time, uniform-price clearing — to turn competition on speed back into competition on price.

### 1.2 The question and the main result

This paper asks which participants bear the latency tax. The BCS argument establishes that the tax exists; it does not settle who pays it. We map incidence over the snipe-to-quote size ratio, the number of snipers, the number of competing makers and the elasticity of uninformed demand, on a limit order book whose matching design is model-checked in TLA+ (§3.1) and whose batch clearing rule has been tested differentially against a re-implementation (§5).

The main result concerns incidence within the liquidity-providing class. At the snipe-to-depth ratio measured on 86 complete book-and-trade hours of BTCUSDT-perpetual data (§3.7), competing makers split the tax: one typically gains while another pays, and who pays is set by how their latencies are spaced rather than by their speed rank (§4.2). [TODO: held-out prediction test]

At the class level the answer depends on structure (Figure 1). With a single maker the BCS prediction inverts at the measured ratio: the maker gains at every HFT count we test and noise traders bear the transfer, because widening cuts the maker's pick-off losses by more than it costs in noise revenue (§4.1). A second competing maker removes the inversion at all but the lowest sniper counts and a third removes it entirely, restoring the incidence BCS predict (§4.2). Supporting experiments, at a pre-registered operating point and at parameters fitted to the perpetual's order flow, are in §§4.3–4.4. Demoted results, robustness checks, the verification record and replication details are in the supplement.

### 1.3 Contributions

**First**, it treats the incidence of the latency tax as an empirical question distinct from its existence and maps it over market structure: under single-maker supply the BCS prediction inverts at the snipe-to-depth ratio measured on the tape, and competitive supply restores it (§§4.1–4.2). Neither BCS nor the message-level measurement of Aquilina, Budish, and O'Neill (2022) treats incidence as a function of market structure.

**Second**, it shows that incidence within the liquidity-providing class is redistributive, so the class-level question is not well posed without a weighting, and that who pays is set by how the makers' latencies are spaced rather than by speed rank (§4.2). [TODO: held-out prediction test]

**Third**, it runs the BCS mechanism on a matching engine whose design is model-checked, and uses differential testing against that engine as an internal-validity check. A Python re-implementation of the batch clearing rule, checked against the C++ source line by line, cleared a volume-tied auction at a different price the first time the two ran against the same book, and the difference moved published batch results (§5).

## 2. Related Literature

**The arms race and its measurement.** Budish, Cramton, and Shim (2015) treat latency competition as a consequence of the continuous book's serial processing rather than as conduct to be regulated, and propose frequent batch auctions as the remedy. Aquilina, Budish, and O'Neill (2022) measure the race directly from message-level data that record failed attempts: races occur about once a minute per FTSE 100 symbol, are decided by microseconds, and sum to a latency-arbitrage tax of about 0.4 basis points of volume. Neither paper asks which participants bear the tax. The closest evidence on that question is Shkilko and Sokolov (2020): when weather disrupts the microwave links that give the fastest traders their edge, adverse selection falls and trading costs decline, in a market with many competing liquidity suppliers. Menkveld (2016) surveys HFT's double role as liquidity supplier and source of adverse selection and notes that the net welfare sign for any participant class is unsettled. Menkveld (2013) and Brogaard, Hendershott, and Riordan (2014) place fast traders on the maker side of the book as well as the sniping side, which is why incidence within the liquidity-providing class is a separate question from incidence on the class.

**Speed and exchange design.** Menkveld and Zoican (2017) show that a faster exchange can worsen liquidity, because it speeds up snipers and makers alike and raises the frequency of speed duels. Baldauf and Mollner (2020) compare batch auctions with asymmetric speed bumps, and Du and Zhu (2017) derive the optimal batching frequency as a trade-off between information aggregation and delayed incorporation. Hoffmann (2014) shows that fast traders concentrate pick-off risk on slow limit-order submitters, who respond by quoting less aggressively, and Biais, Foucault, and Moinas (2015) show that equilibrium investment in speed exceeds the social optimum because the fast impose adverse selection on the slow. These models derive who is fast and what speed costs in equilibrium. We impose both the latencies and the maker's quoting rule, and ask how incidence varies with market structure; a configuration-dependent winner among makers, which we find, is what an equilibrium model would have to pin down.

**Adverse selection and the spread.** Copeland and Galai (1983) model the spread as a trade-off between revenue from uninformed traders and losses to informed ones, and Glosten and Milgrom (1985) make the informed-trading component explicit. Foucault, Kozhan, and Tham (2017) show empirically that arbitrages triggered by news, rather than by transient imbalance, are the ones that widen spreads. Because widening is at once a defence against the informed leg and a tax on the uninformed one, we report the maker's two legs separately (§4.1) rather than only their difference.

**Agent-based models.** The agent-based computational finance tradition (Farmer and Foley, 2009; LeBaron, 2006) argues that simulations of heterogeneous agents can produce out-of-equilibrium phenomena that representative-agent models cannot; its known weaknesses are ad hoc behavioural assumptions, weak calibration, and simulation machinery that is hard to verify. Wah and Wellman (2013) built an agent-based model of latency arbitrage across fragmented markets and found that the arbitrageur profits at other traders' expense while degrading allocative efficiency, the same qualitative transfer we decompose. The method is therefore not new. What this paper adds is the substrate and the question: the BCS mechanism runs on a matching engine whose design is model-checked and has been tested differentially against a re-implementation (§§3.1, 5), with a welfare identity that closes to machine precision, and the question asked of it is who bears the tax rather than whether it exists.

## 3. Model and Experimental Design

One C++ matching engine makes every trade. A Python layer above it supplies the clock, the agents, the fundamental and the accounting, and a pybind11 bridge between the two marks where the engine's formal guarantees end. This section describes the nanosecond event model on which every result in §4 is computed; §3.8 says why the 100 ms tick model of the paper's earlier version was replaced.

### 3.1 The engine, the bridge, and what the verification covers

All trades, in continuous matching and in batch uncrosses alike, are produced by one C++20 matching engine, reached from Python through a pybind11 bridge that is the agents' only channel to it. The engine runs synchronously, so the fills an order produces are drained immediately after it is submitted, and it has no notion of latency: the race lives entirely in the scheduler above it (§3.2). Its circuit breaker is disabled, so that the simulation rather than an exchange halt governs the dynamics. The bridge exposes order entry, cancels, top of book, depth snapshots, the session states and the call-auction `uncross`, and it reports every order the book refuses after admission, so that a refusal reaches the scheduler instead of leaving an agent believing a quote rests (Supplement S3.3).

The engine's matching design is model-checked in TLA+. At its deepest exhaustive configuration the continuous-matching specification explores 171,187,419 distinct reachable states with zero invariant violations, checking FIFO execution and conservation; the auction specification checks that every uncross clears at a single price, the path the batch arm runs through. The verification has two limits. TLC's guarantee is exhaustive only within small bounds — a handful of orders, two prices, short horizons — so it certifies the matching *design* at bounded scope rather than proving unbounded correctness. And the object verified is the specification, not the C++ source: no refinement proof links the two, and the correspondence rests on construction and conformance testing. Nothing above the bridge is verified. The scheduler, agents and accounting are checked by unit tests and, as a whole, by requiring the model to recover BCS's closed-form equilibrium before it is used for anything else (P1, §4.1). Supplement S3 lists the specifications, invariants and configurations; §5 reports a differential test of the engine's clearing rule.

### 3.2 The scheduler and its latency semantics

The scheduler is a discrete-event loop on an integer-nanosecond clock (`simulation/event_scheduler.py`, implementing the contract in `simulation/contracts.py`). Every event — an agent observing a public update, an order or cancel reaching the engine, a fill reported back, a self-scheduled wakeup — carries its own timestamp. Each agent has an observation latency and an order latency, and the scheduler applies them as follows.

- **Observation.** An agent observes a public update at the update's time plus its observation latency, and its own fills at the execution time plus the same latency.
- **Orders and jitter.** Orders and cancels returned from a callback at time $t$ reach the engine at $t$ plus the agent's order latency plus a race jitter drawn uniformly from $\{0,\dots,\varepsilon\}$ ns. Each agent draws its jitter from its own seeded stream, so adding or removing an agent — the treatment in a matched pair of runs — leaves every other agent's draws unchanged.
- **Bursts and per-agent FIFO.** One callback's messages travel as one burst: they arrive together and apply in the order returned, so a maker's requote cannot overtake, and trade against, its own stale quote. Successive bursts from one agent arrive in send order however wide its jitter; a burst that would overtake or tie an earlier one is moved to one nanosecond after it.
- **Ties.** Events due at the same nanosecond run in a seeded random permutation, never in registration order. Events created at an instant with zero delay form a fresh batch at that instant, run after everything already due, so a zero-latency agent cannot act on news before every other agent has had the chance to observe it.
- **Rejects.** When the engine refuses a message — an order at admission or after it, or a cancel of an order already gone — the sender learns of it its observation latency after the refusal. A post-only quote that would cross never rests, and an agent that could not learn this would keep it in the book it believes it has.
- **Book reads.** Agents read the engine's current top of book. Book-data latency is not modelled; the race is driven by the latency of the public signal.

For every jump the scheduler logs the engine arrivals that agents' reactions to that jump produced, in engine order, each with its outcome (filled, missed, cancelled, too late). Whether the maker's cancel beat the first snipe is a lookup in that log, and race-win rates are counts over it. Top-of-book snapshots are taken every 100 ms at the operating point, the tape's book cadence.

### 3.3 The cancel-versus-snipe race

A jump makes the maker's resting quotes stale. The maker, on observing it, sends one burst that cancels them and posts new quotes; each sniper, on observing it, sends one immediate-or-cancel (IOC) order at the stale price if the quote is stale by more than its cost. The engine applies the messages in arrival order. If the maker's cancel arrives first, every snipe finds nothing at that price and dies, and because a sniper never sends a market order a losing snipe cannot lift the maker's new quote. If a snipe arrives first, it fills against the stale quote. There is no race-specific code: the outcome is set by latencies and jitter draws alone.

With every racing message drawing an independent $U[0,\varepsilon]$ jitter on top of its fixed latency, a maker whose total latency exceeds the snipers' by $\ell$ beats all $k$ snipers with probability
$$
P_{\text{win}}(\ell,\varepsilon,k) = \frac{1}{k+1}\left(\frac{\varepsilon-\ell}{\varepsilon}\right)^{k+1} \quad (0\le \ell\le\varepsilon), \qquad 0 \quad (\ell>\varepsilon),
$$
derived in `analysis/bcs_theory.py`. At $\ell = 0$ it is BCS's $1/N$ with $N = k+1$ racing firms. P2 tests the formula directly and P7 tests the loss it implies (§4.3, §4.4). In the tick model a maker and a sniper reacting to the same news acted in the same 100 ms tick, and the race between them was a jitter draw inside it (§3.8).

### 3.4 The fundamental

The fundamental $V(t)$ is a jump-diffusion published as discrete updates (`agents/fundamental_paths.py`). A Brownian diffusion moves on a fixed grid, every 10 ms at the operating point, and compound-Poisson jumps arrive at rate $\lambda_j$ anywhere in integer-nanosecond time with signed Laplace$(0,b)$ sizes, so the absolute jump $J$ is exponential with mean $b$. Every change in $V$ is a public update, and jump updates are flagged for the race log. The path is generated whole and seed-deterministically before a run, the diffusion drawn first so that runs differing only in jump settings share it. Parameters enter in physical units — diffusive volatility in bp/$\sqrt{\text{s}}$ with jumps excluded, jump rate per second, $b$ in bp — and are converted once as fractions of the starting value $v_0$. Steps are arithmetic in price, which is accurate while moves stay small relative to $v_0$, as they do over a 30-minute run. Agents trade only on the updates they observe; the true $V$ is read by the accounting, by the maker's adverse-fill test and by the slow informed investor (§3.5).

Laplace jumps are the default because BCS's equilibrium has a closed form for them. The per-jump sniping prize at half-spread $h$ is $b\,e^{-h/b}$, and BCS's equation (3), $\lambda_i h = \lambda_j\,b\,e^{-h/b}$ with $\lambda_i$ the investor arrival rate, gives the equilibrium half-spread $h^* = b\,W(\lambda_j/\lambda_i)$, $W$ the Lambert function (derived in `analysis/bcs_theory.py`). A correlated-signal variant is implemented for news that arrives through a lead instrument: the lead path $L$ publishes, the traded asset is worth $V = v_0 + \beta(L - v_0) + I(t)$ with $I$ an idiosyncratic Gaussian walk, and updates carry $E[V\mid L]$ while $V$ itself is never published. It is unit-tested, and no pre-registered experiment uses it.

### 3.5 Agents

Every agent implements one interface — callbacks on start, on an observed update, on a wakeup and on a fill, each returning orders, cancels and wakeups — and nothing else connects it to the scheduler.

**Makers.** One class executes any maker strategy given as a `MakerPolicy` (`agents/strategic_maker.py`), a small frozen value that a best-response search can perturb, compare and cache (P8). Its dimensions are: whether to quote at all, the no-quote option that gives entry and exit a margin; the base half-spread and size; a ladder of levels behind the touch; Avellaneda–Stoikov (2008) quoting with risk aversion $\gamma$ over a rolling horizon, with the order-arrival decay tied to the base half-spread so that $\gamma = 0$ is fixed-spread quoting; linear inventory skew and an inventory limit; adverse-selection widening; quote fading after a jump or an adverse fill; a requote threshold; requoting at once after a fill; and post-only quotes, which the engine refuses rather than lets cross. The default policy is BCS's liquidity provider: a fixed half-spread around the last update observed, cancelled and requoted in one burst on every update. The widening dimension carries the earlier version's maker onto the event clock: a fill adverse against the true $V$ at execution multiplies the half-spread by one plus a sensitivity, up to a cap; the multiple decays toward base on every observed update; and resting size can thin by the same factor. The true $V$ enters only that adverse-fill test.

**Snipers.** A sniper reacts only to news (`agents/event_sniper.py`). On observing an update it compares the new value with the current top of book and, if a resting quote is stale by more than its threshold, sends one IOC at that stale price for its snipe size; otherwise it does nothing. Its threshold is its marginal cost, the taker fee of the run's schedule.

**Investors.** Uninformed flow is a Poisson population of reservation-value investors (`agents/event_noise.py`). Arrival $i$ values the asset at the last update it observed plus a private shock $\eta_i \sim N(0,s_\eta^2)$; it buys at the ask if that value is at least the ask, sells at the bid if it is at most the bid, and otherwise leaves. Participation therefore falls as the spread widens without an elasticity parameter: with the signal at the mid and half-spread $h$, an arrival crosses with probability $2\Phi(-h/s_\eta)$, and an infinite $s_\eta$ recovers the tick model's fixed-probability crossers. Sizes are lognormal.

**The slow informed investor.** A long-horizon investor reads the true $V$ with a lag at fixed decision times and, when its forecast differs from the mid, works a parent order as a series of IOC children at the touch. It is the one agent that reads $V$ directly, because learning $V$ late is what makes it informed. It is not part of the operating point; P6 adds it, reading $V$ one second late, to test BCS's claim that information slower than the quote-setter's is worthless in a continuous book (§4.4).

### 3.6 Accounting: fees, rebates, the cost of speed, and two identities

Welfare is one fold over the engine's trade list (`metrics/accounting.py`), so it cannot drift from what executed. Every fill pays the maker or the taker fee in basis points of notional; the maker is the non-aggressor, and a negative maker fee is a rebate. Fees are transfers to the exchange, which gives two identities that hold to rounding in every run: gross trading P&L sums to zero across participants at any common mark, and net P&L plus exchange revenue sums to zero. Speed is a third leg. A participant can be charged for its latency, and that money leaves the market for fibre and towers; it is the resource cost BCS call waste, so it is reported beside the identities and never inside them (P4). Open inventory is marked twice, at the last two-sided mid and at the true $V$ at the end of the run, and rents are read at $V$. Where a prediction concerns profit per event, fills are scored instead as edge against $V$ at execution, BCS's payoff, which has the same mean without the terminal mark's inventory noise. The operating point charges makers nothing and takers 1.7 bp, Binance USDⓈ-M's lowest public taker tier (VIP 9); P5 adds a zero schedule, a taker-heavy one at the regular tier's 2 bp maker and 5 bp taker, and a stylised maker-rebate one at −1 and 3 bp.

### 3.7 Frequent batch auctions on the engine's uncross

The batch arm (`simulation/event_batch.py`) keeps the scheduler's clock, latencies, jitter, FIFO and tie rule, and replaces matching on arrival with BCS's frequent batch auction (BCS §VII). Interval $k$ is the half-open window $[k\tau, (k+1)\tau)$, and its uncross runs at $(k+1)\tau$ before any arrival at that nanosecond, so an arrival on a boundary belongs to the next batch. Cancels take effect on arrival, so a cancel received during an interval is honoured before that interval's uncross. New orders are held until the boundary, handed to the engine in arrival order — or, in a rationing arm, in a seeded shuffle — and cleared by the engine's own `uncross`; orders resting from earlier batches keep priority. An IOC means "this auction only": it enters the uncross as a limit and its remainder is cancelled immediately after, so it never rests. The book is sealed: agents and snapshots see it as it stood after the last uncross. The engine stamps every uncross trade with a buy aggressor, so for fees the scheduler makes the later arrival the taker and an order resting from an earlier batch the maker.

The engine's clearing rule is not BCS's. It ranks candidate prices by executable volume, then imbalance, then distance to a reference price, then market pressure. The reference is the session's first limit order, and `uncross` never updates it, so volume-and-imbalance ties resolve toward where the session opened rather than toward the last print (verified in `tests/test_event_batch.py`). With one stale ask at $A$ and two or more snipers bidding $V' > A$, every price in $[A, V']$ ties on volume and imbalance, and the engine clears at the stale price $A$ whenever its reference lies nearer $A$; BCS's Bertrand argument (Prop. 6(iii)) clears at $V'$. A second price rule, "pressure", removes the reference step: the engine still decides who trades and how much, and the scheduler moves the price to the reference-free cascade's only when that price clears the same volume and lies inside every filled order's limit, so the fills are exactly those at the new price; otherwise the engine's price stands and the refusal is counted. P9 runs both rules and reads its verdicts on the engine as it is (§4.5). The theory supplies the exposure: a liquidity provider slower than the snipers by $\delta$ is exposed on a share $\delta/\tau$ of jumps (Prop. 6(i)); a fast one only through jitter, on a share $\varepsilon/(2\tau)$ (footnote 34), which we refine to $(\varepsilon/\tau)(N-1)/(2(N+1))$ by requiring a sniper to make the batch the provider missed.

### 3.8 The earlier tick model

The paper's earlier version ran on a tick model: every agent acted on a 100 ms grid, the fundamental was a Gaussian random walk stepped each tick, the maker observed it three ticks late, snipers reacted within the tick, and noise traders crossed with a fixed probability per tick. It was replaced because its clock could not represent the race. A maker and a sniper reacting to the same news acted in the same tick, and the race between them was a jitter draw inside it; a random walk has no jumps, which is where BCS's rents come from (P3); the growth of liquidity gaps with the number of snipers, and the decline of rent with it, came from where the end-of-tick snapshot fell relative to the snipes; and its batch arm entered snipers' IOCs as limits at the stale price, which let them outbid the maker for noise flow inside a batch. Its results are not carried over. The claims the pre-registration re-tests appear in §4 as predictions; Supplement S5 describes the model, the retired results and the reason for each.

### 3.6 Calibration to BTC order flow

To discipline the operating point against real microstructure, we calibrate the simulation's *environment* parameters to Binance BTCUSDT-perpetual order flow recorded by a companion collector (20-level book snapshots at about 10 per second and the aggregated-trade stream, both with nanosecond timestamps). The collector drops a few hours on most days, so each moment is estimated gap-safe rather than by discarding gapped days (Supplement S2.1). The sample is 27 days (2026-06-24 to 2026-07-21, less 2026-07-19; 37.0M trades, 4.16M snapshots, 2.27M seconds of coverage), yielding: trade arrival 16.28/s; trade sizes p50/p90/p99 of 0.003/0.177/2.01 BTC; median spread of one exchange tick (≈0.016 bp); median top-of-book depth 6.18 BTC; and 1-second realized volatility of 0.650 bp/√s.

We fix 1 tick = 100 ms of wall clock, so the maker's 3-tick latency is 300 ms of quote staleness while HFTs react within a tick. Under that convention arrival intensity and volatility invert to a per-tick arrival probability of 0.427 and $\sigma$ = 20.6, against the pre-registered operating point's 0.15 and 20, and both fits verify by simulation (Supplement S2.1). The operating point is therefore close to the data on volatility and thinner than the perp in order flow, by a factor of 2.8 per trader, which is why §4.4 re-runs the experiments at the fitted values.

One agent parameter had to move with them. The maker widens per adverse fill, so its widening is not scale-free in order flow: at calibrated arrival the pre-registered adverse sensitivity of 0.1 drives the *no-HFT* spread to 5.36× base, a control-loop artifact rather than an arms-race effect. We set it to 0.015 for the calibrated runs, holding spread decay at its pre-registered 0.1, on the same selection criterion as the original (derivation in Supplement S2.1). The snipe size is set to the calibrated quote size (2,061 units), so that a single snipe still clears the top quote; the 50-unit arm is the starting point of §4.1.

The calibration does not fit spread and depth levels (the maker quotes about 1.5 bp where the perp trades at about 0.016 bp), nor, in the experiments of §4, the size distribution (constants, against heavy-tailed real sizes). Nor can it fit the latency and race structure, which no public crypto feed reveals: measuring races takes failed-order message data of the kind Aquilina, Budish, and O'Neill (2022) obtained from a regulator. HFT latency and the race therefore remain swept design parameters. The *environment* the agents trade in is disciplined by data; the strategic structure is designed, not estimated.

### 3.7 Measuring the snipe-to-depth ratio on the tape

§4.1 sweeps the snipe-to-quote size ratio, and where the real market sits on that axis is what turns the surface from a parameter study into a claim. No public feed reports the quantity, so we measure it.

The sample is 86 complete book-and-trade hours of BTCUSDT-perp: hours with at least 30,000 of a nominal 36,000 snapshot rows and a matching trade file. Each trade is divided by the top-of-book depth it met, read from the snapshot immediately preceding it, and Binance's per-price-level aggregated-trade records are regrouped into single taker orders so that multi-level sweeps, the race flow of interest, do not fragment and bias the ratio downward. That yields 2.27 million taker orders.

Nothing in a public tape flags a snipe, so we do not label them: we report the distribution unconditionally and under a *race proxy* — an order arriving within 0.2 s of a snapshot on which the top of book moved by at least one price increment — and treat the gap between the two as the informative object. We condition on a price move rather than on order size because a move is what the BCS mechanism says triggers a race. A placebo on non-move orders in the same window measures the dilution that 100 ms snapshots impose on a microsecond phenomenon: its median is 0.0057 against the proxy's 0.0283, a factor of 5.0, so we treat the proxy as a bound rather than a measurement.

**Table 1.** Snipe-to-depth ratio on the BTCUSDT-perp tape: taker-order size divided by the top-of-book depth it met (dimensionless), 86 hours.

| sample | $n$ (taker orders) | p10 | p50 | p90 | p99 |
|---|---|---|---|---|---|
| unconditional | 2,269,256 | 0.00027 | **0.0072** | 0.393 | 18.0 |
| race proxy | 390,540 | 0.00051 | **0.0283** | 4.24 | 290 |
| placebo (no move) | 1,849,504 | 0.00024 | 0.0057 | 0.208 | 2.18 |

The distribution is extremely right-skewed, which is why the surface sweeps as far as ratio 1.0. Its stability on a longer 105-hour sample, the alignment rules and the de-fragmentation corrections are in Supplement S2.2.

## 4. Results

We report the incidence surface first (§4.1) and its extension to competing makers and elastic demand (§4.2), then the four supporting experiments on which the incidence claim rests (§4.3) and their re-run at calibrated parameters, which supplies magnitudes (§4.4). The zero-sum residual closes to floating-point noise on every run (§3.4).

### 4.1 The incidence surface: who bears the latency tax

**Incidence is not pinned by the data.** The calibrated main arm sets the snipe size equal to the quote size, preserving the operating point's ratio of 1.0 so a snipe clears the top quote. Holding the snipe at 50 units instead — 2.4% of the book — changes not the size of the effect but its direction: total rent rises in $k$ rather than staying flat, per-HFT rent flattens instead of decaying as $1/k$, and the maker's PnL delta is *positive* for $k\leq5$, turning negative only at $k\geq8$, with noise traders bearing the loss throughout. The claim that rent is extracted from the market maker is therefore conditional on the snipe-to-quote size ratio. Two ratio points do not locate a boundary in a two-dimensional space, which is what this section maps. We sweep snipe size against HFT count on the calibrated environment of §3.6, seven snipe quantities × nine HFT counts at n=100 seeds (6,300 treatment runs plus a shared 100-seed no-HFT baseline: 6,400 engine runs), holding the maker count at one.

The surface reproduces the published calibrated cells exactly where the two grids coincide, and the zero-sum residual is machine zero in all 63 cells (Supplement S2.3).

**The boundary.** Incidence is classified per cell by the sign of the maker's PnL delta against the shared no-HFT baseline, at 95% bootstrap confidence; a cell whose interval brackets zero is reported as indeterminate rather than assigned to whichever side its point estimate falls on.

**Table 2.** Incidence boundary under single-maker supply (calibrated environment, $n=100$). The maker's PnL delta against the no-HFT baseline is in simulation currency (§3.4).

| Snipe/quote ratio | Maker gains up to | Flips at | Maker delta at k=21 |
|---|---|---|---|
| 0.0049 | all k ≤ 21 | — | +2,154 (still gains) |
| 0.0097 | all k ≤ 21 | — | +1,425 (still gains) |
| 0.0243 | k = 6 | k = 7 | −5,213 |
| 0.0485 | k = 2 | k = 3 | −17,203 |
| 0.1213 | k = 1 | k = 2 | −39,006 |
| 0.3396 | none | below k=1 | −61,978 |
| 1.0000 | none | below k=1 | −72,332 |

At the race proxy reading of 0.0283, the flip occurs at k=7. If the real market operates with more than seven competing snipers at that ratio, the inversion does not hold under the race proxy.

The critical count falls monotonically as the snipe grows, from beyond the grid at the smallest ratios to below one at the largest — a collapse spanning the full range of k over roughly one order of magnitude in ratio. At the two smallest ratios no flip occurs anywhere on the grid: the maker's gain does not merely survive competition, it *increases* in k (from +1,061 at k=1 to +2,154 at k=21 at ratio 0.0049). At the two largest the maker loses even to a single sniper. The two calibrated snipe sizes above were one slice each of this surface.

**Where the real tape sits, and what it does to the BCS incidence prediction.** The ratio is a design parameter in the sense that no feed reports it directly, but §3.7 bounds it: measured per trade against contemporaneous top-of-book depth on 86 complete book-and-trade hours of BTCUSDT-perp, it is 0.0072 at the median unconditionally and 0.0283 under the race proxy, with the placebo quantifying the dilution. The unconditional median falls between the two smallest ratios on the grid — and that is the region in which the maker gains at every HFT count we tested, while noise traders bear the entire transfer. **In the regime the tape supports, under single-maker liquidity supply, the BCS prediction that the latency tax falls on liquidity providers inverts.** §4.2 adds the maker-count axis, and the inversion does not survive a second maker. The maker widens in response to adverse fills, and that widening leaves it better off on net — by cutting what the snipers take from its stale quotes rather than by earning more from noise flow, which in fact falls (see the leg decomposition below); the loss lands on the noise traders who cross the wider spread. The race-proxy median of 0.0283 sits just above the 0.0243 row, where the flip occurs at k=7 — so under the more conservative conditioning the maker still gains against a handful of snipers and loses only once the field grows. Both readings place real BTC at or below the boundary rather than deep in the maker-pays regime that §4.3 reports, and the maker-pays regime is reached only at snipe sizes 35–140× the measured median. One qualification attaches to the measurement: the tape under-measures multi-level sweeps, because Binance aggregates fills per taker order per price level, so the true race-order ratio is somewhere above the proxy, and the boundary matters more than any single point on it.

![Figure 1. Incidence boundary under single-maker supply (calibrated environment, $n=100$). Horizontal axis: snipe-to-quote size ratio (dimensionless, log scale); vertical axis: the first HFT count $k$ at which the maker no longer gains at 95% confidence. Open markers with arrows are censored: no flip occurs on the swept grid ($k \leq 21$), or the maker already loses at $k=1$. The shaded vertical band is the ratio measured on the BTCUSDT-perp tape, from the unconditional median (0.0072) to the race-proxy median (0.0283); its upper edge lies past the 0.0243 flip at $k=7$.](../results/figures/fig7_incidence_boundary.png)

The boundary is plotted with its censoring made explicit, because only three of the seven ratios flip inside the grid: at 0.0049 and 0.0097 the maker gains at every count up to twenty-one, so the critical count is bounded below by 21 rather than measured, and at 0.3396 and 1.0000 the maker already loses to a single sniper, so it is bounded above by 1. Drawing a curve through all seven would fabricate four of them. The figure's substantive content is the position of the tape band relative to the boundary: the unconditional median lies within the maker-gains region at every count we swept, while the race-proxy end of the band lies just past the 0.0243 row, where the maker gains only up to $k=6$. For the BCS incidence prediction to hold on this market the measured ratio would have to sit above the boundary curve rather than below it.

Across the 63 cells the noise traders' delta varies by a factor of two while the maker's changes sign, so the maker's exposure is essentially the whole source of variation on the surface; under inelastic demand that near-constant toll is what the model allows. Incidence also does not reduce to the product of snipe ratio and HFT count. Both checks are in Supplement S2.3.

**Decomposing the inversion into its two legs.** The account given above — that widening leaves the maker better off by cutting what snipers take from its stale quotes while its noise-flow revenue falls — is a claim about two quantities, and the surface reports only their difference. We therefore measured the legs separately at the tape-band cells ($n=100$, single maker as in the rest of this section). Every maker fill is scored as edge against the fundamental at fill time, positive when the maker captured value, and partitioned by the *same* adverse test that triggers the widening (§3.2), so the decomposition is measured with the mechanism's own discriminator rather than an external proxy. Because maker PnL is mark-to-market while the legs are realized edge at fill time, inventory carry is reported as an explicit third column rather than absorbed into either leg; the three sum to the net delta exactly, and the net column reproduces the surface's $\Delta$MM (largest reconciliation error $3.6\times10^{-12}$).

| ratio | $k$ | revenue leg | sniping leg | inventory carry | net $\Delta$MM |
|---|---|---|---|---|---|
| 0.0049 | 1 | −2,949 | +2,289 | +1,721 | **+1,061** |
| 0.0049 | 3 | −5,481 | +4,787 | +2,282 | **+1,588** |
| 0.0049 | 7 | −6,487 | +6,040 | +2,360 | **+1,913** |
| 0.0049 | 21 | −6,747 | +6,526 | +2,374 | **+2,154** |
| 0.0097 | 1 | −4,329 | +3,512 | +2,062 | **+1,244** |
| 0.0097 | 3 | −6,249 | +5,448 | +2,343 | **+1,542** |
| 0.0097 | 7 | −6,852 | +6,041 | +2,369 | **+1,558** |
| 0.0097 | 21 | −6,529 | +5,566 | +2,389 | **+1,425** |

**Table 3.** The maker's PnL delta at the tape-band cells split into its legs: seed-matched deltas against the shared no-HFT baseline, $n=100$, in simulation currency.

The revenue leg is **negative** in all eight cells: with snipers present the maker earns *less* from non-adverse flow than it does in the no-HFT baseline, because widening drives away more noise volume than it recovers in edge per fill. The sniping leg is **positive**, meaning the maker's losses to adverse fills *shrink* relative to baseline — the wider quote is picked off less profitably, and the baseline itself carries adverse selection from noise traders hitting stale quotes when the fundamental moves. The net gain is therefore not "more noise revenue than sniping loss." It is reduced adverse-selection cost, plus a substantial inventory-carry term, together outweighing a fall in noise revenue.

Two consequences follow. The net column is the surface's own $\Delta$MM and is positive throughout, and the intuitive story in which widening earns the maker more from noise flow is the wrong way round on the revenue leg. And the inventory-carry column is large enough (roughly a third to a half of the gross legs' difference) that a decomposition omitting it would misattribute the source of the gain; the maker's benefit is partly that it holds inventory through a favourable mark, which is a distinct channel from either leg and one the widening story does not describe at all. Which of the three channels dominates under elastic demand is not yet measured.

The claim holds for all HFT counts tested — up to twenty-one. Behavior at higher counts is not measured.

### 4.2 Maker competition and elastic demand

§4.1 holds two things fixed: liquidity is supplied by a single maker, and uninformed demand is inelastic. This section relaxes each in turn. The maker-count axis reverses the inversion at the class level; elasticity does not move it. Both extensions reproduce §4.1 exactly at their inelastic, single-maker corner.

**Maker competition.** We re-run the 63-cell surface at maker counts $M \in \{1,2,3,5\}$ on the calibrated environment, holding every §3.6 parameter fixed, at $n=100$ (441 cells, 44,800 engine runs). Aggregate top-of-book depth is held at the calibrated quote size and divided across the makers, so adding makers varies competition and latency spread rather than the depth a snipe meets; latencies for $M>1$ are §4.4's set, anchored on measured replenishment quantiles. A zero-latency-leader variant, placing the leading maker at zero latency, is run as an optimistic bound. The $M=1$ column reproduces §4.1 to **0.0 absolute difference** on five metrics at three probe cells re-run at $n=100$, and conservation holds across all 441 cells at a maximum absolute residual of $5.57\times10^{-10}$.

**Table 4.** The incidence surface by maker count $M$ ($n=100$). Rent is total HFT rent in simulation currency.

| $M$ | latencies (ticks) | ratio 0.0049 | ratio 0.0097 | flip @ 0.0243 | rent at $k=3$, ratio 1.0 |
|---|---|---|---|---|---|
| **1** | 3 | **gains at all $k\leq21$** | **gains at all $k\leq21$** | $k=7$ | 84,375 |
| 2 | 1, 5 | gains at $k\leq2$ | gains only at $k=1$ | none | 22,645 |
| 3 | 1, 3, 5 | no gains at any $k$ | no gains at any $k$ | none | 9,575 |
| 5 | 1–5 | no gains at any $k$ | no gains at any $k$ | none | 2,499 |

The inversion all but fails at $M=2$, the smallest competitive case, and fails completely at $M=3$. Of eighteen tape-band cells, eighteen are maker-gains at $M=1$, three at $M=2$ — only at the lowest HFT counts, $k\leq2$ at ratio 0.0049 and $k=1$ at 0.0097 — and none at $M=3$ or $M=5$; the zero-latency-leader arms show none at any $M>1$. This is a sign change carried by the intervals rather than a loss of power: at $k=3$, ratio 0.0097, the maker's delta runs $+1{,}542$ $[1{,}278, 1{,}844]$ at $M=1$, $-741$ $[-995, -477]$ at $M=2$, $-613$ $[-694, -533]$ at $M=3$, $-189$ $[-306, -54]$ at $M=5$. The noise-trader toll shrinks in step, from $-2{,}759$ under a single maker to $-205$ at five, so competitive supply reinstates the incidence BCS predicts rather than merely cancelling the inversion. Two channels drive it and both are real: splitting aggregate depth multiplies each maker's own snipe-to-quote ratio by $M$ — the aggregation problem of §6.2 measured rather than argued — and, at matched per-maker ratio, competition still moves the boundary (at per-maker 0.0243 the single maker gains through $k=6$ while $M=5$ already pays from $k=5$).

**The aggregate conceals a within-class redistribution.** A negative summed delta is consistent with every maker losing or with some gaining while others fund them. We therefore bootstrap each maker's delta separately at the tape-band cells, at $n=100$, across eight latency configurations at two and three makers (maximum residual $5.57\times10^{-10}$), each verdict at 95% bootstrap confidence. With a zero-latency leader the ordering is monotone in latency: at $M=2$ the leading maker gains $+2{,}284$ $[2{,}096, 2{,}477]$ at $k=3$, ratio 0.0097, while the five-tick maker pays $-3{,}475$ $[-3{,}656, -3{,}303]$. Without one it is not, and who gains depends on how the latencies are spaced rather than on speed rank: under {1, 3, 5} the middle maker gains, and under the closely spaced {2, 3, 4} the slowest gains most often. Per-configuration cell counts are in Supplement S1.3; they describe one grid and are not independent replications, since neighbouring cells share seeds. The evidence this claim needs is a held-out test of whether the latency configuration predicts who wins. [TODO: held-out prediction test]

Agent order is ruled out as the cause. Re-running the calibrated sets with the latencies assigned to the maker slots in every order leaves every verdict unchanged: at three makers, where depth splits evenly, the per-maker results are identical to the bit across all six orders, and at two they differ by at most 276, through the one extra unit the first slot quotes. The identity orders reproduce the published per-maker run to 0.0 on all ninety per-maker means. §4.4's monotone-in-latency ordering was measured at ratio 1.0 and does not extend to the tape band, and what sets the winner there we have not identified.

**Elastic demand.** The second fixed assumption is that noise traders cross at a fixed per-tick probability whatever the maker quotes. We set $\lambda_{\text{eff}} = \lambda (s_{\text{base}}/s)^{\alpha}$ with $s$ read off the book, so arrivals match §3.6's fitted intensity at base spread for every $\alpha$ and only the response to widening changes, and re-run the full surface at $\alpha \in \{0, 0.5, 1.0\}$ (189 cells, $n=20$). The $\alpha=0$ path reproduces §4.1 exactly at three $n=100$ probe cells.

**Table 5.** The single-maker surface under elastic noise-trader demand ($n=20$). Rent in simulation currency; baseline trades are trades per no-HFT run.

| $\alpha$ | ratio 0.0049 | ratio 0.0097 | flip @ 0.0243 | rent at $k=3$, ratio 1.0 | baseline trades |
|---|---|---|---|---|---|
| **0.0** (inelastic) | gains at all $k\leq21$ | gains at all $k\leq21$ | $k=6$ | 85,638 | 5,165 |
| 0.5 (mild) | gains at all $k\leq21$ | gains to $k=13$ | $k=5$ | 86,931 | 4,902 |
| 1.0 (unit) | gains at all $k\leq21$ | gains to $k=13$ | $k=5$ | 88,502 | 4,657 |

Because this surface is at $n=20$, its $\alpha=0$ row differs slightly from the $n=100$ single-maker figures above (flip at $k=7$, rent 84,375); comparisons across $\alpha$ are within the one run. Eighteen of eighteen tape-band cells gain inelastically and seventeen at both mild and unit elasticity; the one exception, ratio 0.0097 at $k=21$, turns indeterminate rather than maker-pays. Noise traders do withdraw — their loss shrinks from $-2{,}667$ to $-2{,}033$ — but the incidence does not move. Elasticity deepens a revenue leg that is already negative (§4.1) without touching the sniping leg that produces the gain. Details are in Supplement S2.4.

**What the two extensions jointly establish.** The class-level answer is structure-dependent, and so is the within-class one. Whether summed maker PnL is positive depends on how many makers supply the book; the class splits into winners and losers, and who is on which side depends on how the makers' latencies are spaced. Elasticity was tested at a single maker, so it supports the single-maker inversion and says nothing yet about the within-class split. The maker-count surface is mapped at $n=100$, the standard of the within-class decomposition; the elasticity surface remains at $n=20$.

### 4.3 What the incidence claim rests on: Experiments 1–4

Four experiments at the pre-registered operating point establish the properties the incidence surface assumes: that the accounting closes, that the mechanism carries no hidden threshold, how competition among snipers changes the rent pool and the book, and that the clearing rule drives the outcome. Per-cell tables and figures are in Supplements S1 and S2.5.

#### Experiment 1: welfare decomposition and the zero-sum conservation check

Over 100 seeds at the operating point the zero-sum residual is $2.78\times10^{-12}$ $[-3.34\times10^{-12}, 9.11\times10^{-12}]$ in treatment and $-5.39\times10^{-12}$ $[-2.16\times10^{-11}, 1.05\times10^{-11}]$ in the matched no-HFT baseline, so the decomposition into HFT, maker and noise-trader PnL has no leak. As §3.4 notes, this checks the bookkeeping only.

Within this conserved frame, HFT entry extracts a rent of 34.70 [26.25, 44.80] per run and the maker's PnL falls by 28.50 [20.35, 38.31] against the matched baseline. That rent is positive by construction at this operating point — a sniper fires only when the stale quote is mispriced by more than its cost, which is zero here — so it calibrates the harness rather than testing a hypothesis. The full decomposition, the microstructure correlates and a $3\times3$ sensitivity-by-decay grid are in Supplement S1.1.

#### Experiment 2: latency-advantage scaling

Experiment 2 sweeps the market maker's staleness, holding the HFTs at zero latency. Rent rises with staleness at approximately 10.46 units of simulation currency per tick ($n=100$), and a linear specification and a segmented one with a free breakpoint are indistinguishable ($\Delta\mathrm{BIC} = -0.17$). We read this as no detectable knee, not as positive support for linearity. Rent first becomes distinguishable from zero at about two ticks of staleness, the detectability floor of a grid that resolves latency only at tick granularity (§6.2). The welfare decomposition closes at every point on the sweep. The figure and per-cell values are in Supplement S2.5.

#### Experiment 3: HFT competition

We next ask how the number of competing high-frequency traders, $k$, shapes the rents they extract and the costs they impose. We sweep $k$ over the grid $\{0, 1, 2, 3, 5, 8, 13, 21\}$ with $n=100$ seeds per cell, holding the market-maker (MM) population and order-flow process fixed. That total rent stays flat as $k$ grows is a consistency check rather than a finding: at this operating point one snipe takes the whole quote (snipe size equal to quote size), every HFT has the same latency and one wins each race on a uniform jitter draw, so the number of snipe opportunities is set by the maker's staleness and the fundamental path and $k$ can only divide it.

Per-HFT rent falls monotonically and steeply, from 34.82 [26.40, 44.84] at $k=1$ to 1.60 [1.21, 2.07] at $k=21$ (Supplement S2.5). The decline is steep and close to hyperbolic, and the same structure appears in execution shares: the per-HFT fill rate falls from 0.2886 [0.2863, 0.2909] at $k=1$ to 0.0139 [0.0137, 0.0140] at $k=21$. The fill rate times $k$ is 0.289 at every count, to within 1% — what one winner per race among $k$ symmetric entrants produces by construction.

Total extracted rent is flat across the grid, with mean 34.42 and range 33.70–34.93 (Supplement S2.5, Table S4). MM PnL is approximately constant near 73 for all $k\geq1$, and the welfare delta tracks total rent and is flat. The pool is fixed only because a single snipe clears the quote. When it does not — the arm of §4.1 with snipes of 2.4% of calibrated depth — later HFTs fill what the first left, and from $k=1$ to $k=21$ total HFT fills rise elevenfold and total rent sevenfold (1,179 to 8,623). Competition enlarges the pool once snipes are smaller than the quote; the full-quote arm here is the special case, and §4.1 maps where between the two the real market sits.

Liquidity gaps grow roughly linearly in $k$, about seventeenfold by $k=21$, and the growth persists with the maker's depth-thinning rule switched off; we have not identified the channel (Supplement S1.2).

**No cost of speed is modelled.** BCS call the arms race socially wasteful because firms spend real resources on speed. Nothing in this model spends anything: latency is a parameter, not an investment, so the transfer is zero-sum by construction and the race's waste is outside what these experiments can measure. What Experiment 3 speaks to is how the transfer changes with the number of fast traders.

#### Experiment 4: batch auctions as a remedy

Experiment 4 switches the clearing rule from continuous matching to a uniform-price call auction, holding every agent and market parameter fixed, and sweeps the clearing interval. Both grids clear through the verified engine's own uncross (§5), so the comparison isolates the clearing rule rather than a reimplementation of it.

Market-maker welfare recovers monotonically once the interval is long enough for the slow quoter to participate: at-least-50% recovery by an interval of about 25 ticks, and about 97% by 500, taking maker loss from 28.5 to 0.7 — the same under both tie-break rules the experiment has been run with (§5). Over 50,000-tick runs the size of the recovery depends on the rule, but both rules agree on the direction: batched, the maker does better than continuous at every interval of five ticks or more (§5). That comparison rests on a matched-seed decomposition rather than on the rent estimator, and it is the result of this experiment we rely on. The welfare figure is in Supplement S2.5.

The accompanying reduction in HFT rent points the same way but is not established. At 3,000 ticks only the interval-5 cell's rent interval excludes zero, and at 50,000 ticks the estimator's dispersion — which grows super-linearly in run length — swamps every batch cell above interval 1, and under the retired tie-break rule three of those cells were significant, so we draw no rent conclusion from the long grid and decline to nominate an optimal interval on rent grounds. At intervals above 100 ticks those cells are uninformative and are in the replication materials (Supplement S4).

The interval-1 cell runs the other way: a per-tick auction worsens maker loss to 41.3, against 28.5 under continuous clearing, and liquidity gaps spike to about 137 against a continuous baseline of 9.7. The obvious reading — the maker's three-tick latency exceeds the one-tick interval, so it cannot refresh within a clearing cadence — is close to a tautology, and it is not what the data show ($n=100$, each arm against its own continuous control). A seven-tick maker facing a five-tick auction, slower than the interval, still does 40% better batched than continuous (−40.1 against −66.1). A maker exactly as fast as the interval (one tick, one tick) is worse under the per-tick auction by point estimate (−16.8 against −10.3), but the intervals overlap; and a zero-latency maker removes the race altogether, the rent being near zero in both arms. The backfire is specific to the per-tick auction rather than to latency exceeding the interval, and whether it survives a maker as fast as the interval is not established. Batching is a remedy above a minimum cadence; what sets that cadence we cannot yet say.

### 4.4 Experiments 1–4 at calibrated parameters

The pre-registered operating point was chosen for mechanism clarity, not realism. §3.6 fits arrival intensity and fundamental volatility to BTCUSDT-perp moments, and this section re-runs Experiments 1–4 at those fitted values with a lognormal size model and the perp's book depth. Every qualitative finding survives the move: the transfer remains a machine-precision zero-sum, rent scales smoothly in maker staleness with no phase transition, and batch auctions recover maker welfare above a minimum cadence while the per-tick auction backfires.

What calibration adds is magnitude. Normalised by traded notional, HFT rent moves from 0.125 bp at the operating point to 4.42 bp calibrated. The calibrated figure is an upper bound rather than an estimate, because a single latency-disadvantaged maker facing the perp's order flow has no competitor to replenish a cleared quote and so absorbs the entire race. Replacing it with $N$ makers whose latencies span the measured top-of-book replenishment quantiles, holding aggregate depth fixed, collapses the rent by a factor of fifty:

**Table 6.** Calibrated HFT rent with $N$ competing makers (rent in basis points of traded notional; liquidity gaps per run).

| $N$ makers | latencies (ticks) | HFT rent (bp) | liquidity gaps |
|---|---|---|---|
| 1 | 3 | 4.421 | 52.73 |
| 2 | 1, 5 | 0.801 | 38.00 |
| 3 | 1, 3, 5 | 0.324 | 16.29 |
| 5 | 1–5 | 0.089 | 2.71 |

$n=100$ seeds; the $N=1$ row reproduces the published single-maker figure, so the sweep nests it rather than re-specifying it, and the zero-sum residual stays at machine zero throughout. The ≈0.4 bp latency-arbitrage tax that Aquilina, Budish, and O'Neill (2022) measure from regulator-held exchange messages falls inside the range these arms span — envelope consistency between a simulated and a directly measured quantity, not agreement on a number.

The mechanism is visible in the per-maker decomposition. At $N=5$ maker PnL is monotone in latency, the fastest earning $+2{,}014$ and the slowest losing $-2{,}393$, because a fast competitor replenishes the touch before the slow maker's stale quote can be sniped. The slow maker still absorbs adverse selection; what changes is that it is no longer the only supplier.

At calibrated parameters the per-tick auction is still the worst batch cell (Supplement S1.2). A second calibrated snipe size, 50 units, is the starting point of §4.1.

## 5. Differential testing against the verified engine

Model checking certifies the matching design at bounded scope (§3.1). It says nothing about code that re-implements part of that design, and the harness contained such code. Testing it against the engine found a divergence that had moved published results.

**The test.** Until the bridge exposed the engine's call auction, the batch arm of Experiment 4 cleared through a Python re-implementation of the clearing rule (`discover_uncross_price`, `simulation/batch_auction.py`), written from the engine's `OrderBook::discoverUncrossPrice` by reading the C++ source line by line. Once the bridge exposed `uncross` and the session-state controls (Supplement S3.3), both implementations could clear the same book. They agree on executable volume but not always on price. On a book with bids 102×50 and 101×30 against asks 99×40 and 100×60, eighty units clear at either 100 or 101 with equal imbalance; the Python rule clears at 100 and the engine at 101. Both apply the same cascade — maximise executable volume, minimise imbalance, minimise distance to a reference price, then market pressure — and on this book they part at the reference step. The engine anchors its reference on the first order's price before any trade (102 here) and picks 101; the Python rule has no reference until its first clearing, falls through to market pressure, and a sell surplus clears lower. A volume-based check cannot see the difference, and every metric that depends on execution price can. The case is pinned in `tests/test_batch_auction.py::test_python_matcher_divergence_pinned`, and the Python rule is kept only as a cross-check.

**The divergence moved published results, through that code path only.** Experiment 4's 50,000-tick grid has seven batch arms and one continuous arm, and the continuous arm never calls `uncross`. We re-ran the grid with batch clearing moved from the Python rule to the engine, holding seeds, fundamental paths, run length and every parameter fixed.

**Table 7.** Experiment 4 at 50,000 ticks ($n=100$), before and after batch clearing moved to the engine. Point estimates in simulation currency (§3.4); \* marks a 95% bootstrap interval that excludes zero.

| arm | HFT rent, Python rule | HFT rent, engine | maker Δ, Python rule | maker Δ, engine |
|---|---|---|---|---|
| interval 1 | 562\* | 572\* | −801\* | −783\* |
| interval 5 | 245\* | −170 | −409\* | 194 |
| interval 10 | 281\* | −252 | −331\* | 445\* |
| interval 25 | 370\* | −194 | −218\* | 322\* |
| interval 50 | 471 | 290 | −165\* | 45 |
| interval 100 | 487 | 426 | −69\* | −7 |
| interval 500 | −554 | −260 | −6 | 1 |
| continuous (control) | 578.1\* | 578.1\* | −687.0\* | −687.0\* |

HFT rent crosses zero in three batch cells, from significantly positive to indistinguishable from zero with a negative point estimate, and the maker's delta changes significance in five. In the continuous control every maker, noise-trader and liquidity-gap value is bit-identical across the two runs, and HFT rent agrees to within $10^{-10}$ (578.1 [449, 709] both times), the floating-point residue of a change to how rent is summed that landed in the same commit. The control rules out seed variation, drift in the agent code and environment differences, and confines the change to the substituted code path. The constructed book shows that the two rules can disagree; the control shows that the disagreement reached published economic quantities and nothing else did.

**The tie-break is an economic assumption.** The engine's rule is correct relative to its own specification, not relative to any venue, and venues order these steps differently, so the port is also a comparison of two plausible tie-break rules. On the 3,000-tick grid the rule changes no batch cell's significance verdict for either the maker's delta or HFT rent. On the 50,000-tick grid it changes most of the batch arm: under the Python rule the maker loses at every interval from 5 to 100 ticks, recovering 40–68% of its continuous-clearing loss at intervals 5–25, while under the engine's rule it breaks even or gains, significantly at intervals 10 and 25. What holds under both rules is the comparison with continuous clearing: at every interval of five ticks or more, the maker does better batched than continuous. How much better, whether it ends up ahead, and whether batching removes the rent all depend on the tie-break.

The episode shows that a re-implementation checked against source code can still differ from the code it copies the first time both run on the same input. It does not show that the engine is correct; it shows that running against the engine caught an error that reading its source did not.

## 6. Discussion

### 6.1 Implications for market design

First, the class-level answer to "who bears the latency tax" is structure-dependent, so a regulator cannot read it off the existence of the tax. Whether summed maker PnL is positive or negative turns on how many makers supply the book and on the snipe-to-depth ratio prevailing there — quantities that differ across venues and that no public feed reports — so an incidence claim transported from one market to another is an assumption rather than a measurement.

Second, at the tape-measured ratio latency competition sorts liquidity providers rather than taxing them uniformly: one maker typically gains while another pays, and which one depends on how their latencies are spaced. A remedy justified as protecting "liquidity providers" protects some of them at the expense of others.

Third, the per-tick auction backfires: it raises maker loss to 41.3 against the continuous 28.5, and a maker slower than the interval does not by itself explain it (§4.3). Batching is a remedy only above a minimum cadence, and what fixes that cadence is not yet measured. Because the maker's recovery above that floor is monotone while the rent reduction is not statistically resolved, the remedy is better described by whom it protects than by what it takes from snipers.

### 6.2 Limitations

**First, the elasticity of uninformed demand.** Noise traders cross at a fixed per-tick probability in every arm except the elasticity surface (§3.2), so the volume a maker earns its widened spread on cannot fall when it widens. §4.2 relaxes this to unit elasticity at a single maker and the maker-gains region largely remains, as the leg decomposition predicts, but elasticity above unit is untested and the within-class split has not been tested against elastic demand at all.

**Second, liquidity supply is narrow in both count and rule.** §4.2 maps maker counts to five, but §4.1's headline arm holds one maker, and every maker in this paper follows a single imposed adverse-selection quoting rule. A maker that defended itself differently — fading quotes, skewing on inventory — might not profit from small snipes at all, and none of ours chooses its spread to maximise profit, so "the maker gains" is a property of that rule under sniping rather than a statement about profit-maximising liquidity provision. The rule's aggressiveness is one hand-tuned parameter, and re-running §4.1's main readings across a $7.5\times$ span of it ($n=20$, `exp6b_sensitivity_adverse.json`) leaves the inversion's *existence* intact at the tape band while moving its *location* across the whole grid and changing total rent by a factor of five; no critical HFT count should therefore be read as a property of the market rather than of this maker. Scope is also narrow: one asset on one venue, spread and depth *levels* far from the perp's (§3.6), latency resolved only at tick granularity, and IOC-in-batch semantics in the auction arm that are a modelling choice rather than an institutional given.

**Third, the ratio measured on the tape and the ratio swept in the simulation are not the same object.** The tape's 0.0072 is a snipe against *aggregate* top-of-book depth, whereas the simulation measures it against *one* maker's quote, so the per-maker ratio lies between the tape ratio and the tape ratio times the number of makers at the touch — a count no public feed reports. §4.2 shows why this matters: splitting aggregate depth across $M$ makers multiplies each maker's own ratio by $M$, and the maker-gains region at the tape band is all but gone at $M=2$. What remains unobservable is $M$ itself. The surface reports every cell of a grid fixed before the runs, so we apply no cell-level multiple-testing correction (Supplement S2.6).

**Fourth, the model's clock and its costs are coarse where the rents are small.** One tick is 100 ms (§3.6), a convention the 100 ms book snapshots force, but races on real venues are decided in microseconds (Aquilina, Budish, and O'Neill, 2022), so the model cannot represent the race at the race's own resolution. Fees are absent: Binance's standard USDT-margined futures schedule charges about 2 bp to makers and 5 bp to takers, the same order as or larger than the 0.089–4.42 bp rents of §4.4, and a fee schedule enters every leg of the decomposition. Every incidence result here is gross of fees (Supplement S2.6).

None of the four changes the structural point: the identity of the class bearing the latency tax is not settled by the existence of the tax. It is a separate empirical question, and at the measured snipe size its answer depends on how many makers supply the book.

### 6.3 Future work

On the structural axes, what stays open is a held-out test of whether the latency configuration predicts which maker wins (§4.2), elasticity above unit and its interaction with the maker-count surface, and the elasticity surface at $n=100$; both surfaces resolve their boundaries only to the width of the grid.

First, the single-venue, single-asset scope should be relaxed. A multi-venue extension would let us study latency arbitrage across correlated instruments and order routing, where snipers can compete across books rather than within one.

Second, the strategy spaces of both the market maker and the HFTs are deliberately narrow and should be enriched — for example, inventory-aware or quote-fade defences for the maker and more elaborate timing or order-type strategies for the snipers — to test how far the welfare decomposition and the batch-auction recovery are robust to more sophisticated play on both sides.

Third, the adverse-sensitivity check of §6.2 should be finished on its own terms, since it currently resolves the boundary's direction more confidently than its location. Two refinements would do it: re-running it at §4.1's $n=100$ with the full ratio axis restored, the present $n=20$ check pinning the boundary only to the width of the $k$-grid; and extending it to spread decay, which enters the same stationary-spread relation and was held at its pre-registered value throughout. The spacing point applies to §4.2's surface too: at $n=100$ it places the closing of the tape-band region between $M=2$ and $M=3$, a resolution set by the $M$ grid.

## 7. Conclusion

Our main result concerns incidence *within* the liquidity-providing class. At the snipe-to-depth ratio measured on the BTCUSDT-perpetual tape, competing makers split the tax — one typically gains while another pays (§4.2) [TODO: held-out prediction test] — and who pays is set by how their latencies are spaced rather than by speed rank. The class-level answer is structure-dependent: a lone maker at the measured ratio profits from the arms race and noise traders bear the transfer, while adding a competitor reverses that at all but the lowest sniper counts and restores the incidence Budish, Cramton, and Shim predict. Aggregate maker PnL is therefore a weighted sum over unlike participants rather than a verdict on liquidity provision, and "do liquidity providers bear the latency tax?" has no structure-free answer.

One condition applies throughout: every maker here follows an imposed adverse-selection widening rule with an exogenous sensitivity, so "the maker gains" is a property of that rule under sniping, not a claim about profit-maximising liquidity providers who choose their spread. The results rest on a matching engine whose design is model-checked at bounded scope, on welfare accounting that closes in every cell, and on a differential test that caught, and removed, a clearing-rule divergence in the harness (§5).

## References

Aquilina, M., Budish, E., & O'Neill, P. (2022). Quantifying the High-Frequency Trading "Arms Race". *Quarterly Journal of Economics*, 137(1), 493–564.

Avellaneda, M., & Stoikov, S. (2008). High-Frequency Trading in a Limit Order Book. *Quantitative Finance*, 8(3), 217–224.

Baldauf, M., & Mollner, J. (2020). High-Frequency Trading and Market Performance. *Journal of Finance*, 75(3), 1495–1526.

Biais, B., Foucault, T., & Moinas, S. (2015). Equilibrium Fast Trading. *Journal of Financial Economics*, 116(2), 292–313.

Brogaard, J., Hendershott, T., & Riordan, R. (2014). High-Frequency Trading and Price Discovery. *Review of Financial Studies*, 27(8), 2267–2306.

Budish, E., Cramton, P., & Shim, J. (2015). The High-Frequency Trading Arms Race: Frequent Batch Auctions as a Market Design Response. *Quarterly Journal of Economics*, 130(4), 1547–1621.

Copeland, T. E., & Galai, D. (1983). Information Effects on the Bid-Ask Spread. *Journal of Finance*, 38(5), 1457–1469.

Du, S., & Zhu, H. (2017). What Is the Optimal Trading Frequency in Financial Markets? *Review of Economic Studies*, 84(4), 1606–1651.

Farmer, J. D., & Foley, D. (2009). The Economy Needs Agent-Based Modelling. *Nature*, 460(7256), 685–686.

Foucault, T., Kozhan, R., & Tham, W. W. (2017). Toxic Arbitrage. *Review of Financial Studies*, 30(4), 1053–1094.

Glosten, L. R., & Milgrom, P. R. (1985). Bid, Ask and Transaction Prices in a Specialist Market with Heterogeneously Informed Traders. *Journal of Financial Economics*, 14(1), 71–100.

Hoffmann, P. (2014). A Dynamic Limit Order Market with Fast and Slow Traders. *Journal of Financial Economics*, 113(1), 156–169.

LeBaron, B. (2006). Agent-Based Computational Finance. In *Handbook of Computational Economics*, Vol. 2 (pp. 1187–1233).

Menkveld, A. J. (2013). High Frequency Trading and the New Market Makers. *Journal of Financial Markets*, 16(4), 712–740.

Menkveld, A. J. (2016). The Economics of High-Frequency Trading: Taking Stock. *Annual Review of Financial Economics*, 8, 1–24.

Menkveld, A. J., & Zoican, M. A. (2017). Need for Speed? Exchange Latency and Liquidity. *Review of Financial Studies*, 30(4), 1188–1228.

Shkilko, A., & Sokolov, K. (2020). Every Cloud Has a Silver Lining: Fast Trading, Microwave Connectivity, and Trading Costs. *Journal of Finance*, 75(6), 2899–2927.

Wah, E., & Wellman, M. P. (2013). Latency Arbitrage, Market Fragmentation, and Efficiency: A Tactical Market Model. In *Proceedings of the 14th ACM Conference on Electronic Commerce (EC)*, 855–872.
