# The liquidity-gap channel: an artefact of when the tick model samples the book

## Verdict: artefact. Delete the gap-growth result (S1.2) as a market finding

The rise of liquidity gaps with the number of HFTs k (2.84 at k = 0 to 47.47 at
k = 21, slope 2.13) is not something the market does. It comes from how the
tick scheduler samples the book, combined with the HFTs' jitter draw. The
mechanism is identified, a 1 µs change that leaves the race untouched removes
the growth entirely, and on the event clock gaps are flat in k, as P2's H2c
predicted.

1. **Mechanism.** The tick scheduler snapshots once per tick, after everything
   due that tick (`scheduler.py`, `run`). Inside tick s, the steps run in this
   order:
   - last tick's HFT IOCs (stamped s − 1000 + j, jitter j ≥ 1);
   - the maker's cancel and requote (stamped s);
   - this tick's noise IOCs, and any HFT IOC with jitter exactly 0.

   An HFT stamps its IOC `t + int(U(0, 100))` µs, so it runs in its own
   decision tick only when it draws j = 0 (p = 1/100). A side emptied at the
   start of a tick is refilled by that tick's requote before the snapshot. A
   side emptied by a zero-jitter IOC *after* the requote is still empty when
   the snapshot is taken. Per snipe-able tick, some sniper draws zero with
   probability 1 − 0.99^k ≈ k/100, which is linear in k. One sniper still wins
   each opportunity, so fills stay flat.
2. **Attribution (T1).** Excess gaps over k = 0 track 1 − 0.99^k with
   correlation 0.9996 (scale 238 snipe-able ticks per run). At k = 21, 44.6 of
   the 47.5 gaps are one-sided snapshots emptied by a zero-jitter HFT IOC;
   noise-made ones stay at ≈ 1.9 for every k, and the maker makes none.
3. **The targeted test (T2), predicted before it ran.** We stamp every HFT IOC
   1 µs later (jitter U{1..100}). The draws and the order among snipers are
   unchanged, but no IOC can now run in its decision tick. The gap slope falls
   from 2.13 [2.07, 2.20] to 0.00 [0.00, 0.00]: 2.91 gaps at every k from 1
   to 21, against 2.84 with no HFT. Prediction confirmed.
4. **On the submission clock (T3), one-sided time is flat in k.** Each
   successful snipe empties a side for about one tick, whatever k is.
   Episodes per run fall slightly, from 251 at k = 1 to 243 at k = 21 (ratio
   0.97 [0.96, 0.97]). The one-sided share of time is 8.0% at k = 1 and 8.1%
   at k = 21 (ratio 1.01 [1.01, 1.02]). The end-of-tick snapshot catches 1%
   of those episodes at k = 1 and 18% at k = 21. That fraction is what grows
   with k.
5. **On the event clock (H2c), gaps are flat in k. HIT at both lags (pilot).**
   gaps(21)/gaps(3) is 1.00 [1.00, 1.00] under the §3.3 detector on periodic
   snapshots and 1.00 [1.00, 1.00] for exact one-sided time, at lag 0 and at
   lag > ε. Exact episode counts give 1.04 [1.04, 1.04] at lag 0, where the
   maker wins 1/(k + 1) of races, and 1.00 at lag > ε. The prediction was
   < 2; the tick model gives 4.9.

Recommended text change: drop S1.2's growth claim and its figure. In its place
put one sentence: "Gap counts in the tick model grow with k only because the
end-of-tick snapshot catches snipes that draw zero jitter; on the submission
clock, and on the event clock, one-sided time does not depend on k."
Supplement S1.1's 9.70-against-2.84 gap delta is the same artefact at k = 3
(6.7 of the 9.7 gaps are zero-jitter snapshots).

## A wider consequence: in the tick model k does nothing except through this channel

In the shifted arm every run is identical for every k ≥ 1, up to float
rounding (|Δ| < 1e-9 in HFT rent; gaps, fills and maker P&L are exactly
equal), at both operating points. With `hft_race_noise_us` (100) below
`dt_us` (1000), adding snipers changes only which of them wins. The tick
model's k-dependence is therefore all zero-jitter artefact:

- **Published operating point.** Total rent 34.82 → 33.70 and fills
  249 → 241 over k = 1 → 21; shifted: 34.91 and 250 at every k.
- **Calibrated operating point** (`gap_channel_tick_calibrated.json`). The
  decline in calibrated Exp 3 rent quoted in the supplement, 85,721 → 74,261
  over k = 1 → 21, is the artefact. Shifted, rent is 86,497 at every k, and removing the channel
  raises HFT rent by 12,236 [11,468, 12,955] at k = 21. Gaps run 32.0 → 217.8
  published and 21.3 flat shifted.

Why the channel moves money and not only gap counts: after a zero-jitter IOC
empties a side, every agent acting in the next tick sees that side empty.
Snipers and noise traders then skip it for one tick. This is a second
sampling problem: agents read the book at the start of a tick, before that
tick's submissions.

**A pre-stated prediction that missed.** T2 also said the shift would leave
HFT rent and maker P&L within 5% of their level at every k. At the published
point the point estimates are within 3.6%, but the HFT-rent interval exceeds
5% at k = 5, 13 and 21. At the calibrated point the shift moves rent by up to
16%. The channel is not purely observational, as the paragraph above
explains. So P2 H2b's reading that "the tick model's flat rent is the
slow-maker special case" is right at the published point. The calibrated
decline in k, though, should not be reported as a rent result.

## Evidence and reproduction

`experiments/gap_channel.py` (predictions T1–T3 and H2c are in its docstring,
committed in e024820 before the targeted runs). `TracedScheduler` is
`LatencyScheduler` plus one log line per submission. Its end-of-tick gap
counts, rent and fills reproduce `exp1_primary._run` exactly at both
operating points (`tests/test_gap_channel.py`).

| file | what |
|---|---|
| `results/pilot/gap_channel_tick.json` | published exp3 point, seeds 1–100, k ∈ exp3's grid, published vs shifted jitter |
| `results/pilot/gap_channel_tick_calibrated.json` | same at the calibrated point |
| `results/pilot/gap_channel_event.json` | event model, **pilot** (exp11's placeholder CFG), k ∈ {1, 3, 8, 21}, lag 0 and lag 90 µs ≫ ε = 2 µs, seeds 1–100 |

Disclosure: while checking that the tick reproduction was exact, we saw the
T1 attribution and T3 episode counts for two seeds and k ∈ {0, 1, 3, 21}, and
event-model counts for one seed, before committing the predictions. The jitter
shift (T2) had not been run.

Event-model caveats (pilot): exp11's placeholder maker quotes one unit and
does not requote on a fill, so most event-model gaps are sides an investor
emptied that wait for the next 10 ms update. That is why the detector counts
about 510 gaps per 60 s run regardless of k. The exact one-sided measure
(`TracedEventScheduler` logs the book after every engine arrival) is the one
to cite. Periodic snapshots fall at fixed phases of the update grid, so their
counts depend on where the samples land.

No shared files were changed or need changing. The tick scheduler stays as it
is, so published results keep reproducing.
