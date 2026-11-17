# Where each performance number comes from

Every line of README.md, BENCHMARKS.md, PerformanceWhitepaper.md,
Project_Overview.md and docs/CapacityPlanning.md that carries a number with a
time, rate, size, percentage or ratio unit (`152/order` included) — or a number
in a latency/throughput column, or in a table header — has a row here. **Only the rows whose source is `X86-71B332F-VM` have a raw artifact in the repository.**
One `bench-x86` run is landed, with `scripts/import_bench_results.sh <run-id>`,
as `results/71b332f962e0af4dc15570183530efc07e1cd7e1/`; the rows below write that directory as `results/71b332f…/`.
Rows marked *pending* name the file of that run which measures the same thing
on x86: the doc number on such a row has **not** been rewritten from it (it is
still the ARM or `d2e688c` figure), so it is no better sourced than the doc
says, and the row stays pending until the doc number is replaced from a landed file.

`tests/doc_perf_numbers_sourced.py` (ctest `DocPerfNumbersSourced`) fails when
a doc line with numbers has no row, when a row's numbers are no longer in its
file, when a row has no source, or when the lines moved. A row is keyed by its
file and the exact numbers on the line, so a changed number needs a new row;
and a row whose source is `WITHDRAWN`, `UNRECORDED` or `ESTIMATE` fails unless
the document itself says so ("withdrawn", "unrecorded" or "not recorded",
"estimate") on that line, in its table's header row or in an enclosing
heading, so a reader who never opens this file is not misled (DOCS-4);
a line that only moved is fixed with `tests/doc_perf_numbers_sourced.py .
--fix-lines`, and `--missing` prints the rows to fill in.

docs/OSTuning.md, docs/MatchingRules.md, docs/Runbook.md and the other
docs/*.md are not scanned. bcs_research/paper/bcs_paper_draft.md was read and
is not scanned either: its four ms/µs figures are simulation-model parameters
(one tick = 100 ms) and data properties, not engine performance.

## Sources

| ID | What it is | Raw artifact | Reproducible |
|---|---|---|---|
| `X86-D2E688C` | HonestBenchmark (and the A/B runs of that cycle) on AWS c6in.metal at commit `d2e688c` (2026-06-27): `-march=native`, NUMA-pinned, 5 runs; PGO trained on seed 42; Path C with the io_uring ack and the journal on a `/tmp` whose backing was not recorded | none | No. No log; the harness has since changed (warm-up, steady_clock, trade counting) and so has the match path (`0c3115e`) |
| `X86-71B332F-VM` | The `bench-x86.yml` dispatch (run 44027950500, 2026-11-17) on `main` at `71b332f`: GitHub-hosted Ubuntu 24.04 VM, Intel Xeon 6975P-C, 4 vCPU, clang 18, `taskset -c 1`, five repetitions of HonestBenchmark per build; every step exited 0. A shared VM, one run. A `HonestBenchmark` figure is the median of the five repetitions' own printed values, and a bracketed range is their minimum and maximum | `results/71b332f962e0af4dc15570183530efc07e1cd7e1/` in this repository, landed unchanged; its `README.md` lists the files and `env.txt` the machine | Re-runnable by dispatching `bench-x86.yml`; a hosted runner may draw a different CPU |
| `X86-ADC7B5B-VM` | The one `bench-x86.yml` dispatch (run 41896421385, 2026-11-04) at `adc7b5b` on `main`: GitHub-hosted Ubuntu 24.04 VM, AMD EPYC 9V74, 4 vCPU, clang 18, five repetitions, a clock on a ~10 ns grid; recorded as failed because the `perf` steps had no hardware counters | a copy kept with the adversarial review (`benchx86/`), outside this repository; the GitHub artifact until its retention expires | Re-runnable by dispatching `bench-x86.yml` at that commit; it is before this round's changes |
| `ARM-M3` | Runs on the Apple M3 Pro dev machine, `-O3 -march=native`, on a non-isolated box | none | No. No log; a ~42 ns clock tick |
| `AWS-WIRE` | Two-host `WireLatencySender`/`Receiver` run on two c6in.metal | none | No: the OUCH harness was deleted with OUCH (roadmap 1.2) |
| `X86-CI-JOURNAL` | fdatasync ~438 µs (`include/Journal.h`) and 165–190k appends/s from the x86 CI journal-contention run | none | Yes, by dispatching journal-contention.yml; no artifact kept |
| `UNRECORDED` | A measurement or observation whose machine, commit and run are not recorded | none | **No reproducible source** |
| `ESTIMATE` | A planning figure or rule of thumb that no benchmark produces | none | **No reproducible source** |
| `WITHDRAWN` | Kept only so a withdrawal has something to point at; the doc says not to cite it | none | No |
| `CODE` | A property of the source: a `static_assert`ed size, a compiled-in constant | the code | Yes |
| `PARAM` | A configured value or workload parameter (offered rate, buffer size), not a measurement | n/a | n/a |
| `ARITH` | Arithmetic on other numbers in the same doc (a ratio, a sum, a cycle count) | n/a | Follows its inputs |
| `NOT-PERF` | A number with a unit that is not engine performance (TLC wall time, a research-result interval) | n/a | n/a |

"x86 counterpart" means bench-x86 measures the same benchmark on x86; it does
not reproduce the ARM figure, which needs a rerun on a quiet Mac.

## Rows

### README.md

| Where | Numbers | Source | Raw artifact |
|---|---|---|---|
| README.md:49 | 1472 bytes | PARAM | n/a: not a measurement |
| README.md:35,61 | 40% | CODE | the LMM/DMM floor constant |
| README.md:56,58 | 5% | CODE | `cbThreshold_`, `marketProtectionPct_` |
| README.md:100 | 143 ns | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path A, median of five |
| README.md:101 | 95 µs | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path D, median of five |

### BENCHMARKS.md

| Where | Numbers | Source | Raw artifact |
|---|---|---|---|
| BENCHMARKS.md:24 | 237 ns · 3.10M ops/s · 261 ns | X86-D2E688C | pending `honest-pgo-*.txt`, `honest-native-*.txt` |
| BENCHMARKS.md:25,325,555 | 615 ns | X86-D2E688C | pending `honest-native-*.txt` Path C |
| BENCHMARKS.md:29 | 10× | ARITH | pending `cold-cache.txt` (x86 counterpart) |
| BENCHMARKS.md:59 | 237 ns | X86-D2E688C | pending `honest-pgo-*.txt` |
| BENCHMARKS.md:60 | 910 ns · 3.8× | X86-D2E688C | pending `honest-pgo-*.txt` |
| BENCHMARKS.md:70 | 208 ns · 709 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:85,413 | 64 MB | PARAM | n/a: not a measurement |
| BENCHMARKS.md:111 | ~2.6× | ARITH | pending `realistic-flow-sparse.txt` (x86 counterpart) |
| BENCHMARKS.md:126 | 250 ns · 375 ns · 417 ns · 227 ns | ARM-M3 | pending `realistic-flow-sparse.txt` (x86 counterpart) |
| BENCHMARKS.md:238 | 261 ns · 620 ns · 1,010 ns · 2.80M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| BENCHMARKS.md:239 | 269 ns · 620 ns · 1,001 ns · 2.74M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| BENCHMARKS.md:240 | 615 ns · 1,048 ns · 3,568 ns · 1.28M ops/s | X86-D2E688C | pending `honest-native-*.txt` Path C |
| BENCHMARKS.md:243 | 237 ns · 910 ns | X86-D2E688C | pending `honest-pgo-*.txt` |
| BENCHMARKS.md:244 | 3.10M ops/s | X86-D2E688C | pending `honest-pgo-*.txt` |
| BENCHMARKS.md:264 | 12,638 instructions · ~9,430 cycles · ~3.25 µs | ARITH | none |
| BENCHMARKS.md:265,274,551 | 261 ns | X86-D2E688C | pending `honest-native-*.txt` |
| BENCHMARKS.md:276,478 | 10 ns · 62 ns | X86-D2E688C | none: an A/B of a past change; bench-x86 measures HEAD only |
| BENCHMARKS.md:289 | 41 ns · 42 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:293 | ~41.67 ns · 208 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:310,352 | 42 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:306 | 174 ns · 135 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:308 | ~0.34 ns · 120× | ARITH | n/a: not a measurement |
| BENCHMARKS.md:309 | 41.67 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:316 | 250 ns · 542 ns · 1,166 ns · 1,709 ns · 91,917 ns · 2.76M ops/s | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:317 | 250 ns · 500 ns · 791 ns · 1,208 ns · 43,292 ns · 2.95M ops/s | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:318 | 1,750 ns · 4,096 ns · 1,761,280 ns · 3,899,392 ns · 10,773,458 ns · 24,603 ops/s | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:323,423 | 250 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:324,464,552 | 8 ns | ARITH | pending `honest-native-*.txt` |
| BENCHMARKS.md:346 | 42 ns · 375 ns · 667 ns · 792 ns · 18,208 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:347 | 208 ns · 583 ns · 708 ns · 1,416 ns · 8,583 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:348 | 208 ns · 375 ns · 791 ns · 1,792 ns · 4,792 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:349 | 292 ns · 667 ns · 1,125 ns · 1,750 ns · 2,625 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:350 | 208 ns · 458 ns · 709 ns · 1,250 ns · 18,208 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:359 | 208 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:362 | 18 µs · 792 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:382 | 100K/s · 0.0% · 1,166 ns · 1,250 ns · 4,192 ns · 5,280 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:383 | 500K/s · 0.2% · 834 ns · 1,000 ns · 2,334 ns · 5,536 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:384 | 1M/s · 0.4% · 750 ns · 1,000 ns · 1,958 ns · 6,016 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:385 | 2M/s · 6.6% · 54% | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:403,465 | 3M/s | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:387 | 4M/s · 42.7% · 583 ns · 27,136 ns · 1,458 ns · 56,576 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:388 | 6M/s · 100.0% · 500 ns · 16,777,216 ns · 1,333 ns · 17,039,360 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:390,393,447,499 | 1M/s | PARAM | n/a: not a measurement |
| BENCHMARKS.md:392 | 1.26× · 100K/s · 2.37× · 500K/s · 3.07× · 1M/s | ARITH | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:394 | 0.2% · 0.5% · 0.6% · 834 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:395 | 1,208 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:398 | 6M/s · 500 ns · 16.8 ms | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:405 | 2.6 ms · 10.4 ms · 3M/s · 83.4% · 90.1% · 99.9% | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:409 | 1M/s · 0.4% · 4M/s · 42.7% | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:417 | 250 ns · 500 ns · 958 ns · 4,832 ns · 36,542 ns · 302 ns | ARM-M3 | pending `cold-cache.txt` (x86 counterpart) |
| BENCHMARKS.md:418 | 2,625 ns · 7,648 ns · 12,416 ns · 34,560 ns · 3,531,875 ns · 3,855 ns | ARM-M3 | pending `cold-cache.txt` (x86 counterpart) |
| BENCHMARKS.md:419 | 2,375 ns · 7,148 ns · 11,458 ns · 29,728 ns | ARITH | pending `cold-cache.txt` (x86 counterpart) |
| BENCHMARKS.md:421 | 10× · 41 ns | ARITH | pending `cold-cache.txt` (x86 counterpart) |
| BENCHMARKS.md:429 | 30 s | PARAM | n/a: not a measurement |
| BENCHMARKS.md:432 | 30.2 s · 999,987 orders/sec | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:433 | 843,520 KB · 1,186,016 KB · 40.6% | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:441 | ~780 ms · ~48 µs | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:443 | 1,073,741,824 ns | ARITH | n/a: not a measurement |
| BENCHMARKS.md:448 | 999,987/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:480 | 32.8% · 3,568 ns · 167 ns | X86-D2E688C | none: an A/B of a past change; bench-x86 measures HEAD only |
| BENCHMARKS.md:481 | 2.5-3.6% · 100% · 16% · 13% · 33% · 62% · 5-400x · ~185us · 9.9us · 240us · ~10us | ARM-M3 | none: an A/B of a past change; bench-x86 measures HEAD only |
| BENCHMARKS.md:491 | 780 ms · 48 µs | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:500 | 250k/s | PARAM | n/a: not a measurement |
| BENCHMARKS.md:501 | ~1M/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:502 | 400k/s · 400,000/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:504 | 400k/s · 1 s | PARAM | n/a: not a measurement |
| BENCHMARKS.md:509 | 10,688 ns · 10.4 ms · 953 MB | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:510 | ~790 ns · 7–11 µs · 953 MB · 1,186 MB | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:511 | ~850 ns · 330–415 µs | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:513 | 402k/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:514 | ~6 s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:518 | ~790 ns · 400k/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:519 | ~4,500 ns · 50k/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:527 | 5 µs | PARAM | n/a: not a measurement |
| BENCHMARKS.md:537 | 1μs | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:538 | 2.7ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:544 | 4x | ARITH | n/a: not a measurement |
| BENCHMARKS.md:553 | 346 ns | ARITH | pending `honest-native-*.txt` |
| BENCHMARKS.md:557 | ~42 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:571 | 16 s | NOT-PERF | n/a: not a measurement |
| BENCHMARKS.md:606 | 36 B · 34.1 · 29.36 · 1.06 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| BENCHMARKS.md:39 | ~44% | X86-ADC7B5B-VM | `honest-*.txt` "Traded" line |
| BENCHMARKS.md:40 | 44% · 46% · 8% · 2% | PARAM | n/a: the generator's mix |
| BENCHMARKS.md:45 | 15% · 65% | PARAM | n/a: `RealisticWorkload.h` parameters |
| BENCHMARKS.md:51 | 43.8% | ARM-M3 | none |
| BENCHMARKS.md:53,321 | 100.0% | WITHDRAWN | none |
| BENCHMARKS.md:67,340 | 43.9% · 46.2% | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:68 | 7.9% · 2.0% | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:116 | 48% · 49% | ARM-M3 | pending `realistic-flow-sparse.txt` (x86 counterpart) |
| BENCHMARKS.md:127 | 10.4% | ARM-M3 | none |
| BENCHMARKS.md:290 | 41.7% | ARM-M3 | none: a property of the M3 clock |
| BENCHMARKS.md:294 | 20% · 42 ns | ARITH, ARM-M3 | none |
| BENCHMARKS.md:299 | 22-32% | ARM-M3 | none |
| BENCHMARKS.md:334 | 79% | ARM-M3 | none |
| BENCHMARKS.md:335 | ~10% | ARM-M3 | none |
| BENCHMARKS.md:336 | 38% | ARM-M3 | none |
| BENCHMARKS.md:341 | 7.9% · 41.3% · 2.0% | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:353 | 42 ns · 26.4% | ARM-M3 | none |
| BENCHMARKS.md:357 | 73.6% | ARM-M3 | none |
| BENCHMARKS.md:358 | 11.8% | ARM-M3 | none |
| BENCHMARKS.md:386 | 3M/s · 5.5% · 99.9% | PARAM, ARM-M3 | pending `co-3000000.txt` (x86 counterpart) |
| BENCHMARKS.md:399 | 100% | ARM-M3 | pending `co-6000000.txt` (x86 counterpart) |
| BENCHMARKS.md:404 | 2M/s · 42.0% · 48.5% · 54.2% | PARAM, ARM-M3 | pending `co-2000000.txt` (x86 counterpart) |
| BENCHMARKS.md:406 | 6.6% · 5.5% | ARM-M3 | none |
| BENCHMARKS.md:479 | 152/order · 100% · 1.2% | WITHDRAWN | none |
| BENCHMARKS.md:543 | 32.8% | X86-D2E688C | pending `honest-native-*.txt` Path C |
| BENCHMARKS.md:23 | 143 ns | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path A, median of five |
| BENCHMARKS.md:176 | 143 ns · 596 ns · 958 ns · 4.56M orders/s | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path A, median of five |
| BENCHMARKS.md:177 | 208 ns · 797 ns · 1,261 ns · 3.23M orders/s | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path L, median of five |
| BENCHMARKS.md:178 | 226 ns · 834 ns · 1,340 ns · 3.01M orders/s | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path B, median of five |
| BENCHMARKS.md:179 | 325 ns · 132 µs · 154 µs · 383k orders/s | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path C, median of five |
| BENCHMARKS.md:180 | 95.2 µs · 145 µs · 276 µs · 10.8k orders/s | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path D, median of five |
| BENCHMARKS.md:181 | 152 ns · 726 ns · 1,473 ns · 4.25M orders/s | X86-71B332F-VM | `results/71b332f…/honest-native-*.txt` Path A, median of five |
| BENCHMARKS.md:182 | 209 ns · 811 ns · 1,316 ns · 3.23M orders/s | X86-71B332F-VM | `results/71b332f…/honest-native-*.txt` Path L, median of five |
| BENCHMARKS.md:183 | 228 ns · 824 ns · 1,314 ns · 3.01M orders/s | X86-71B332F-VM | `results/71b332f…/honest-native-*.txt` Path B, median of five |
| BENCHMARKS.md:184 | 347 ns · 115 µs · 140 µs · 347k orders/s | X86-71B332F-VM | `results/71b332f…/honest-native-*.txt` Path C, median of five |
| BENCHMARKS.md:185 | 119 ns · 547 ns · 885 ns · 5.22M orders/s | X86-71B332F-VM | `results/71b332f…/honest-pgo-*.txt` Path A, median of five |
| BENCHMARKS.md:186 | 192 ns · 746 ns · 1,178 ns · 3.49M orders/s | X86-71B332F-VM | `results/71b332f…/honest-pgo-*.txt` Path L, median of five |
| BENCHMARKS.md:187 | 199 ns · 737 ns · 1,150 ns · 3.36M orders/s | X86-71B332F-VM | `results/71b332f…/honest-pgo-*.txt` Path B, median of five |
| BENCHMARKS.md:188 | 309 ns · 113 µs · 138 µs · 352k orders/s | X86-71B332F-VM | `results/71b332f…/honest-pgo-*.txt` Path C, median of five |
| BENCHMARKS.md:193 | 0.21 s | X86-71B332F-VM | `results/71b332f…/env.txt`, the fdatasync probe |
| BENCHMARKS.md:194 | 4 KiB | X86-71B332F-VM | `results/71b332f…/env.txt`, the fdatasync probe |
| BENCHMARKS.md:195 | 28.9k orders/s | X86-71B332F-VM | `results/71b332f…/honest-native-3.txt` Path C |
| BENCHMARKS.md:196 | 2.69 ms | X86-71B332F-VM | `results/71b332f…/honest-native-3.txt` Path C |
| BENCHMARKS.md:203 | 100k ops/s · 232 ns · 1,435 ns · 7,488 ns · 0.1% · 3,077 ns | X86-71B332F-VM | `results/71b332f…/co-100000.txt`, `co-poisson-100000.txt`, CO-corrected |
| BENCHMARKS.md:204 | 500k ops/s · 220 ns · 3,877 ns · 11,008 ns · 1.3% · 4,544 ns | X86-71B332F-VM | `results/71b332f…/co-500000.txt`, `co-poisson-500000.txt`, CO-corrected |
| BENCHMARKS.md:205 | 1M ops/s · 217 ns · 5,984 ns · 18,176 ns · 2.6% · 5,216 ns | X86-71B332F-VM | `results/71b332f…/co-1000000.txt`, `co-poisson-1000000.txt`, CO-corrected |
| BENCHMARKS.md:206 | 2M ops/s · 215 ns · 8,768 ns · 15,552 ns · 9.0% · 10,816 ns | X86-71B332F-VM | `results/71b332f…/co-2000000.txt`, `co-poisson-2000000.txt`, CO-corrected |
| BENCHMARKS.md:207 | 3M ops/s · 5,120 ns · 995 µs · 1,049 µs · 58.0% · 647 µs | X86-71B332F-VM | `results/71b332f…/co-3000000.txt`, `co-poisson-3000000.txt`, CO-corrected |
| BENCHMARKS.md:208 | 4M ops/s · 9.37 ms · 13.8 ms · 13.8 ms · 100.0% · 14.7 ms | X86-71B332F-VM | `results/71b332f…/co-4000000.txt`, `co-poisson-4000000.txt`, CO-corrected |
| BENCHMARKS.md:209 | 6M ops/s · 19.4 ms · 32.5 ms · 32.6 ms · 100.0% · 33.0 ms | X86-71B332F-VM | `results/71b332f…/co-6000000.txt`, `co-poisson-6000000.txt`, CO-corrected |
| BENCHMARKS.md:211 | 2M ops/s | X86-71B332F-VM | `results/71b332f…/co-2000000.txt`, `co-4000000.txt` |
| BENCHMARKS.md:217 | 331 ns · 435 ns | X86-71B332F-VM | `results/71b332f…/realistic-flow.txt` |
| BENCHMARKS.md:218 | 171 ns · 210 ns | X86-71B332F-VM | `results/71b332f…/realistic-flow.txt` |
| BENCHMARKS.md:219 | 211 ns · 827 ns | X86-71B332F-VM | `results/71b332f…/realistic-flow.txt` |
| BENCHMARKS.md:220 | 494 ns · 633 ns | X86-71B332F-VM | `results/71b332f…/realistic-flow.txt` |
| BENCHMARKS.md:221 | 204 ns · 506 ns | X86-71B332F-VM | `results/71b332f…/realistic-flow.txt` |
| BENCHMARKS.md:222 | 254 ns · 489 ns | X86-71B332F-VM | `results/71b332f…/realistic-flow-sparse.txt` |
| BENCHMARKS.md:223 | 366 ns · 1,203 ns | X86-71B332F-VM | `results/71b332f…/cold-cache.txt` |
| BENCHMARKS.md:224 | 3,469 ns · 11,328 ns | X86-71B332F-VM | `results/71b332f…/cold-cache.txt` |
| BENCHMARKS.md:226 | 20 s · 50k/s · 400k/s | X86-71B332F-VM | `results/71b332f…/sustained-50k.txt`, `sustained-400k.txt` |

### PerformanceWhitepaper.md

| Where | Numbers | Source | Raw artifact |
|---|---|---|---|
| PerformanceWhitepaper.md:21 | 261 ns · 615 ns · 615 ns · ~125 ns · ~2.2× | X86-D2E688C, WITHDRAWN | pending `honest-native-*.txt` |
| PerformanceWhitepaper.md:48 | ~41.67 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| PerformanceWhitepaper.md:69 | 261 ns · 620 ns · 1,010 ns · 2.80M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| PerformanceWhitepaper.md:70 | 269 ns · 620 ns · 1,001 ns · 2.74M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| PerformanceWhitepaper.md:71 | 615 ns · 1,048 ns · 3,568 ns · 1.28M ops/s | X86-D2E688C | pending `honest-native-*.txt` Path C |
| PerformanceWhitepaper.md:75 | 32.8% · 3,568 ns | X86-D2E688C | pending `honest-native-*.txt` Path C |
| PerformanceWhitepaper.md:77 | 237 ns · 910 ns · 3.10M ops/s | X86-D2E688C | pending `honest-pgo-*.txt` |
| PerformanceWhitepaper.md:96 | 12,638 instructions · ~9,430 cycles · ~3.25 µs | ARITH | none |
| PerformanceWhitepaper.md:97,106 | 261 ns | X86-D2E688C | pending `honest-native-*.txt` |
| PerformanceWhitepaper.md:113 | ~42 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| PerformanceWhitepaper.md:115 | ~84 ns · ~1,040 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:116,243 | 615 ns | X86-D2E688C | pending `honest-native-*.txt` Path C |
| PerformanceWhitepaper.md:124 | 6,561,860 orders/sec | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:126,143,175 | 41 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:118,127,171,174 | 125 ns | WITHDRAWN | none |
| PerformanceWhitepaper.md:128 | 209 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:129 | 333 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:130 | 458 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:131 | 48,834 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:132 | 134 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:141 | 7,114,863 orders/sec | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:144 | 84 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:145 | 208 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:146 | 292 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:147 | 417 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:148 | 19,333 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:149 | 124 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:160 | 22,293 orders/sec | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:162 | 83 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:163,178 | 1,040 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:164 | 2,040 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:165 | 2,670,592 ns · 2.7 ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:166 | 3,981,312 ns · 4.0 ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:167 | 9,749,958 ns · 9.7 ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:168 | 44,768 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:176 | 956 ns | ARITH | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:186 | 1μs | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:187,245 | 2.7ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:194 | 4x | ARITH | n/a: not a measurement |
| PerformanceWhitepaper.md:210 | ~20μs · 4KB | ESTIMATE | none |
| PerformanceWhitepaper.md:211 | ~50-100μs | ESTIMATE | none |
| PerformanceWhitepaper.md:212 | ~2-10ms | ESTIMATE | none |
| PerformanceWhitepaper.md:214 | 4ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:242 | 261 ns · 237 ns | X86-D2E688C | pending `honest-pgo-*.txt`, `honest-native-*.txt` |
| PerformanceWhitepaper.md:244 | ~125 ns | WITHDRAWN | none |
| PerformanceWhitepaper.md:193 | 261 ns · 32.8% | X86-D2E688C | pending `honest-native-*.txt` |

### Project_Overview.md

| Where | Numbers | Source | Raw artifact |
|---|---|---|---|
| Project_Overview.md:99 | 1 MiB | CODE | `TcpGateway::kMaxWriteBufBytes` |
| Project_Overview.md:243 | 100 ms · 400k/s | PARAM | n/a: `SustainedLoadTest --max-backlog-ms` default and the offered rate |
| Project_Overview.md:149 | ~41 ns | ARM-M3 | none: a property of the M3 clock |
| Project_Overview.md:164 | 95% · 86% | NOT-PERF | line and branch coverage of `OrderBook.cpp`, measured by the adversarial review on `main` (its §8) |
| Project_Overview.md:129 | 143 ns · 596 ns | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path A, median of five |
| Project_Overview.md:130 | 208 ns · 797 ns | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path L, median of five |
| Project_Overview.md:131 | 226 ns · 834 ns | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path B, median of five |
| Project_Overview.md:132 | 325 ns · 132 µs · 334–402k orders/s | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path C: median of five, and the range of the five throughputs |
| Project_Overview.md:133 | 95 µs · 141–171 µs · 10.7–11.0k orders/s | X86-71B332F-VM | `results/71b332f…/honest-prod-*.txt` Path D: median of five, and the ranges of the five |
| Project_Overview.md:134 | 204 ns · 506 ns · 331 ns · 171 ns | X86-71B332F-VM | `results/71b332f…/realistic-flow.txt` |
| Project_Overview.md:135 | 3.47 µs · 366 ns | X86-71B332F-VM | `results/71b332f…/cold-cache.txt` |
| Project_Overview.md:136 | 8.8 µs · 2M ops/s · 3M ops/s · 58.0% · 995 µs · 4M ops/s | X86-71B332F-VM | `results/71b332f…/co-2000000.txt`, `co-3000000.txt`, `co-4000000.txt`, CO-corrected |
| Project_Overview.md:137 | 50k/s · 400k/s · 20 s | X86-71B332F-VM | `results/71b332f…/sustained-50k.txt`, `sustained-400k.txt` |
| Project_Overview.md:140 | 280 ns | X86-ADC7B5B-VM | review copy `benchx86/bench-x86-adc7b5b…/`, outside the repository: `honest-prod-*.txt` Path A |
| Project_Overview.md:143 | 0.21 s · 4 KiB | X86-71B332F-VM | `results/71b332f…/env.txt`, the fdatasync probe |
| Project_Overview.md:144 | 119 ns · 152 ns · 28.9k orders/s | X86-71B332F-VM | `results/71b332f…/honest-pgo-*.txt` and `honest-native-*.txt` Path A, median of five; `honest-native-3.txt` Path C |
| Project_Overview.md:145 | 100 ms · 400k/s · 107k/s | PARAM, X86-ADC7B5B-VM | `SustainedLoadTest --max-backlog-ms` default and the offered rate; the decayed rate is from the review copy `benchx86/bench-x86-adc7b5b…/sustained-400k.txt`, outside the repository |

### docs/CapacityPlanning.md

| Where | Numbers | Source | Raw artifact |
|---|---|---|---|
| docs/CapacityPlanning.md:12 | 192 bytes | CODE | the code (`memory-probe.txt` prints the sizeof) |
| docs/CapacityPlanning.md:13 | 16 bytes | ESTIMATE | none |
| docs/CapacityPlanning.md:14,21 | 8 bytes | ESTIMATE | none |
| docs/CapacityPlanning.md:15 | 32 bytes | ESTIMATE | none |
| docs/CapacityPlanning.md:16 | ~248 bytes | ARITH | none |
| docs/CapacityPlanning.md:20 | 248 bytes | ARITH | none |
| docs/CapacityPlanning.md:22 | 64KB | WITHDRAWN | none |
| docs/CapacityPlanning.md:24 | 256MB | ESTIMATE | none |
| docs/CapacityPlanning.md:29 | 10.3 MiB | ARM-M3 | pending `memory-probe.txt` |
| docs/CapacityPlanning.md:30 | 64 KB | WITHDRAWN | none |
| docs/CapacityPlanning.md:31 | ~248 B | ARITH | none |
| docs/CapacityPlanning.md:38 | ~256 MB | ESTIMATE | none |
| docs/CapacityPlanning.md:39 | ~512 MB | ESTIMATE | none |
| docs/CapacityPlanning.md:40 | ~2 GB | ESTIMATE | none |
| docs/CapacityPlanning.md:41 | ~8 GB | ESTIMATE | none |
| docs/CapacityPlanning.md:44 | 10.3 MiB · 0.5 GiB · 5 GiB · 20 GiB | ARITH | pending `memory-probe.txt` |
| docs/CapacityPlanning.md:70 | 25M ops/s · 1.9M ops/s | ESTIMATE | none |
| docs/CapacityPlanning.md:71 | 80M ops/s · 7M ops/s | ESTIMATE | none |
| docs/CapacityPlanning.md:72 | 150M ops/s · 14M ops/s | ESTIMATE | none |
| docs/CapacityPlanning.md:73 | 280M ops/s · 25M ops/s | ESTIMATE | none |
| docs/CapacityPlanning.md:75 | 2.80M ops/s | X86-D2E688C | pending `honest-pgo-*.txt`, `honest-native-*.txt` |
| docs/CapacityPlanning.md:91 | 50–200 µs | ESTIMATE | none |
| docs/CapacityPlanning.md:100,106,107,108,110 | 134 bytes | CODE | the code (`memory-probe.txt` prints the sizeof) |
| docs/CapacityPlanning.md:102 | 24-100 bytes | WITHDRAWN | none |
| docs/CapacityPlanning.md:109 | 24 bytes | CODE | the code |
| docs/CapacityPlanning.md:115 | 100 bytes | ESTIMATE | none |
| docs/CapacityPlanning.md:120 | ~10 MB · ~300 MB | ESTIMATE | none |
| docs/CapacityPlanning.md:121 | ~150 MB · ~4.5 GB | ESTIMATE | none |
| docs/CapacityPlanning.md:122 | ~1.5 GB · ~45 GB | ESTIMATE | none |
| docs/CapacityPlanning.md:123 | ~15 GB · ~450 GB | ESTIMATE | none |
| docs/CapacityPlanning.md:127 | 64 MiB | CODE | the code |
| docs/CapacityPlanning.md:135 | ~1K-5K · ~438 µs · ~1-5 µs | ESTIMATE, X86-CI-JOURNAL, WITHDRAWN | journal-contention.yml, not bench-x86; `env.txt` records an O_DSYNC probe |
| docs/CapacityPlanning.md:136 | 10K-100K | ESTIMATE | none |
| docs/CapacityPlanning.md:158 | ~0 | ESTIMATE | none |
| docs/CapacityPlanning.md:159,160 | ~10–30 µs | ESTIMATE | none |
| docs/CapacityPlanning.md:161 | ~1–4 ms | ESTIMATE | none |
| docs/CapacityPlanning.md:164,172 | 3,568 ns | X86-D2E688C | pending `honest-native-*.txt` Path C |
| docs/CapacityPlanning.md:188 | 150 bytes · 1.5 MB/s · 15 MB/s | CODE, ARITH | `sizeof(GatewayRequestV2)` + 4 |
| docs/CapacityPlanning.md:189 | 332 bytes · 3.3 MB/s · 33 MB/s | CODE, ARITH | `sizeof(GatewayResponseV2)` + 4 |
| docs/CapacityPlanning.md:190 | 300 bytes · 3 MB/s · 30 MB/s | ESTIMATE | none |
| docs/CapacityPlanning.md:191 | ~500 bytes | ESTIMATE | none |
| docs/CapacityPlanning.md:192 | ~8 MB/s · ~78 MB/s | ARITH | none |
| docs/CapacityPlanning.md:197 | ~50-100μs | ESTIMATE | none |
| docs/CapacityPlanning.md:198 | ~5-10μs | ESTIMATE | none |
| docs/CapacityPlanning.md:199 | ~1-2μs | ESTIMATE | none |
| docs/CapacityPlanning.md:208 | 50K orders/sec · 10μs | PARAM | n/a: not a measurement |
| docs/CapacityPlanning.md:211 | 512 MB · 248 bytes · 124 MB | ESTIMATE | none |
| docs/CapacityPlanning.md:212 | 200 GB · 86400s · 80 bytes · 345 GB | ESTIMATE | none |
| docs/CapacityPlanning.md:186 | 10K msg/s · 100K msg/s | PARAM | n/a: not a measurement |
| docs/CapacityPlanning.md:213 | ~90% | ESTIMATE | none |
| docs/CapacityPlanning.md:223,228 | 80% | PARAM | n/a: an alert threshold |
| docs/CapacityPlanning.md:225 | 50% | PARAM | n/a: an alert threshold |
