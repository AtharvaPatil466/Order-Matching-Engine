# Batch auctions: the interval-1 result, the tick batch's IOC handling, and a BCS batch on the event clock

## Verdict on the interval-1 result: explained. Keep the cell, drop "backfire" and "fragility"

The per-tick auction's larger maker loss (41.3 against 28.5 under continuous
trading, Exp 4) has an identified mechanism, and that mechanism predicted a
sign flip that we then observed. It does not show that batching backfires,
and the gap spike (about 137) is a sampling artefact. Recommended rewrite of
§4.3's interval-1 paragraph:

1. **What the maker loses is noise revenue, not more pick-offs.** In the tick
   batch a sniper's IOC enters the auction as a limit priced at the stale
   touch. A buy priced at the stale ask sits a full spread above the maker's
   own bid, so within the batch it has price priority over the maker for
   every noise sell in that batch. It buys that flow at the uniform price,
   which is often the stale ask itself (pinned by
   `test_tick_batch_sniper_ioc_outbids_the_makers_bid_for_noise_flow`). In
   continuous trading two IOCs never trade with each other, so that noise
   sell would have hit the maker's bid.
2. **The cost is about the same at every interval. Interval 1 is simply where
   batching has not yet removed the pick-offs.** The maker's direct pick-off
   loss is *smaller* at interval 1 than under continuous trading (−19.8
   against −34.0), because the IOC now meets the maker's next quote. With a
   3-tick maker and a 1-tick batch, though, most of that loss remains. Add
   the noise-revenue loss (≈ −22 at intervals 1 and 5 alike) and the total
   comes out larger than the continuous one.
3. **This is a redistribution, not a welfare loss.** HFT rent is unchanged
   (35.2 against 34.7). Under continuous trading the noise traders pay the
   HFTs 6.2; at interval 1 the HFTs' presence *improves* the noise traders'
   P&L by 6.1, because their sells now clear near the ask. The maker bears
   more than all of the rent.
4. **The gaps measure when the snapshot is taken, not fragility.** The tick
   batch processes a tick's IOCs after that tick's requote, so the
   end-of-tick snapshot lands after the snipe has emptied a side and before
   the next requote refills it. Continuous trading refills within the same
   tick. Sampled at the matching instant (after the IOC, before the requote),
   the continuous book shows 197.5 such gaps, more than interval 1's 137.0.
   The gap numbers for the batch arm should not be compared with the
   continuous arm's.

The defects listed in §1 below are real, but they do not drive this result.
They decide *which* sniper fills, not whether the maker does.

## 1. How the tick batch (`simulation/batch_auction.py`) handles IOCs

`BatchAuctionScheduler` parks the engine book in AuctionOpen, which makes the
engine reject IOCs. So it submits limits and cancels on arrival, holds
IOCs in a Python list, and at each uncross submits them as GTC limits *after*
everything else. Then it calls `uncross()` and cancels the unfilled
remainders. We checked each candidate defect with a test on the tick
scheduler (`tests/test_event_batch.py`, Part A); the tick scheduler is left
unchanged so published results keep reproducing.

| Candidate | Verdict | Test |
|---|---|---|
| IOCs submitted after the window's limits lose time priority at equal prices | **Defect.** An IOC that arrived at t=0 loses to a limit at the same price that arrived at t=2000; this matches neither price-time priority nor any batch rule | `test_tick_defect_ioc_loses_time_priority_to_a_later_limit` |
| Flush order follows the heap, not a defined rule | **Defect.** Same-price IOCs fill in `submit_at` order, so the latency race survives *inside* the batch, contradicting the module docstring ("submission TIME within an interval is irrelevant"). Exact ties go to whichever was enqueued first (registration order), never to a random draw | `test_tick_defect_same_price_iocs_still_race_inside_the_batch` |
| Flushed IOCs, entered as GTC, can rest between `uncross()` and the cancel | **Not a defect.** Agents act only in `act()`, so nothing can run between the two; the remainder is never visible | `test_tick_ioc_remainder_never_rests_after_the_uncross` |
| Cancels apply on arrival mid-window while IOCs wait | **Not a defect.** This gives BCS's property: a cancel later in the window beats an earlier IOC | `test_tick_cancel_mid_window_beats_an_earlier_ioc` |
| Market orders | **Latent.** They would be flushed as limits at their price field; no agent sends one | — |

Two engine facts also bear on every published batch number
(`tests/test_event_batch.py`, Part B):

- **The uncross reference price is the session's first limit order, and
  `uncross()` never updates it.** After a print at 1100, a 1000/1010 tie still
  clears at 1000, nearest the first order (900). The tie-break step that the
  paper (§5, Appendix A.4) and PREREGISTRATION P9 call "distance to the last
  clearing price" is in fact distance to the *opening* price. In the tick
  batch arm the first order is the maker's first bid (≈ v0 − h), so every
  volume-and-imbalance tie in Exp 4 resolved toward v0, however far V had
  drifted. The reference moves only on leaving a volatility auction.
- **Every uncross trade is stamped aggressor = Buy.** Fee accounting that
  relies on the engine's aggressor side would charge buyers the taker fee on
  every batch fill.

## 2. The interval-1 evidence

Source: `experiments/exp11_batch_semantics.py --tick-probe`, writing
`results/experiments/exp11_interval1_probe.json`. It uses Exp 1's operating
point and seeds 1–100, the seeds Exp 4 used, so the published cells
reproduce exactly (maker P&L 72.78 continuous and 30.75 at interval 1). The
maker's P&L is split by counterparty into *legs*: its trading edge
against V at each fill. The HFT effect is the treatment minus the no-HFT
baseline on matched seeds; brackets are 95% bootstrap intervals.

| HFT effect on | continuous | interval 1 | interval 5 |
|---|---|---|---|
| maker P&L | −28.5 [−38.3, −20.4] | −41.3 [−50.2, −33.6] | −22.5 [−29.9, −16.1] |
| maker leg vs noise | +1.5 [−0.2, 3.0] | **−22.2 [−23.1, −21.3]** | −22.7 [−23.4, −21.9] |
| maker leg vs HFT (pick-offs) | −34.0 [−34.6, −33.4] | **−19.8 [−20.3, −19.4]** | −4.2 [−4.5, −4.0] |
| noise-trader P&L | −6.2 [−7.9, −4.6] | **+6.1 [2.8, 8.9]** | +10.0 [3.9, 15.4] |
| HFT–noise volume (units) | 0 | 3,680 | 6,169 |

**The prediction, and the check.** If the extra loss is noise flow captured by
snipers, then removing the noise traders should reverse the sign: with
nothing to capture, the per-tick batch's smaller pick-off leg should leave the
maker *better* off than under continuous trading. We stated this before
running the probe's `no_noise` variant. With `n_noise = 0`, the
maker loses −25.3 [−26.8, −23.9] at interval 1 against −49.4 [−51.3, −47.7]
continuous; on matched seeds batching saves +24.2 [22.9, 25.5]. Prediction
confirmed.

**A hypothesis the data refuted.** Our first explanation was that losing
snipers' leftover volume (three 50-lot IOCs against one 50-lot quote) crosses
the noise flow. That predicts the effect shrinks with one HFT. It does not:
with `n_hft = 1` the interval-1 maker effect is −44.8 [−53.8, −37.2] against
−28.1 continuous. A single sniper's IOC outbids the maker's bid just as well.

**The batch baseline erodes too, which the paper does not say.** Without HFTs
the maker earns 101.3 continuous, 72.0 at interval 1 and 25.5 at interval 5,
because opposite noise IOCs in one batch cross each other. Part of the
"recovery" of the maker's delta at long intervals is the maker leaving the
market: there is less left to lose.

## 3. A BCS batch on the event clock (`simulation/event_batch.py`)

`EventBatchScheduler` subclasses `EventScheduler`. It keeps the parent's
latencies, jitter, per-agent FIFO arrivals and same-instant shuffle, and
replaces matching on arrival with:

- **Half-open windows** [kτ, (k+1)τ). The uncross runs before any arrival at
  exactly (k+1)τ, so an arrival on a boundary belongs to the next batch,
  deterministically.
- **Cancels take effect on arrival.** Because the book is sealed, honouring a
  cancel on arrival or at the boundary yields the same auction. What matters
  is that a cancel received during interval k is honoured before uncross k.
  That is BCS's design (orders may be submitted, modified or withdrawn at any
  time during the interval). It is also how call auctions work in practice
  (modifications and cancellations are accepted throughout the call phase up
  to the cut-off), and what the engine does natively in AuctionOpen. Tested:
  a cancel 1 ns after the sniper, mid-interval, or 1 ns before the boundary
  always beats the sniper's earlier IOC; the same arrivals on a continuous
  book give the sniper the fill; a cancel landing exactly on the boundary
  is too late.
- **IOC means "this auction only".** It enters the uncross as a limit and its
  remainder is cancelled right after: it never rests and is never seen.
- **Sealed book.** Agents and snapshots see the book as of the last uncross,
  BCS's information policy.
- **Rationing, swept.** `time`: arrival order, the engine's native rule.
  `random`: a seeded shuffle of the window's orders before submission, on
  its own RNG stream so the event stream is identical across arms. In both,
  orders resting from earlier batches stay ahead. Pro-rata is left out. The
  engine's uncross only fills price-time; emulating pro-rata would mean
  splitting orders into unit lots and re-splitting carried-over orders every
  batch. That is exact only to one lot, and it would multiply engine traffic.
  With equal-size snipers, `random` already gives each sniper the pro-rata
  share in expectation.
- **Clearing price, swept, the THEORY finding.** With one stale ask at A and
  two snipers bidding V′ > A, every price in [A, V′] ties on volume and
  imbalance. The engine clears at A when its reference is nearer A, which,
  given the static reference above, is a matter of where the session
  opened. BCS clear at V′ (Bertrand among snipers, Prop 6(iii)). Verified on
  the real engine (`test_engine_clears_a_stale_ask_against_two_bidders_at_the_stale_price`).
  The engine cannot be told to skip the reference step, so `price_rule =
  "pressure"` reprices the batch. The engine still decides who trades and how
  much; the price moves to the reference-free cascade's (the retired
  `discover_uncross_price`, `reference=None`), but only when that price clears
  the same volume and lies between every filled order's limit. That makes the
  engine's fills exactly the fills at the new price. Refusals are counted.
  When the tied set is an interval where supply equals demand, BCS would take
  its midpoint; this rule takes one end of it.
- **Fees.** The order that arrived later is the taker; an order resting from
  an earlier batch is the maker. This replaces the engine's blanket
  aggressor = Buy.

## 4. Pilot (exp11) — event model, placeholder parameters

*Pilot — pre-sign-off, placeholder parameters; not a test of P9.* Results:
`results/pilot/exp11_batch_semantics.json`.

PILOT_RESULTS_PLACEHOLDER

## 5. Requests for the C++ bridge (not done here)

1. Expose `OrderType::LOC` (a resting limit cancelled after the cross if
   unfilled) or an auction-only limit, so an IOC-in-batch is native rather
   than a Python submit-and-cancel.
2. A clearing-price option: skip the reference step (pressure-first), or
   update `referencePrice_` to each uncross print. Then `price_rule =
   "pressure"` would not need the Python reprice, and the engine's reference
   would be the last print the documentation claims.
3. Set `aggressorSide` on uncross trades to "none", or to the later order.
4. A pro-rata rationing mode in `uncross()` (the continuous book already has
   `matchProRata`).
5. Add `OrderTypeNotAllowedInState` (reject code 15) to the Python
   `RejectReason` enum; it currently prints as `???`.
