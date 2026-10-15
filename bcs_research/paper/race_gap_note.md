# The tape-versus-model race gap: the proxy counts races after sub-fee moves

**Exploratory.** This analysis was not pre-registered and does not test any
pre-registered prediction. The move-size thresholds were set before the run.
Code: `calibration/race_by_move_size.py`. Numbers:
`results/calibration/race_by_move_size_v2.json`. Intervals are 95%
percentile intervals from a day-block bootstrap (27 days, 2,000 draws).

## Verdict: a proxy artefact. The 15–70× gap disappears at the fee

The race proxy reports 0.29–1.39 races a second, against the event model's
~0.02 snipes a second. That gap comes from what the proxy counts, not from
the market. The proxy triggers on any price move, and 99% of its
same-millisecond races follow moves below the 1.7 bp taker fee (97% of its
two-stamp lower-bound races). 91% follow moves below 0.5 bp (77% at the lower
bound). Keep only the races whose move clears the fee plus a half-spread, and
the tape's rate falls to:

- **0.0096 [0.0070, 0.0126]/s** at the upper bound (same millisecond), which
  is **0.48× [0.35, 0.63]** the model's rate;
- **0.0083 [0.0064, 0.0105]/s** at the lower bound (≥ 2 stamps within 1 ms),
  which is **0.42× [0.32, 0.52]** the model's rate.

At fee-clearing moves, then, the tape races about half as often as the model
snipes, not 15–70 times as often. Fee-clearing price moves on the jump fit's
grids tell the same story, whether or not a race followed. The comparison
that matters is the model as run, which uses only the 10 ms jump fit
(0.200/s, b = 0.705 bp, σ = 0.242 bp/√s), measured over the same horizons:

| horizon | 10 ms | 100 ms | 1 s |
|---|---|---|---|
| tape moves ≥ fee + half-spread, /s | 0.0087 [0.0065, 0.0112] | 0.0142 [0.0103, 0.0190] | 0.0321 [0.0240, 0.0411] |
| model as run (10 ms fit), /s | 0.0178 | 0.0180 | 0.0192 |
| tape ÷ model as run | 0.49 [0.36, 0.63] | 0.79 [0.57, 1.06] | 1.67 [1.26, 2.14] |
| λ_j·e^(−h/b) from that horizon's own fit, /s | 0.0178 | 0.0118 | 0.0026 |

The last row is the brief's 0.003–0.018/s. At 1 s it understates the model as
run, because the 1 s fit files most fee-sized moves under diffusion. Most
fee-clearing moves draw a visible race: 79% [76, 81] of them within the same
millisecond and 86% [85, 88] within 1 ms.

## By threshold (upper bound at W = 0, lower bound at W = 1 ms, the proxy's own pair)

| move ≥ (bp) | races/s, upper bound | races/s, lower bound | share of volume in races, upper / lower (%) |
|---|---|---|---|
| any | 1.386 [1.105, 1.698] | 0.291 [0.228, 0.359] | 41.0 / 26.2 |
| 0.1 | 0.903 [0.706, 1.125] | 0.246 [0.193, 0.307] | 34.5 / 25.2 |
| 0.25 | 0.407 [0.311, 0.514] | 0.159 [0.123, 0.200] | 28.5 / 23.4 |
| 0.5 | 0.126 [0.095, 0.163] | 0.066 [0.050, 0.084] | 22.0 / 20.7 |
| 1.0 | 0.029 [0.022, 0.037] | 0.020 [0.015, 0.025] | 13.7 / 17.1 |
| 1.7 | 0.0097 [0.0071, 0.0128] | 0.0084 [0.0064, 0.0106] | 6.1 / 13.5 |
| 2.5 | 0.0042 [0.0029, 0.0057] | 0.0045 [0.0034, 0.0057] | 1.8 / 10.4 |
| 5.0 | 0.0013 [0.0008, 0.0018] | 0.0012 [0.0009, 0.0016] | 0.4 / 5.1 |

The "any" row reproduces `race_proxy_v2.json` exactly. A race's move runs
from the implied mid before its first millisecond to the furthest implied mid
its own records reach. It is not the trigger's single-record step: a sweep
prints one record per price level, so thresholding single steps would find
almost no fee-clearing races.

## What this does not settle

1. **The shape of the jump law.** At 10 ms the model supplies 2× the tape's
   fee-clearing jumps but only about a sixth of its 5 bp jumps (0.0002
   against 0.0010/s). A single exponential tail cannot do both: the scale
   fitted to exceedances just above the detection threshold is 0.31 bp,
   steeper than 0.705, which would predict even fewer 5 bp jumps, yet the
   tape has more. The tape's jump sizes are steep near the threshold and
   heavy in the far tail — not Laplace at all, as the fit's Kolmogorov–
   Smirnov distance of 0.27 already says. Matching rates near the fee
   does not validate rent per snipe, which depends on how far jumps
   overshoot the fee.
2. **Slow fee-sized moves.** Over 1 s the tape clears the fee 1.7× as often
   as the model, because the tape's volatility rises with horizon and a
   jump diffusion's does not (`jumps.py`). Whether these moves are
   snipe-able depends on how long quotes stay stale. The fee-clearing race
   rate at W = 100 ms is 0.031 [0.023, 0.041]/s (1.6× the model), but
   clusters merged at that width chain into drifts. Those rows bound the
   rate; they do not measure jumps.
3. **The tape's blind spots remain.** The upper bound counts a single taker's
   sweep as a race. The lower bound misses same-millisecond races. Neither
   sees participants or failed orders, and part of each move is the race's
   own price impact. The two bounds agree within 15% at the fee, which
   narrows the range but removes none of these limits. The sample is one
   venue and one instrument over 27 days.

## Suggested limitations text

> The tape's race proxy counts 0.29–1.39 races per second, 15–70× the
> model's snipe rate, but it triggers on any price move: 97–99% of its races
> follow moves below the 1.7 bp taker fee. Restricted to moves that clear the
> fee and half-spread, the tape shows 0.008–0.010 races per second
> (0.42–0.48× the model's rate; day-block 95% intervals span 0.32–0.63×).
> Fee-clearing moves arrive at 0.49–1.67× the model's rate between 10 ms and
> 1 s. The model therefore does not under-supply races at the fee. Its
> Laplace jump law does misplace the tail, with about 2× too many fee-sized
> 10 ms jumps and too few beyond 5 bp, and it lacks the tape's slower
> fee-sized moves (exploratory; not pre-registered).
