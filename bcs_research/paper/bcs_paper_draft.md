# Who Bears the Latency Tax? Incidence of the HFT Arms Race on a Model-Checked Limit Order Book

<div align="center">

**Atharva Patil**

Xavier Institute of Engineering

</div>

## Abstract

Budish, Cramton, and Shim show that the continuous limit order book's latency arms race imposes a tax; which participants bear it is a separate question. We study it on a nanosecond discrete-event model in which a maker's cancel and snipers' orders race to a C++ matching engine whose design is model-checked in TLA+, with a jump-diffusion fundamental, order flow and fees taken from the BTCUSDT-perpetual tape, and with eleven predictions — the first that the model recovers BCS's own equilibrium — fixed in a pre-registration before any confirmatory run. [TODO after results: the replication verdict (P1); who bears the tax between makers and investors and among makers (P2, P8, P10); waste, fees and participation (P4–P7); batch auctions (P9); how many predictions hit, missed or were inconclusive.] A differential test of the engine against a Python re-implementation of its batch clearing rule found a tie-break divergence that had moved the earlier version's batch-auction results.

## 1. Introduction

### 1.1 The arms-race argument

Budish, Cramton, and Shim (2015, hereafter BCS) argue that the continuous limit order book's serial processing creates a mechanical arbitrage on every public price move: whoever reacts first picks off liquidity providers' stale quotes before they can be revised. The resulting race over speed rather than price dissipates resources and taxes liquidity provision without improving price discovery, and BCS propose frequent batch auctions — discrete-time, uniform-price clearing — to turn competition on speed back into competition on price.

### 1.2 The question and the design

This paper asks which participants bear the latency tax. The BCS argument establishes that the tax exists; it does not settle who pays it — makers or the investors who trade with them, and which of several competing makers. The answer depends on how the race is run, how makers quote, and the fees each side pays.

An earlier version of this paper answered on a 100 ms tick model. It reported that a single maker gains from the race at the snipe size measured on the tape, and that which of several makers pays is set by how their latencies are spaced. That model's clock could not represent the race between a cancel and a snipe, and two of its results came from when it sampled the book (§3.8). We therefore rebuilt the model on a nanosecond event clock over the same verified engine (§3), recalibrated it to what the tape can resolve (§3.9), and wrote down eleven predictions before running it (§3.11). Several predict that the earlier headlines will not survive. Each is reported as a hit, a miss or inconclusive (§4).

[TODO after results: the main result in two or three sentences, each with the prediction and verdict it rests on.]

### 1.3 Contributions

**First**, it treats the incidence of the latency tax as a question distinct from its existence and tests it under predictions fixed in advance: how the race's cost divides between makers and investors (P2, P7), which of several makers pays (P8, P10), how much of the rent free entry turns into waste (P4), and what fee schedules and batch auctions change (P5, P9). [TODO after results]

**Second**, it requires the simulator to recover BCS's closed-form equilibrium — the race-loss share, the equilibrium spread and that spread's independence of the number of firms — before using it for anything BCS did not say (P1, §4.1). [TODO after results: P1 verdict]

**Third**, it states what a public crypto tape can say about races: stamps are whole milliseconds, futures records merge takers, and a race's footprint can only be bounded (§3.9).

**Fourth**, it uses differential testing against the model-checked engine as an internal-validity check: a Python re-implementation of the batch clearing rule, checked against the C++ source line by line, cleared a volume-tied auction at a different price the first time the two ran on the same book, and the difference moved the earlier version's batch results (§5).

## 2. Related Literature

**The arms race and its measurement.** Budish, Cramton, and Shim (2015) treat latency competition as a consequence of the continuous book's serial processing rather than as conduct to be regulated, and propose frequent batch auctions as the remedy. Aquilina, Budish, and O'Neill (2022) measure the race directly from message-level data that record failed attempts: races occur about once a minute per FTSE 100 symbol, are decided by microseconds, and sum to a latency-arbitrage tax of about 0.4 basis points of volume. Neither paper asks which participants bear the tax. The closest evidence on that question is Shkilko and Sokolov (2020): when weather disrupts the microwave links that give the fastest traders their edge, adverse selection falls and trading costs decline, in a market with many competing liquidity suppliers. Menkveld (2016) surveys HFT's double role as liquidity supplier and source of adverse selection and notes that the net welfare sign for any participant class is unsettled. Menkveld (2013) and Brogaard, Hendershott, and Riordan (2014) place fast traders on the maker side of the book as well as the sniping side, which is why incidence within the liquidity-providing class is a separate question from incidence on the class.

**Speed and exchange design.** Menkveld and Zoican (2017) show that a faster exchange can worsen liquidity, because it speeds up snipers and makers alike and raises the frequency of speed duels. Baldauf and Mollner (2020) compare batch auctions with asymmetric speed bumps, and Du and Zhu (2017) derive the optimal batching frequency as a trade-off between information aggregation and delayed incorporation. Hoffmann (2014) shows that fast traders concentrate pick-off risk on slow limit-order submitters, who respond by quoting less aggressively, and Biais, Foucault, and Moinas (2015) show that equilibrium investment in speed exceeds the social optimum because the fast impose adverse selection on the slow. These models derive who is fast and what speed costs in equilibrium. We impose the latencies, let the makers' quoting range from imposed rules to best responses over a strategy space (P8), and ask how incidence varies with market structure.

**Adverse selection and the spread.** Copeland and Galai (1983) model the spread as a trade-off between revenue from uninformed traders and losses to informed ones, and Glosten and Milgrom (1985) make the informed-trading component explicit. Foucault, Kozhan, and Tham (2017) show empirically that arbitrages triggered by news, rather than by transient imbalance, are the ones that widen spreads. Because widening is at once a defence against the informed leg and a tax on the uninformed one, the maker's two legs are tested separately (P7, §4.4) rather than only their difference.

**Agent-based models.** The agent-based computational finance tradition (Farmer and Foley, 2009; LeBaron, 2006) argues that simulations of heterogeneous agents can produce out-of-equilibrium phenomena that representative-agent models cannot; its known weaknesses are ad hoc behavioural assumptions, weak calibration, and simulation machinery that is hard to verify. Wah and Wellman (2013) built an agent-based model of latency arbitrage across fragmented markets and found that the arbitrageur profits at other traders' expense while degrading allocative efficiency, the same qualitative transfer we decompose. What this paper adds is the substrate and the question: the BCS mechanism runs on a matching engine whose design is model-checked and has been tested differentially against a re-implementation (§§3.1, 5), with accounting whose identities hold to rounding (§3.6), and the question asked of it, under pre-registered predictions, is who bears the tax rather than whether it exists.

## 3. Model and Experimental Design

One C++ matching engine makes every trade. A Python layer above it supplies the clock, the agents, the fundamental and the accounting, and a pybind11 bridge between the two marks where the engine's formal guarantees end. Every result in §4 is computed on the nanosecond event model described here; §3.8 says why the 100 ms tick model of the paper's earlier version was replaced.

### 3.1 The engine, the bridge, and what the verification covers

All trades, continuous and batch alike, are produced by one C++20 matching engine, reached only through the pybind11 bridge. The engine runs synchronously and has no notion of latency: the race lives in the scheduler above it (§3.2). The bridge exposes order entry, cancels, the book, the session states and the call-auction `uncross`, and it reports every order the book refuses after admission, so that no agent believes a refused quote rests (Supplement S3.3).

The engine's matching design is model-checked in TLA+. At its deepest exhaustive configuration the continuous-matching specification explores 171,187,419 distinct reachable states with zero invariant violations, checking FIFO execution and conservation; the auction specification checks that every uncross clears at a single price. The verification has two limits. It is exhaustive only within small bounds — a handful of orders, two prices, short horizons — so it certifies the matching *design* at bounded scope. And it verifies the specification, not the C++ source; their correspondence rests on construction and conformance testing. Nothing above the bridge is verified: the scheduler, agents and accounting are checked by unit tests and, together, by requiring the model to recover BCS's equilibrium before it is used for anything else (P1, §4.1). Supplement S3 lists the specifications; §5 reports a differential test of the engine's clearing rule.

### 3.2 The scheduler and its latency semantics

The scheduler is a discrete-event loop on an integer-nanosecond clock (`simulation/event_scheduler.py`, implementing `simulation/contracts.py`). Each agent has an observation latency and an order latency, applied as follows.

- **Observation.** An agent observes a public update, and its own fills, at the event's time plus its observation latency.
- **Orders and jitter.** Orders and cancels sent at time $t$ reach the engine at $t$ plus the order latency plus a jitter drawn uniformly from $\{0,\dots,\varepsilon\}$ ns. Each agent draws from its own seeded stream, so adding or removing an agent — the treatment in a matched pair of runs — leaves every other agent's draws unchanged.
- **Bursts and per-agent FIFO.** One callback's messages arrive together and apply in the order sent, so a maker's requote cannot overtake its own cancel. Successive bursts from one agent arrive in send order however wide its jitter.
- **Ties.** Events due at the same nanosecond run in a seeded random permutation, never in registration order; zero-delay events run after everything already due at that instant, so no agent acts on news before every other has had the chance to observe it.
- **Rejects.** An agent learns that the engine refused one of its messages — an order, or a cancel of an order already gone — its observation latency after the refusal, so a refused post-only quote is not kept in the book the agent believes it has.
- **Book reads.** Agents read the current top of book; book-data latency is not modelled, and only the public signal's latency races.

For every jump the scheduler logs the engine arrivals that agents' reactions produced, in engine order, with each outcome; race-win rates are counts over that log. Top-of-book snapshots are taken every 100 ms, the tape's book cadence.

### 3.3 The cancel-versus-snipe race

A jump makes the maker's quotes stale. The maker, on observing it, sends one burst that cancels them and posts new ones; each sniper sends one immediate-or-cancel (IOC) order at the stale price if the quote is stale by more than its cost. If the cancel arrives first, every snipe finds nothing at that price and dies — a sniper never sends a market order, so a losing snipe cannot lift the new quote. If a snipe arrives first, it fills. The outcome is set by latencies and jitter alone; there is no race-specific code. With independent $U[0,\varepsilon]$ jitter on every racing message, a maker whose total latency exceeds the snipers' by $\ell$ beats all $k$ snipers with probability
$$
P_{\text{win}}(\ell,\varepsilon,k) = \frac{1}{k+1}\left(\frac{\varepsilon-\ell}{\varepsilon}\right)^{k+1} \quad (0\le \ell\le\varepsilon), \qquad 0 \quad (\ell>\varepsilon),
$$
derived in `analysis/bcs_theory.py`; at $\ell = 0$ it is BCS's $1/N$ with $N = k+1$. P2 tests the formula and P7 the loss it implies. In the tick model a maker and a sniper reacting to the same news acted in the same 100 ms tick, and their race was a jitter draw inside it (§3.8).

### 3.4 The fundamental

The fundamental $V(t)$ is a jump-diffusion published as discrete updates (`agents/fundamental_paths.py`): a Brownian diffusion on a fixed grid, every 10 ms at the operating point, plus compound-Poisson jumps at rate $\lambda_j$ anywhere in nanosecond time, with signed Laplace$(0,b)$ sizes, so the absolute jump $J$ is exponential with mean $b$. Every change is a public update, and jumps are flagged for the race log. The path is generated whole and seed-deterministically before a run. Agents trade only on the updates they observe; the true $V$ is read by the accounting, the maker's adverse-fill test and the slow informed investor. Laplace jumps give BCS's equilibrium a closed form: the per-jump sniping prize at half-spread $h$ is $b\,e^{-h/b}$, and BCS's equation (3), $\lambda_i h = \lambda_j b\,e^{-h/b}$ with investor arrival rate $\lambda_i$, gives $h^* = b\,W(\lambda_j/\lambda_i)$, $W$ the Lambert function. A correlated-signal variant, in which a lead instrument publishes and the traded asset's own value is never published, is implemented and unit-tested; no pre-registered experiment uses it.

### 3.5 Agents

Every agent implements one interface — callbacks on start, on an observed update, on a wakeup and on a fill, each returning orders, cancels and wakeups.

**Makers.** One class executes any maker strategy given as a `MakerPolicy` (`agents/strategic_maker.py`), a frozen value that a best-response search can perturb and compare (P8). Its dimensions are: quoting at all (the no-quote option); base half-spread and size; a ladder of levels; Avellaneda–Stoikov (2008) quoting with risk aversion $\gamma$ over a rolling horizon, reducing to fixed-spread quoting at $\gamma = 0$; inventory skew and an inventory limit; adverse-selection widening; quote fading after a jump or adverse fill; a requote threshold; requoting after a fill; and post-only quotes. The default is BCS's liquidity provider: a fixed half-spread around the last update observed, cancelled and requoted in one burst on every update. The widening dimension carries the earlier version's maker onto the event clock: a fill adverse against the true $V$ multiplies the half-spread by one plus a sensitivity, up to a cap, the multiple decays on every update, and size can thin by the same factor.

**Snipers.** A sniper reacts only to news. If an update leaves a resting quote stale by more than its threshold, the taker fee of the run's schedule, it sends one IOC at that price for its snipe size (`agents/event_sniper.py`).

**Investors.** Uninformed flow is a Poisson population of reservation-value investors (`agents/event_noise.py`). Arrival $i$ values the asset at the last update plus a private shock $\eta_i \sim N(0,s_\eta^2)$, buys at the ask if that value is at least the ask, sells at the bid if it is at most the bid, and otherwise leaves. Participation therefore falls as the spread widens without an elasticity parameter; at half-spread $h$ around the signal an arrival crosses with probability $2\Phi(-h/s_\eta)$. Sizes are lognormal.

**The slow informed investor** reads the true $V$ late at fixed decision times and works a parent order toward it as IOC children at the touch. It is the one agent that reads $V$ directly, since learning $V$ late is what informs it. It is outside the operating point; P6 adds it, one second late, to test BCS's claim that information slower than the quote-setter's is worthless in a continuous book.

### 3.6 Accounting: fees, rebates, the cost of speed, and two identities

Welfare is one fold over the engine's trade list (`metrics/accounting.py`), so it cannot drift from what executed. Every fill pays the maker or taker fee in basis points of notional; the maker is the non-aggressor, and a negative maker fee is a rebate. Fees are transfers to the exchange, which gives two identities that hold to rounding: gross trading P&L sums to zero across participants at any common mark, and net P&L plus exchange revenue sums to zero. The cost of speed is a third leg, money that leaves the market for fibre and towers — the resource BCS call waste — and is reported beside the identities, never inside them (P4). Open inventory is marked at the last two-sided mid and at the true $V$, and rents are read at $V$; where a prediction concerns profit per event, fills are scored as edge against $V$ at execution, BCS's payoff. The operating point charges makers 0 and takers 1.7 bp, Binance USDⓈ-M's lowest public taker tier (VIP 9); P5 adds zero fees, the regular tier's 2 bp maker and 5 bp taker, and a stylised −1/3 bp maker rebate.

### 3.7 Frequent batch auctions on the engine's uncross

The batch arm (`simulation/event_batch.py`) keeps the scheduler's clock, latencies, jitter, FIFO and tie rule, and replaces matching on arrival with BCS's frequent batch auction. Interval $k$ is the half-open window $[k\tau,(k+1)\tau)$, cleared at $(k+1)\tau$ before any arrival at that nanosecond. Cancels take effect on arrival, so a cancel received during an interval is honoured before its uncross. New orders are held to the boundary, handed to the engine in arrival order (or a seeded shuffle, in a rationing arm) and cleared by its own `uncross`; an IOC enters as a limit for that auction only. The book is sealed: agents see it as it stood after the last uncross. The engine stamps every uncross trade with a buy aggressor, so for fees the later arrival is the taker.

The engine's clearing rule is not BCS's. It ranks candidate prices by volume, then imbalance, then distance to a reference price, then market pressure, and its reference is the session's first limit order, which `uncross` never updates (`tests/test_event_batch.py`). With one stale ask at $A$ and two or more snipers bidding $V' > A$, every price in $[A, V']$ ties on volume and imbalance, and the engine clears at the stale price $A$ whenever its reference lies nearer $A$; BCS's Bertrand argument (Prop. 6(iii)) clears at $V'$. A second rule, "pressure", drops the reference step by repricing the engine's fills, only when the reference-free price clears the same volume inside every filled order's limit. P9 reads its verdicts on the engine as it is. BCS give the exposure: a provider slower than the snipers by $\delta$ is exposed on a share $\delta/\tau$ of jumps (Prop. 6(i)), a fast one only through jitter, $\varepsilon/(2\tau)$ (footnote 34), which we refine to $(\varepsilon/\tau)(N-1)/(2(N+1))$ by requiring a sniper to make the batch the provider missed.

### 3.8 The earlier tick model

The earlier version ran on a tick model: every agent acted on a 100 ms grid, the fundamental was a Gaussian random walk stepped each tick, the maker observed it three ticks late, snipers reacted within the tick, and noise traders crossed with a fixed probability per tick. Its clock could not represent the race: a maker and a sniper reacting to the same news acted in the same tick, and their race was a jitter draw inside it. Its random walk had no jumps, which is where BCS's rents come from (P3). The growth of liquidity gaps with the number of snipers, and the decline of rent with it, came from where the end-of-tick snapshot fell; and its batch arm entered snipers' IOCs as limits at the stale price, letting them outbid the maker for noise flow. Its results are not carried over; the claims the pre-registration re-tests appear in §4 as predictions, and Supplement S5 describes the model, the retired results and the reason for each.

### 3.9 Calibration to the BTCUSDT-perpetual tape

**Data, and what it can resolve.** The primitives are fitted to Binance BTCUSDT-perpetual order flow from a companion collector: 20-level book snapshots at about 10 per second and the aggregated-trade stream, over 27 days (2026-06-24 to 2026-07-21, less 2026-07-19): 37.0 million trade records in 2.27 million seconds of coverage, every moment computed within hole-free hourly files (Supplement S2.1). The earlier version said both streams carry nanosecond timestamps; they do not. A trade's exchange timestamp is Binance's millisecond stamp, and none of the 37 million is finer than a millisecond. The trade id has no gaps within a file, so the order of records inside a millisecond is exact. And a USDⓈ-M futures record is not one taker order: Binance merges fills at one price and taking side within 100 ms, across takers. Price changes are therefore stamped to the millisecond, but record counts are not taker counts, and several snipers hitting one stale level print as one record (`calibration/fine_grain.py`, `results/calibration/fine_grain_v2.json`).

**Order flow and book.** Records arrive at 16.28 a second, with sizes of 0.003, 0.177 and 2.01 BTC at p50, p90 and p99; the median spread is one 0.10 USD tick (0.0158 bp) and the median top-of-book depth 6.18 BTC. These set the investors' crossing rate, their lognormal size law (median 3 lots of 0.001 BTC, $\sigma_{\ln} = 2.80$ from the p50–p99 ratio), the price grid and the 6,180-lot aggregate quote.

**Jumps.** A Lee and Mykland (2008) threshold test at the 1% level runs per hole-free hour on the trade-implied mid, the trade price less half a tick for a buy and plus half a tick for a sell, with the local scale from bipower variation of one-minute returns (`calibration/jumps.py`). At the 10 ms update interval it gives a diffusive $\sigma$ of 0.242 bp/$\sqrt{\text{s}}$, 0.200 jumps a second and a Laplace scale $b$ of 0.705 bp, with jumps carrying 78% of return variance (`results/calibration/jump_fit_v2.json`). The Laplace law matches the jumps' second moment to within 6% but not their shape (Kolmogorov–Smirnov distance 0.27). The fit depends on the interval — 0.037 and 0.004 jumps a second with $b$ of 1.49 and 3.97 bp at 100 ms and 1 s — and the tape's volatility rises from 0.52 to 0.65 bp/$\sqrt{\text{s}}$ between 10 ms and 1 s, which no Lévy path reproduces. The operating point uses the 10 ms fit, its own update cadence (Supplement Table S10).

**Races on the tape.** A public tape shows fills, not the failed orders that identify a race (Aquilina, Budish, and O'Neill, 2022), so we bound the footprint a race leaves (`calibration/race_proxy.py`). A trigger is a record that moved the trade-implied mid in its own direction; a race is two or more same-side records from the start of the trigger's millisecond to $W$ ms after it. The upper bound counts every such burst, including one taker sweeping several levels; the lower bound counts only bursts spanning at least two millisecond stamps, which must involve at least two takers but misses same-millisecond races. The upper bound is 1.39 races a second at $W = 0$, falling to 0.40 at 100 ms; the lower bound is 0.29 at 1 ms and peaks at 0.53 at 5 ms (Supplement Table S11).

**What the races follow (exploratory).** An exploratory analysis, not pre-registered, with thresholds set before it ran, sorted the races by the size of the move behind them (`calibration/race_by_move_size.py`; `paper/race_gap_note.md`; `results/calibration/race_by_move_size_v2.json`; day-block bootstrap intervals over the 27 days). Of the same-millisecond races 99% follow moves smaller than the 1.7 bp taker fee, and 97% of the lower-bound races do. After moves that clear the fee plus a half-spread the tape shows 0.0096 races a second [0.0070, 0.0126] at the upper bound and 0.0083 [0.0064, 0.0105] at the lower, and 79% of such moves draw a race within the same millisecond. The moves themselves arrive at 0.0087 a second [0.0065, 0.0112] on a 10 ms grid, 0.49 [0.36, 0.63] times the rate of the model as run; on 100 ms and 1 s grids the ratio is 0.79 [0.57, 1.06] and 1.67 [1.26, 2.14], because the tape's fee-sized moves include slower ones a jump-diffusion lacks (Supplement Tables S12–S13).

**Race size against depth.** The incidence predictions are indexed by a race's size relative to the top-of-book depth it met. At $W = 0$, against the hit side's level-one size in a snapshot at most 200 ms old, the median is 0.052 (550,014 races); against snapshots at most 20 ms old it is 0.18 (109,036 races), because a stale snapshot shows depth already taken (`results/calibration/race_proxy_v2.json`). The earlier version's 0.0283 triggered on 100 ms book snapshots and regrouped records under the spot market's aggregation rule, one record per taker order and price level, which futures records do not follow. The pre-registration therefore evaluates every count over "tape-band" cells on the corrected ratios {0.052, 0.18} beside the original {0.0049, 0.0097} (§3.11).

**Stylized facts.** Table 1 measures the tape, the tick model and the event model with one function (`analysis/stylized_facts.py`). The event-model column is the facts pilot: ten one-hour seeds at a pre-sign-off operating point whose race differs from §3.10's — three snipers 90 µs faster than one maker, no jitter — and which includes the slow informed investor. Its fills are merged into aggregate-trade records under the futures rule, so it is measured as the tape is, and no parameter was fitted to a fact. A fact *matches* if it has the tape's sign and lies within a factor of two of it (or an absolute floor), is *closer* if its relative error beats the tick model's, and otherwise *misses*, a tolerance stated before the run. These are calibration diagnostics, not hypothesis tests. [TODO: rerun at the signed-off operating point of §3.10 and replace this column.]

**Table 1.** Stylized facts on the tape, the tick model and the event model (facts pilot, 10 seeds × 1 h). Spreads and volatilities in bp; impact in bp per standard deviation of signed flow. Every fact is in Supplement Table S14.

| fact | tape | tick model | event model | verdict |
|---|---|---|---|---|
| median spread | 0.0158 | 1.20 | 0.0159 | matches |
| trades per second, mean | 15.87 | 20.84 | 14.34 | matches |
| trades per second, Fano factor | 125.3 | 0.83 | 0.79 | misses |
| volatility of 1 s returns (per √s) | 0.656 | 8.17 | 0.508 | matches |
| volatility of 10 s returns (per √s) | 0.696 | 2.26 | 0.507 | matches |
| lag-1 autocorrelation, 100 ms returns | 0.101 | 0.343 | −0.002 | closer |
| lag-1 autocorrelation, 10 s returns | 0.020 | −0.462 | 0.006 | matches |
| trade-sign autocorrelation, lag 1 | 0.585 | 0.607 | −0.093 | misses |
| trade-sign autocorrelation, lag 100 | 0.102 | −0.008 | 0.001 | closer |
| share of records sharing a millisecond | 0.602 | 0.569 | 0.004 | misses |
| impact, same second | 0.341 | 3.44 | 0.124 | closer |
| impact, 30 s later | 0.311 | 1.21 | 0.114 | closer |

The event model matches the tape's spread, mean trade rate, volatility and long-horizon return autocorrelation, where the tick model's spread was 76 times the tape's; its impact is persistent like the tape's but a third of its size. It misses everything that depends on clustered or split order flow: trade counts are close to Poisson, trade signs carry no memory, and almost no records share a millisecond.

The event model is therefore a mechanism model with tape-calibrated primitives, not a market replica. Its jump law, order-flow rate and sizes, price grid, depth and fees come from the tape and the venue; its strategic structure — who is fast, by how much, and how makers quote — is designed and swept, because no public feed reveals it.

### 3.10 The operating point

The pre-registration fixed the operating point before any confirmatory run (`experiments/operating_point.py`, each value pinned by a test). The fundamental is §3.9's 10 ms jump fit on a grid where one engine unit is the venue's 0.10 USD tick ($v_0 = 630{,}927$). Investors cross at 16.28 a second against the BCS half-spread, with a private-value scale of 0.52 bp, the path's total one-second volatility; their arrival rate is fixed, not re-solved per arm, so the investor stream is the same in every arm whatever the makers quote. Makers rest 6,180 lots in aggregate, split evenly across $M$ makers. The default maker quotes BCS's zero-profit half-spread at the fitted parameters, 0.0086 bp, about half a tick, so on the grid it mostly quotes a one-tick spread. Maker latencies are one-way — observation and order latency both $L$ — and each prediction states its own; by default the maker is as fast as the snipers, who observe and send in 50 µs each. Every racing firm, makers included, draws $\varepsilon = 80$ µs of jitter per burst; investors do not race. Snipe size is a stated ratio of the aggregate quote. Fees are 0 for makers and 1.7 bp for takers, and a sniper's threshold is the taker fee. Runs last 30 simulated minutes, $n = 100$ seeds per cell unless a prediction says otherwise. An experiment's arms share each seed's path and investors, so every effect is a paired per-seed difference. Where a prediction concerns the earlier version's maker, it runs on the event clock with its calibrated sensitivity 0.015, decay 0.1 per 100 ms, cap of ten times base and size thinning, cancelling and requoting on every update, at 300 ms one-way unless the prediction states another latency.

### 3.11 Pre-registration and inference

Eleven predictions, P1–P11, were written before the event-model experiments they cover were run, and signed off on 2026-10-12 (`paper/PREREGISTRATION.md`). Each names a direction, a magnitude wherever theory supplies one, a test, and the outcome that counts as a miss; several predict that a headline of the earlier version will not survive. Every prediction ends as exactly one of HIT (its decision rule is met), MISS (its miss condition is met) or INCONCLUSIVE (neither). An inconclusive result never counts as a hit, and if more than a third of the primaries are inconclusive the program is underpowered and §4 says so first.

**Pilots and confirmatory runs.** Pilot runs before the sign-off built and sized the experiments. They are kept apart (`results/pilot/`), labelled as not tests, and none is reported here as a result; the only pilot figures in the paper are Table 1's calibration diagnostics and one sniping rate in §6.2, both labelled. The confirmatory batch started after the sign-off was committed, at §3.10's operating point, and every result in §4 comes from it.

**Amendments and clarifications.** Three amendments were approved at sign-off and logged with whether results had been seen. Every count over "tape-band" cells — the earlier version's 18 cells, snipe-to-quote ratios {0.0049, 0.0097} by sniper counts {1, 2, 3, 5, 6, 7, 8, 13, 21} — is also evaluated on §3.9's corrected band {0.052, 0.18}, with verdicts read on the original band. A secondary prediction, P10b, was added because a pilot suggested it — the maker most often at the front of the queue at the stale touch when a jump's first snipe arrives is the one that pays — with its predictor committed before the confirmatory run, which draws fresh configurations. And P11's prediction about the noise-trader toll became an equivalence test, because the toll is near zero and a variance index on it is noise. Details the predictions left open were fixed before any confirmatory result existed (pre-registration §4.1); those that bear on §4: the slow informed investor is outside the operating point; maker latencies are one-way; P9 reads the engine's reference price as the session's first order, where P9 had assumed the last clearing price; P10b's verdict of record uses the configurations where its predictor names a payer; and P11's uses its written 512 base samples, with 2,048 reported beside. Where a clarification departs from a literal reading, both readings are reported.

**Tests.** Intervals are 95% percentile bootstraps of the mean over seeds, 2,000 resamples; proportions use Wilson intervals. Each experiment has one primary hypothesis, and the eleven primaries form one family, Holm-adjusted (Holm, 1979) at $\alpha = 0.05$; secondaries use unadjusted 95% intervals. A magnitude predicted "within $\pm x$" is decided by a band rule: HIT if the 95% interval lies inside the band, MISS if it is disjoint from it, INCONCLUSIVE otherwise — not "the interval contains the prediction", which across a dozen cells would fail a correct model about half the time. Predictions of no effect or invariance are tested by two one-sided tests: HIT if the 90% interval lies inside the stated margin, MISS if the 95% interval is disjoint from it.

## 4. Results

This section reports the confirmatory runs and nothing else (§3.11), in the order a reader needs them: validation, the race and its incidence, welfare and waste, market design, sensitivity. Each subsection states what was predicted, in substance as pre-registered, then the verdict, estimate and interval. Tape-band counts are read on the paper band, with the corrected band beside. The earlier version's results are in Supplement S5.

[RESULT PENDING: primaries HIT, MISS and INCONCLUSIVE, before and after the Holm adjustment. If more than three of the eleven are INCONCLUSIVE, say first that the program is underpowered.]

**Table 2.** Results summary, one row per prediction. The primaries form one Holm family; P10b is secondary.

| prediction | hypothesis of record (abbreviated) | test | estimate [95% CI] | verdict | Holm-adjusted |
|---|---|---|---|---|---|
| P1 | race-loss share $=(N-1)/N$, $N\in\{2,3,5,10\}$, ±0.02 | band rule, Wilson | | | |
| P2 (H2a) | maker's cancel-first share $= P_{\text{win}}(\ell,\varepsilon,k)$, ±0.02 | band rule, Wilson | | | |
| P3 (H3a) | Laplace ÷ random-walk rent rate = closed form (1.30), ±10% | bootstrap ratio | | | |
| P4 (H4a) | entry stops at $N^*=5$; waste/rent $0.909\pm0.05$ | marginal-profit intervals, band rule | | | |
| P5 (H5a) | taker fee scales snipes per jump by $e^{-f_t/b}$, profit per snipe $b$, ±5% | band rule | | | |
| P6 (H6a) | participation ratio $e^{-(\bar h_{\text{HFT}}-\bar h_0)/\bar v}$ ±10%; deadweight > 0 | band rule, bootstrap | | | |
| P7 (H7a) | cancelling ÷ non-cancelling maker loss $=1-P_{\text{win}}$, ±0.03 | band rule | | | |
| P8 (H8a) | Nash half-spread non-increasing in $M$; within 20% of breakeven at $M=5$ | bootstrap, band rule | | | |
| P9 (H9a) | batch sniping loss < continuous and ≤ 1.2 × footnote-34 bound | bootstrap | | | |
| P10 | "slowest maker pays" right in ≥ 80% of held-out configurations | binomial against $1/M$ | | | |
| P10b | queue-front maker pays in ≥ 80% of held-out configurations | binomial against $1/M$ | | | — |
| P11 (H11a) | sniper-latency first-order index < 0.05; $\lambda_j$ and $b$ together ≥ 0.5 | Sobol, bootstrap | | | |

### 4.1 Validation: the BCS replication (P1)

Configured to BCS's assumptions — one liquidity provider and $N-1$ equally fast snipers, a pure-jump Laplace signal, one-unit Poisson investors, no fees, 40 seeds of 600 s per cell — the model was predicted to recover BCS's equilibrium: a race-loss share of $(N-1)/N$ within ±0.02 for $N \in \{2,3,5,10\}$ (primary), an indifference half-spread within ±3% of $h^*$ that does not trend in $N$, profit per successful snipe of $b$, and in the batch arm $(\varepsilon/\tau)(N-1)/(2(N+1))$ stale executions per snipe-able jump. A miss would indict the scheduler, not BCS.

[RESULT PENDING: P1 — verdict; race-loss share per cell (3 jump ratios × 4 values of $N$) [Wilson]; $\hat h$, $\hat h_0$, slope of $\hat h$ on $(N-1)/N$, profit per snipe [CI]; jitter and batch arms; cells re-run at 80 seeds.]

[FIGURE PENDING: Figure 1 — measured against predicted race-loss share and $\hat h/h^*$ in the twelve cells, with their bands.]

### 4.2 Validation: jumps against a random walk (P3)

At matched quadratic variation, the ratio of sniper rent rates under Laplace jumps and under a Gaussian random walk was predicted to equal its closed form within ±10%, 1.30 at the signed-off 100 updates a second (primary); profit per snipe to stay at $b$ at every spread under jumps but to fall with the spread under the random walk (H3b); and the earlier maker to show fewer tape-band maker-gains cells under jumps (H3c).

[RESULT PENDING: P3 — H3a ratio [CI] and verdict; H3b per-snipe profit by spread and slopes [CI]; H3c counts.]

### 4.3 The race and its incidence (P2, P8, P10, P10b)

**The race (P2).** The share of snipe-able jumps on which the maker's cancel arrives first was predicted to equal $P_{\text{win}}(\ell,\varepsilon,k)$ within ±0.02 for $k \in \{1,3,8,21\}$ and $\ell/\varepsilon \in \{0, 0.25, 0.5, 1.5\}$ (primary; at $n = 100$ the interval nearly fills the band, so INCONCLUSIVE was expected to be likely). With snipe size equal to quote size, total rent was predicted to grow as $k/(k+1)$ at $\ell = 0$ and to be flat beyond $\varepsilon$ (H2b); gaps not to grow linearly in $k$ (H2c); and the earlier maker to show the single-maker inversion only when slow — at most 2 of 18 tape-band cells maker-gains at 1 ms or less, at least 12 at 300 ms (H2d).

[RESULT PENDING: P2 — H2a verdict and the 16 cells' shares [Wilson]; H2b rent ratios [CI]; H2c ratio [CI]; H2d counts by maker latency, paper band with corrected band beside.]

[FIGURE PENDING: Figure 2 — the cancel-first share against $\ell/\varepsilon$ for each $k$, with the $P_{\text{win}}$ curves and bands (H2a), beside the earlier maker's maker-gains count against its latency (H2d).]

**Endogenous makers (P8).** With makers choosing their policy by best response, the Nash quoted half-spread was predicted to be non-increasing in $M \in \{1,2,3,5\}$ and within 20% of the breakeven half-spread at $M = 5$ (primary; that clause lies below the smallest half-spread the price grid allows and cannot hit); active makers to fall with a fixed cost of quoting and with $k$ (H8b); the slowest maker to have the lowest PnL delta in at least 90% of tape-band cells (H8c); and a maker lagging every sniper by more than $\varepsilon$ to stop quoting at the touch (H8d).

[RESULT PENDING: P8 — Nash half-spread by $M$ [CI] and verdict; H8b–H8d.]

**Who pays among makers (P10, P10b).** In held-out latency configurations, "the slowest maker pays" was predicted to be right in at least 80% of configurations, closely spaced sets included (primary), and on the tick model right in 60–95% overall but under 50% of closely spaced sets. P10b predicts that the maker most often at the front of the queue at the stale touch when a jump's first snipe arrives pays, in at least 80%.

[RESULT PENDING: P10 — hit rates overall and on closely spaced sets [interval]; Spearman share; tick-model hit rates.]

[RESULT PENDING: P10b — hit rate where the predictor names a payer [interval]; literal reading beside.]

### 4.4 Welfare and waste (P4, P5, P6, P7)

**The cost of speed (P4).** With a cost of speed $c$ and free entry, entry was predicted to stop at $N^* = \lfloor \lambda_i h^*/c \rfloor$, five firms at $c$ = rent/5.5, with waste $N^* c$ equal to 0.909 of the rent within ±0.05 (primary); waste over rent to follow $\lfloor R/c\rfloor c/R$ across costs with investor revenue and rent invariant (H4b); and the noise-trader toll against the earlier maker to be invariant to $c$ (H4c).

[RESULT PENDING: P4 — $N$ at which entry stops, marginal-profit intervals, waste/rent [CI]; H4b, H4c.]

**Fees (P5).** Holding quotes fixed, a taker fee $f_t$ was predicted to scale snipes per jump by $e^{-f_t/b}$ and leave net profit per snipe at $b$, within ±5% (primary); with a re-pricing provider, the all-in investor half-spread to depend on the total fee, not its split (H5b); and the earlier, non-re-pricing maker to keep at least as many maker-gains cells under taker-heavy and rebate schedules as under zero fees (H5c).

[RESULT PENDING: P5 — H5a ratios [CI]; H5b spreads by schedule; H5c counts.]

**Participation and the slow informed investor (P6).** With investors who trade only when their gain from trade exceeds the all-in half-spread, HFT presence was predicted to lower participation by $e^{-(\bar h_{\text{HFT}} - \bar h_0)/\bar v}$ within ±10% and to create a deadweight loss (primary); the slow informed investor's edge not to be positive (H6b); and the inversion to survive at participation elasticity at most 1 and fail at 2 or more (H6c).

[RESULT PENDING: P6 — participation ratio and deadweight [CI]; H6b, H6c.]

**Maker strategies (P7).** A maker cancelling on every update was predicted to lose, per snipe-able jump, the fraction $1 - P_{\text{win}}$ of an otherwise identical non-cancelling maker's loss, within ±0.03 (primary); inventory skew to raise sniping loss by $E[\cosh(\delta/b)]$ (H7b); ladder losses to fall with depth as $e^{-(h_j - h_1)/b}$ (H7c); an Avellaneda–Stoikov maker to show no inversion (H7d); and quote fading to reproduce the earlier leg pattern (H7e).

[RESULT PENDING: P7 — H7a ratio [CI] by cell; H7b–H7e.]

### 4.5 Market design: frequent batch auctions (P9)

For intervals $\tau \in \{1, 10, 100\}$ ms and a maker within $\varepsilon$ of the snipers, the maker's sniping loss was predicted to be below continuous trading's at every interval and within 1.2 times BCS's footnote-34 bound $\lambda_j(\varepsilon/2\tau)\,\text{prize}(h)$ (primary); price-competitive snipers to clear at the stale quote under the engine's reference-first rule but near the new value under the pressure rule (H9b); no interval, even one shorter than the maker's latency, to leave the maker significantly worse off than continuous trading (H9c); and within-batch rationing not to change the maker's loss (H9d). The verdicts read the engine's uncross as it is (§3.7).

[RESULT PENDING: P9 — loss by $\tau$ [CI] against continuous and the bound; H9b–H9d, H9c's strict and lenient readings.]

[FIGURE PENDING: Figure 3 — the maker's sniping loss against $\tau$, batch and continuous, with the footnote-34 bound.]

### 4.6 Sensitivity (P11)

Over Sobol indices from 512 Saltelli base samples across the hand-set parameters, the sniper latency level, with the maker's lag fixed, was predicted to have a first-order index below 0.05 for total sniper rent, while the jump rate and jump scale carry at least half its variance (primary; BCS Prop. 5); adverse sensitivity and the snipe-to-quote ratio to rank among the top three total-order indices for the maker's PnL delta (H11b); and the noise-trader toll from sniping to lie within ±10% of mean sniper rent (H11c, as amended).

[RESULT PENDING: P11 — indices with bootstrap intervals at $N = 512$ (of record), $N = 2{,}048$ beside; H11b, H11c.]

## 5. Differential testing against the verified engine

Model checking certifies the matching design at bounded scope (§3.1). It says nothing about code that re-implements part of that design, and the harness contained such code. Testing it against the engine found a divergence that had moved published results.

**The test.** Until the bridge exposed the engine's call auction, the batch arm of the tick model's Experiment 4 (Supplement S5) cleared through a Python re-implementation of the clearing rule (`discover_uncross_price`, `simulation/batch_auction.py`), written from the engine's `OrderBook::discoverUncrossPrice` by reading the C++ source line by line. Once the bridge exposed `uncross` and the session-state controls (Supplement S3.3), both implementations could clear the same book. They agree on executable volume but not always on price. On a book with bids 102×50 and 101×30 against asks 99×40 and 100×60, eighty units clear at either 100 or 101 with equal imbalance; the Python rule clears at 100 and the engine at 101. Both apply the same cascade — maximise executable volume, minimise imbalance, minimise distance to a reference price, then market pressure — and on this book they part at the reference step. The engine anchors its reference on the first order's price before any trade (102 here) and picks 101; the Python rule has no reference until its first clearing, falls through to market pressure, and a sell surplus clears lower. A volume-based check cannot see the difference, and every metric that depends on execution price can. The case is pinned in `tests/test_batch_auction.py::test_python_matcher_divergence_pinned`, and the Python rule is kept only as a cross-check.

**The divergence moved published results, through that code path only.** Experiment 4's 50,000-tick grid has seven batch arms and one continuous arm, and the continuous arm never calls `uncross`. We re-ran the grid with batch clearing moved from the Python rule to the engine, holding seeds, fundamental paths, run length and every parameter fixed.

**Table 3.** Tick-model Experiment 4 at 50,000 ticks ($n=100$), before and after batch clearing moved to the engine. Point estimates in the tick model's simulation currency (Supplement S5.1); \* marks a 95% bootstrap interval that excludes zero.

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

**The tie-break is an economic assumption.** The engine's rule is correct relative to its own specification, not relative to any venue, and venues order these steps differently, so the port is also a comparison of two plausible tie-break rules. On the 3,000-tick grid the rule changes no batch cell's significance verdict for either the maker's delta or HFT rent. On the 50,000-tick grid it changes most of the batch arm: under the Python rule the maker loses at every interval from 5 to 100 ticks, recovering 40–68% of its continuous-clearing loss at intervals 5–25, while under the engine's rule it breaks even or gains, significantly at intervals 10 and 25. What holds under both rules is the comparison with continuous clearing: at every interval of five ticks or more, the maker does better batched than continuous. How much better, whether it ends up ahead, and whether batching removes the rent all depend on the tie-break. The tick batch arm had two further defects, unrelated to the tie-break: it gave a window's IOCs lower time priority than limits that arrived after them, and let same-price IOCs fill in submission order, so a latency race survived inside the batch (Supplement S5.2). The event model's batch (§3.7) replaces it.

The episode shows that a re-implementation checked against source code can still differ from the code it copies the first time both run on the same input. It does not show that the engine is correct; it shows that running against the engine caught an error that reading its source did not.

## 6. Discussion

### 6.1 Implications for market design

[TODO after results: implications, each tied to the prediction it rests on. The pre-registration makes four testable: whether which maker pays is predictable from latency rank or from queue position (P10, P10b); whether free entry turns the transfer into waste (P4); whether the split of fees between maker and taker matters to a maker that re-prices (P5); and what the engine's reference-first uncross does to BCS's batch-auction remedy (P9).]

### 6.2 Limitations

**The jump law has the right rate near the fee but the wrong shape.** The tape's race proxy counts 0.29–1.39 races a second against about 0.02 snipes a second in the facts pilot, but that gap is the proxy's: at moves that clear the fee the tape races 0.42–0.48 times as often as the pilot's snipers fill (exploratory, §3.9). What remains is the jump law's shape. On a 10 ms grid the Laplace fit gives about twice the tape's rate of fee-clearing moves (0.0178 against 0.0087 a second) and about a sixth of its rate of moves of 5 bp or more (0.0002 against 0.0010), so matching the rate near the fee does not validate rent per snipe, which depends on how far jumps overshoot it. Over one second the tape clears the fee 1.67 times as often as the model, through slower moves a jump-diffusion lacks; whether they can be sniped depends on how long quotes stay stale. And the tape still shows neither participants nor failed orders: its upper bound counts one taker's sweep as a race, its lower bound misses same-millisecond races, and part of each measured move is the race's own impact (Supplement Tables S12–S13).

**The engine's limits.** The book is a fixed window of 200,001 price ticks centred on the first price it sees, with no re-centring; an order outside it is refused after admission, and the scheduler stops the run rather than continue with a maker missing from the book. At the operating point the window spans ±16% of $v_0$, far beyond a 30-minute path's range, but the replication's pure-jump path at BCS's scale would leave it, so P1 reverses a jump's sign when that keeps $V$ within 90,000 ticks of its start, which keeps jump times and sizes and changes only the level path (`experiments/exp9_bcs_replication.py`). The uncross's reference price is the session's first order and is never updated, so with two or more snipers in a batch the engine clears a stale quote at the stale price where BCS's Bertrand outcome clears at the new value (§3.7). P9's verdicts read the engine as it is, with the reference-free "pressure" rule reported beside. And because the engine stamps every uncross trade with a buy aggressor, the batch arm's fee incidence — the later arrival pays the taker fee — is a modelling choice.

**Order flow is simpler than the tape's.** Investors arrive as a Poisson process, so trade counts do not cluster (Fano factor 0.79 against the tape's 125, Table 1). No investor splits a parent order at the operating point, so trade signs carry no memory; the slow informed investor's parent order is the model's only metaorder, and only P6 includes it. Makers quote one level by default against the tape's 20-level book, and only P7's ladder quotes more. Agents read the current book without delay, so only the signal's latency races.

**Not a replica.** The model takes its primitives from the tape, but its strategic structure — sniper and maker latencies, the jitter, the number of racing firms and how makers quote — is designed and swept, because a public tape records neither failed orders nor who sent what (Aquilina, Budish, and O'Neill, 2022). The snipe-to-quote ratio shows the gap: the tape measures a race against aggregate displayed depth, the model against the quotes of the makers it contains, and how many makers rest at the touch no public feed reports. One asset on one venue is studied, and every verdict in §4 is a statement about this mechanism at this operating point.

### 6.3 Future work

Three extensions follow from §6.2. Order flow with clustering and metaorders — a self-exciting arrival process and split parent orders — would test whether the verdicts depend on Poisson flow. A fee schedule for the fastest firms, rather than the public tier, would locate the model's rents against what those firms pay. And an engine whose book re-centres and whose uncross references the last print, with an option to skip the reference step, would let the batch arm run BCS's clearing rule natively rather than through a reprice (`paper/batch_interval1_note.md`). Beyond these, one asset on one venue leaves cross-venue latency arbitrage, where snipers compete across books, untouched.

## 7. Conclusion

[TODO after results: the main result and the verdicts it rests on.]

The results rest on a matching engine whose design is model-checked at bounded scope (§3.1), on accounting whose two identities hold to rounding (§3.6), on a simulator required to recover BCS's equilibrium before anything else (P1), on predictions fixed before the runs (§3.11), and on a differential test that caught, and removed, a clearing-rule divergence in the harness (§5).

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

Holm, S. (1979). A Simple Sequentially Rejective Multiple Test Procedure. *Scandinavian Journal of Statistics*, 6(2), 65–70.

Hoffmann, P. (2014). A Dynamic Limit Order Market with Fast and Slow Traders. *Journal of Financial Economics*, 113(1), 156–169.

LeBaron, B. (2006). Agent-Based Computational Finance. In *Handbook of Computational Economics*, Vol. 2 (pp. 1187–1233).

Lee, S. S., & Mykland, P. A. (2008). Jumps in Financial Markets: A New Nonparametric Test and Jump Dynamics. *Review of Financial Studies*, 21(6), 2535–2563.

Menkveld, A. J. (2013). High Frequency Trading and the New Market Makers. *Journal of Financial Markets*, 16(4), 712–740.

Menkveld, A. J. (2016). The Economics of High-Frequency Trading: Taking Stock. *Annual Review of Financial Economics*, 8, 1–24.

Menkveld, A. J., & Zoican, M. A. (2017). Need for Speed? Exchange Latency and Liquidity. *Review of Financial Studies*, 30(4), 1188–1228.

Shkilko, A., & Sokolov, K. (2020). Every Cloud Has a Silver Lining: Fast Trading, Microwave Connectivity, and Trading Costs. *Journal of Finance*, 75(6), 2899–2927.

Wah, E., & Wellman, M. P. (2013). Latency Arbitrage, Market Fragmentation, and Efficiency: A Tactical Market Model. In *Proceedings of the 14th ACM Conference on Electronic Commerce (EC)*, 855–872.
