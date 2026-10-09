# Replicating BCS's closed-form equilibrium in the event-driven simulator

**Purpose.** Before the event model says anything Budish, Cramton & Shim
(2015, QJE 130(4); "BCS") did not, it must recover what they did say. This
spec sets the simulator to BCS's assumptions, names what to measure, gives the
theory's numbers at the chosen parameters, and fixes pass/fail tolerances. It
also lists every place the simulator's mechanism necessarily differs from
BCS's and what each difference should do to the comparison. Predictions are
pre-registered as P1 in `paper/PREREGISTRATION.md`; the closed forms are in
`analysis/bcs_theory.py` (`python analysis/bcs_theory.py` prints §3's table).

## 1. The model being replicated (BCS §VI.A–B)

- A public signal y jumps as a compound Poisson process at rate λ_j; jumps are
  symmetric, J = |jump|. We use Laplace(0, b) jumps, so J ~ Exp(mean b).
- Investors arrive at Poisson rate λ_i, buy or sell one unit with equal
  probability, at market, immediately.
- N equally fast trading firms: one liquidity provider (LP) quoting one unit at
  y ± h (BCS's spread s = 2h), and N − 1 stale-quote snipers.
- On a jump the LP cancels and re-quotes around the new y while every sniper
  tries to trade at the stale quote; messages are processed serially in random
  order, so the LP is sniped with probability (N − 1)/N when J > h.
- Equilibrium (eq. 3): λ_i·h* = λ_j·Pr(J > h*)·E[J − h* | J > h*], independent
  of N. For Laplace jumps h* = b·W(λ_j/λ_i), W the principal Lambert W
  (derived in `laplace_half_spread`).

N in this spec always counts the LP: N = 2 is one LP and one sniper.

## 2. Simulator configuration

The mapping is onto `simulation/contracts.py`. Time is integer nanoseconds;
prices are engine fixed-point ticks (`PRICE_PRECISION` = 10,000, so 1,000
ticks = 0.10).

### 2.1 Fundamental path (`FundamentalPath`)

| Setting | Value | Why |
|---|---|---|
| Diffusion σ | 0 | BCS's y is a pure jump process; any diffusion adds races BCS's eq. 3 does not count |
| Jump rate λ_j | 2.5, 10, 40 per s (§3) | spans λ_j/λ_i = 0.25, 1, 4 |
| Jump law | signed Laplace(0, b), b = 1,000 ticks | |J| ~ Exp(b): memoryless, so profit per successful snipe is b exactly |
| V₀ | 10,000,000 ticks (1,000.00) | V wanders: after 24,000 jumps (λ_j = 40/s, 600 s) its s.d. is √(2·24,000)·b ≈ 219,000 ticks, 2% of V₀; at V₀ = 100.00 it would be 22% and a path could approach zero |
| `updates()` | exactly one `SignalUpdate(is_jump=True)` per jump, no others | |
| Duration | 600 s per seed | §5 sizing |

If the jump-diffusion lane parameterises Laplace jumps by their standard
deviation, b = σ_J/√2.

### 2.2 Agents (`EventAgent`)

All trading firms and investors: `md_latency_ns = order_latency_ns = 10_000`
(10 µs). No fees. One unit everywhere.

**Liquidity provider** (one).
- `on_start`: GTC limit bid at round(V₀ − h), ask at round(V₀ + h), qty 1.
- `on_signal(update)`: in this order — `Cancel` both live quotes, then new bid
  and ask at round(V_new ± h). The cancels must reach the engine before the
  new quotes (§6, item 4).
- `on_fill(fill)`: if `fill.is_maker` and the order is still the LP's *live*
  quote, replace it at the same price (BCS: "replaces the accepted ask with a
  new ask at this same value"). A fill on a quote already replaced — a snipe
  that beat the cancel — is ignored; re-posting it would re-post a stale quote.
- h is set per run from the grid of §4.

**Snipers** (N − 1).
- `on_signal(update)`: read the book through `BookView` (the stale quote is
  still there: the LP's cancel is in flight). If `update.value > best_ask()`,
  send IOC buy 1 at `best_ask()`; if `update.value < best_bid()`, IOC sell 1 at
  `best_bid()`. Never a market order: a sniper that loses the race must miss,
  not lift the LP's new quote.
- No other activity.

**Investors** (one agent, or one per side).
- Self-scheduled `Wakeup`s with exponential inter-arrival times, rate λ_i =
  10/s, side 50/50, a seeded RNG of their own.
- Each arrival: a market order (or an IOC priced ten b through the touch) for
  1 unit.

### 2.3 Scheduler

- **Primary arm:** zero race jitter; equal arrival times are broken by the
  scheduler's seeded uniform permutation (contracts: "never by registration
  order"). This is BCS's random serial processing.
- **Jitter arm:** i.i.d. U[0, ε] per callback, ε = 1 µs (BCS footnote 21: the
  same (N − 1)/N).
- Seeds: the fundamental path, the investor stream and the tie-break/jitter
  stream each get their own seed, derived from the run seed, so that runs at
  different h share the same jumps and investors (common random numbers).

### 2.4 Accounting

PnL is per-fill edge against V at execution time — for a buy of q at p at
time t, `q·(path.value_at(t) − p)`, sign reversed for a sale. That is BCS's
payoff (x liquidates costlessly at y). Do **not** use the paper's end-of-run
mark-to-market: it adds inventory × (V_T − V_trade), which has mean zero here
but a variance that would swamp §5's tolerances.

## 3. The theory's numbers

λ_i = 10/s, b = 1,000 ticks. h* and h₀ in ticks; rates in ticks per second;
per-jump figures are per jump of any size.

| λ_j/λ_i | N | h* | h₀(N) | P(J > h*) | race loss (N−1)/N | LP loss per jump | sniper take rate | LP profit rate | stale-volume share |
|---|---|---|---|---|---|---|---|---|---|
| 0.25 | 2 | 203.9 | 111.8 | 0.8156 | 0.5 | 407.8 | 1,019 | 1,019 | 0.0925 |
| 0.25 | 3 | 203.9 | 144.3 | 0.8156 | 0.6667 | 543.7 | 1,359 | 679.6 | 0.1197 |
| 0.25 | 5 | 203.9 | 168.9 | 0.8156 | 0.8 | 652.4 | 1,631 | 407.8 | 0.1402 |
| 0.25 | 10 | 203.9 | 186.7 | 0.8156 | 0.9 | 734.0 | 1,835 | 203.9 | 0.1550 |
| 1 | 2 | 567.1 | 351.7 | 0.5671 | 0.5 | 283.6 | 2,836 | 2,836 | 0.2209 |
| 1 | 3 | 567.1 | 432.6 | 0.5671 | 0.6667 | 378.1 | 3,781 | 1,890 | 0.2744 |
| 1 | 5 | 567.1 | 490.1 | 0.5671 | 0.8 | 453.7 | 4,537 | 1,134 | 0.3121 |
| 1 | 10 | 567.1 | 529.8 | 0.5671 | 0.9 | 510.4 | 5,104 | 567.1 | 0.3379 |
| 4 | 2 | 1,202 | 852.6 | 0.3005 | 0.5 | 150.3 | 6,011 | 6,011 | 0.3754 |
| 4 | 3 | 1,202 | 990.4 | 0.3005 | 0.6667 | 200.4 | 8,014 | 4,007 | 0.4449 |
| 4 | 5 | 1,202 | 1,083 | 0.3005 | 0.8 | 240.4 | 9,617 | 2,404 | 0.4902 |
| 4 | 10 | 1,202 | 1,145 | 0.3005 | 0.9 | 270.5 | 10,820 | 1,202 | 0.5197 |

- h* = b·W(λ_j/λ_i) is the same in every row of a block: eq. 3 has no N.
- h₀(N) = b·W(((N − 1)/N)·λ_j/λ_i) is the LP's breakeven with no outside
  option (λ_i·h = λ_j·prize(h)·(N − 1)/N). It is *not* BCS's spread: at h* the
  LP earns exactly what a sniper earns (eqs. 1 = 2), its opportunity cost,
  which is why BCS's s* exceeds 2h₀. The replication measures both.
- Profit per successful snipe is b = 1,000 in every cell (memorylessness).
- Sniper take rate = λ_j·b·e^(−h/b)·(N − 1)/N is what all snipers together
  book; BCS's "total rents" (Prop 1(v)) are λ_i·h* and include the LP's 1/N.
- Stale-volume share is BCS §VI.E.3's share of trades at stale quotes.

At a grid point h ≠ h*, the curves the simulator must trace are
π_LP(h) = λ_i·h − λ_j·b·e^(−h/b)·(N − 1)/N and
D(h) = π_LP(h) − π̄_S(h) = λ_i·h − λ_j·b·e^(−h/b).

### 3.1 Batch-auction arm (BCS §VII)

Only if the event model exposes a batch mode (contracts.py does not yet say;
see §7). Batch interval τ = 1 ms (λ_i·τ = 0.01, so a batch almost never holds
two investors and one-unit depth suffices), N ∈ {2, 5}, LP quoting h*.

- ε = 0: the LP's cancel always makes the batch its jump falls in; Prop 7 —
  zero stale-quote executions, zero sniper PnL.
- ε = 10 µs: the LP's stale quote meets a sniper in the batch with probability
  φ = (ε/τ)(N − 1)/(2(N + 1)) per jump (`fba_jitter_exposure`; BCS footnote
  34's ε/(2τ) refined to require a sniper to make the batch — derived), and is
  sniped when also J > h. Stale executions per jump φ·e^(−h*/b):

| λ_j/λ_i | N | φ | stale executions per jump | per 600 s seed | sniper take rate | vs continuous |
|---|---|---|---|---|---|---|
| 0.25 | 2 | 0.001667 | 0.001359 | 2.0 | 3.4 | 1,019 |
| 0.25 | 5 | 0.003333 | 0.002719 | 4.1 | 6.8 | 1,631 |
| 1 | 2 | 0.001667 | 0.000945 | 5.7 | 9.45 | 2,836 |
| 1 | 5 | 0.003333 | 0.001890 | 11.3 | 18.9 | 4,537 |
| 4 | 2 | 0.001667 | 0.000501 | 12.0 | 20.0 | 6,011 |
| 4 | 5 | 0.003333 | 0.001002 | 24.0 | 40.1 | 9,617 |

Requirements for the arm to test what it claims: snipers must submit orders
that rest until the next uncross (an IOC the engine kills on entry in an
auction state would zero sniping for the wrong reason); a cancel received
during the batch must remove the order before the uncross (BCS: "canceled
quotes are not even entered into the next batch auction").

## 4. What to measure

From fills, the path and the LP's own quote record — no scheduler internals
needed. A jump is **snipe-able** if V_new is above the LP's ask (up-jump) or
below its bid (down-jump) as they stood at the jump.

1. **Race-loss share** — snipe-able jumps on which a sniper filled the stale
   quote ÷ snipe-able jumps. Theory (N − 1)/N.
2. **Profit per successful snipe** — mean edge of sniper fills. Theory b.
3. **Sniper take rate** — Σ sniper edge / T. Theory: §3 table at h*.
4. **LP profit curve π_LP(h)** and **indifference curve D(h)** = π_LP(h) −
   mean sniper profit rate, at each grid point.
5. **Empirical indifference spread ĥ** — the root of D(h). Theory h*.
6. **Empirical zero-profit spread ĥ₀** — the root of π_LP(h). Theory h₀(N).
7. **Stale-volume share** at h*: sniper fills ÷ all fills.
8. Sanity: investor fills / (λ_i·T) ≥ 0.999; zero-sum (Σ edges over all
   agents = 0 to machine precision).

**Finding the zero-profit spreads.** "Iterate the LP's spread until its
expected profit is zero" is done on a fixed grid with common random numbers,
so every point of the curve is a check, not only the root:

- grid for ĥ: h*·{0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.15}, rounded to ticks;
- grid for ĥ₀: h₀(N)·{0.85, …, 1.15};
- per seed, the root is the linear interpolation of D (or π_LP) between the
  two adjacent grid points where it changes sign (extrapolate from the end
  segment if a seed has no sign change — report how many);
- ĥ is the mean over seeds, its interval the §5 bootstrap over seeds.

Bisection on the seed-mean curve with the seed set held fixed is an acceptable
equivalent provided it is run to a bracket of ≤ 0.5% of h*.

## 5. Sample size and pass/fail

Per cell: **40 seeds × 600 s**, for each of 12 cells (3 ratios × 4 N) and
each of 14 grid points: 6,720 runs, plus the jitter arm at h* and the batch
arm. An idealised BCS Monte Carlo (the model
of §1 with no engine) gives a per-seed standard deviation of ĥ of 5.1%, 2.3%
and 1.3% of h* at λ_j/λ_i = 0.25, 1, 4 (N = 2; smaller at N = 10), so 40
seeds put the 95% half-width under 1.6% in the worst cell; race-loss
intervals are about ±0.005 (≈ 49,000 snipe-able jumps per cell at ratio 0.25).

Intervals are 95% percentile bootstraps over seeds (2,000 resamples, seed 0,
`metrics/bootstrap.py`), Wilson for proportions. Each quantity has a
tolerance band around its theory value and is classified as:

- **PASS** — the 95% interval lies inside the band;
- **FAIL** — the interval is disjoint from the band;
- **INCONCLUSIVE** — otherwise; re-run that cell once at 80 seeds, then
  classify again, reporting both.

Classifying by "interval inside the band" rather than "interval contains the
theory value" is deliberate: across 12 cells the latter would fail a correct
simulator about half the time by chance.

| Quantity | Theory | Band | Primary? |
|---|---|---|---|
| Race-loss share | (N − 1)/N | ± 0.02 absolute | yes |
| ĥ (indifference) | h* | ± 3% | yes |
| ĥ independent of N | slope of ĥ on (N − 1)/N = 0 | interval inside ± 3% of h* | yes |
| ĥ₀ (zero profit) | h₀(N) | ± 3% | |
| Profit per successful snipe | b | ± 3% | |
| Sniper take rate at h* | §3 | ± 5% | |
| D(h), π_LP(h) at each grid point | §3 curves | ± 5% of λ_i·h* | |
| Stale-volume share at h* | §3 | ± 0.02 absolute | |
| Jitter arm race-loss share | (N − 1)/N | ± 0.02 absolute | |
| Batch arm, ε = 0 | 0 stale executions | exactly 0 | |
| Batch arm, ε = 10 µs | §3.1 executions per jump | ± 25% | |

The replication **passes** if every primary row passes in every cell. A
failure is reported with the §6 item that explains it, or as unexplained.

Tolerances are wider than sampling noise on purpose: §6 lists deviations that
can move the numbers by a fraction of a percent, and the bands absorb those
while still catching a scheduler that, say, orders ties by registration (race
loss 1 or 0, not (N − 1)/N) or a sniper that uses market orders (after losing
a race it lifts the LP's new quote and loses h there).

## 6. Where the simulator's mechanism differs from BCS's

Each item states the difference and what it should do to the comparison at
§2's settings.

1. **Integer prices.** Quotes sit at round(V ± h), so the LP's effective
   half-spread differs from h by up to 0.5 tick, symmetrically. The Laplace
   prize changes by the factor E[e^(−δ/b)] ≈ 1 + (1/24)(1/b)² — about 4×10⁻⁸
   at b = 1,000. Negligible. (BCS footnote 19: on a discrete grid the LP role
   becomes strictly preferred and firms race for it; roles here are assigned,
   so that channel is off.)
2. **Discrete units.** BCS trade one unit, and so does every agent here, so
   the units match exactly. Quoting q > 1 against one-unit snipers would
   change the economics to BCS eq. 4 (snipers take all stale depth) and is
   out of scope.
3. **Engine price-time priority.** With one resting order per side there is
   no queue, so priority is inert. It matters as soon as two makers share a
   price, where queue position, not just speed, decides who is sniped. That
   is a mechanism BCS do not have, so multi-maker results cannot be
   compared with BCS's numbers.
4. **Tie-breaks and jitter.** BCS process simultaneous messages in uniformly
   random order. The scheduler's seeded permutation of exact ties reproduces
   that, and so does i.i.d. jitter (footnote 21). Two ways it can differ: (a) a
   non-uniform permutation, which would show up directly in the race-loss
   share; (b) permuting individual *actions* rather than agents' callbacks,
   which could put the LP's new bid ahead of its own cancel — after a jump
   larger than 2h the new bid crosses the stale ask and the engine's
   self-trade prevention, not the race, decides the outcome. The spec requires
   each callback's actions to arrive in order (§7).
5. **A continuous order book, not one-unit quotes.** The engine keeps a real
   book, so there are windows BCS's zero-latency model makes measure-zero:
   - after a snipe or an investor fill, that side is empty for md + order
     latency (20 µs) until the LP's refill lands, so an investor arriving then
     on that side misses: a few ×10⁻⁴ of investors at these rates, inside the
     0.999 sanity line;
   - an investor arriving between a jump and the LP's re-quote trades at a
     stale quote; buy and sell are equally likely, so the LP's expected edge
     on it is still h and the bias is zero, at a variance cost of order 10⁻⁴;
   - two jumps within one latency window (≈ λ_j·20 µs ≤ 8×10⁻⁴ of jumps at
     λ_j = 40/s) create overlapping races that BCS never see.
   Together these move π_LP and ĥ by well under 0.1%.
6. **Positive latency and round trips.** BCS's firms react in zero time; here
   every reaction takes md + order latency. With all firms equal this shifts
   every race by the same 20 µs and changes nothing above except item 5's
   windows. It matters as soon as latencies differ (P2 in the
   pre-registration), where `race_win_probability` gives the race outcome.
7. **Refill timing.** BCS's LP refills an investor-filled quote instantly; here
   it takes 20 µs. Covered by item 5.
8. **Unbounded jumps.** BCS assume bounded jump support; Laplace is
   unbounded. Equation 3 still has a unique root (see `bcs_theory` module
   docstring); the only practical risk, a path wandering to zero price, is
   removed by §2.1's V₀ (the path's s.d. over a run is 2% of V₀).
9. **Accounting.** BCS value every trade at y at that instant. §2.4 does the
   same; the paper's end-of-run mark would add zero-mean noise only.
10. **Batch clearing (arm 3.1 only).** Three differences, none of which bite
    in the stale-price-sniper configuration specified here, all of which bite
    in P9 of the pre-registration:
    - the engine's uncross ranks prices by volume, then imbalance, then
      distance to the reference (last clearing) price, then pressure. BCS clear
      where excess demand changes sign. With one stale ask at A and two
      snipers bidding V′ > A, every price in [A, V′] ties on volume and
      imbalance, so the engine clears at A while BCS clear at V′ (Prop 6(iii)'s
      Bertrand argument). Stale-price snipers bid A, the only crossing price,
      so the rules agree here;
    - the engine rations by arrival time inside the batch; BCS give equal
      priority to orders from the same interval and ration randomly. That
      changes which sniper is filled, not whether the LP is;
    - BCS hide in-batch activity until the auction; `BookView` reads the
      current book. Stale-price snipers only read the LP's resting quote,
      which is public in both.

## 7. Requests to the scheduler and contract lanes

- **Within-callback order.** Exact-tie permutation and jitter should apply per
  agent callback, keeping that callback's actions in the order returned (item
  4). The contract says "a seeded random permutation" without saying of what.
- **Jitter law.** i.i.d. U[0, ε] per callback, ε configurable and 0 allowed.
- **Batch mode.** §3.1 needs one, with resting-until-uncross semantics for
  orders entered during a batch and cancels applied before the uncross.
- **Seed streams.** Separate seeds for path, investors and tie-breaks (§2.3),
  so that common random numbers survive a change of h.
