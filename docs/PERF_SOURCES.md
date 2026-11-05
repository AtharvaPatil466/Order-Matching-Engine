# Where each performance number comes from

Every line of README.md, BENCHMARKS.md, PerformanceWhitepaper.md,
Project_Overview.md and docs/CapacityPlanning.md that carries a number with a
time, rate, size or ratio unit — or a number in a latency/throughput column —
has a row here. **None of them has a raw artifact in the repository yet.**
Rows marked *pending* name the file the manual `bench-x86` workflow writes;
once a run is landed with `scripts/import_bench_results.sh <run-id>` it is
`results/<sha>/<file>`, and the doc number and its row are rewritten from it.
Until then a pending number is no better sourced than the doc says.

`tests/doc_perf_numbers_sourced.py` (ctest `DocPerfNumbersSourced`) fails when
a doc line with numbers has no row, when a row's numbers are no longer in its
file, when a row has no source, or when the lines moved. A row is keyed by its
file and the exact numbers on the line, so a changed number needs a new row;
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
| `ARM-M3` | Runs on the Apple M3 Pro dev machine, `-O3 -march=native`, on a non-isolated box | none | No. No log; a ~42 ns clock tick |
| `AWS-WIRE` | Two-host `WireLatencySender`/`Receiver` run on two c6in.metal | none | Only with two hosts (`scripts/wire_latency_aws.sh`) |
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
| README.md:16 | 237ns | X86-D2E688C | pending `honest-pgo-*.txt` |
| README.md:17 | 3.10M_ops/s | X86-D2E688C | pending `honest-pgo-*.txt` |
| README.md:18 | 1ns/op | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| README.md:50 | 24 bytes | CODE | the code |
| README.md:50 | 1s · 15s | PARAM | n/a: not a measurement |
| README.md:55 | 1472 bytes | PARAM | n/a: not a measurement |
| README.md:106 | 125 ns | WITHDRAWN | none |
| README.md:116 | 237 ns · 910 ns · 3.8× | X86-D2E688C | pending `honest-pgo-*.txt` |
| README.md:117 | 208 ns · 709 ns · 3.4× | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| README.md:120 | 208 ns · 237 ns | ARM-M3, X86-D2E688C | pending `realistic-flow.txt` (x86 counterpart) |
| README.md:122 | ~42 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| README.md:131 | 261 ns · 620 ns · 1,010 ns · 2.80M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| README.md:132 | 269 ns · 620 ns · 1,001 ns · 2.74M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| README.md:133 | 615 ns · 1,048 ns · 3,568 ns · 1.28M ops/s | X86-D2E688C | pending `honest-native-*.txt` Path C |
| README.md:136 | ~125 ns | WITHDRAWN | none |
| README.md:138 | 3,568 ns · ~167 ns | X86-D2E688C | pending `honest-native-*.txt` Path C |
| README.md:140 | 237 ns · 910 ns · 3.10M ops/s | X86-D2E688C | pending `honest-pgo-*.txt` |
| README.md:149 | 5M ops/s | UNRECORDED | pending `stress.txt` |
| README.md:157 | 49B · 112.0 · 8.9 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| README.md:158 | 49B · 10.2 · 98.5 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| README.md:159 | 36B · 34.1 · 29.4 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| README.md:160 | 32B · 1.0 · 1015 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| README.md:161 | 32B · 0.5 · 2128 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| README.md:163 | 110× | ARITH | pending `codec.txt` (x86 counterpart) |

### BENCHMARKS.md

| Where | Numbers | Source | Raw artifact |
|---|---|---|---|
| BENCHMARKS.md:20 | 237 ns · 3.10M ops/s · 261 ns | X86-D2E688C | pending `honest-pgo-*.txt`, `honest-native-*.txt` |
| BENCHMARKS.md:21,253,482 | 615 ns | X86-D2E688C | pending `honest-native-*.txt` Path C |
| BENCHMARKS.md:24,547 | 1.0 ns/op | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| BENCHMARKS.md:25,541 | 110× | ARITH | pending `codec.txt` (x86 counterpart) |
| BENCHMARKS.md:27 | 10× | ARITH | pending `cold-cache.txt` (x86 counterpart) |
| BENCHMARKS.md:57 | 237 ns | X86-D2E688C | pending `honest-pgo-*.txt` |
| BENCHMARKS.md:58 | 910 ns · 3.8× | X86-D2E688C | pending `honest-pgo-*.txt` |
| BENCHMARKS.md:68 | 208 ns · 709 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:83,341 | 64 MB | PARAM | n/a: not a measurement |
| BENCHMARKS.md:109 | ~2.6× | ARITH | pending `realistic-flow-sparse.txt` (x86 counterpart) |
| BENCHMARKS.md:124 | 250 ns · 375 ns · 417 ns · 227 ns | ARM-M3 | pending `realistic-flow-sparse.txt` (x86 counterpart) |
| BENCHMARKS.md:158 | 261 ns · 620 ns · 1,010 ns · 2.80M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| BENCHMARKS.md:159 | 269 ns · 620 ns · 1,001 ns · 2.74M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| BENCHMARKS.md:160 | 615 ns · 1,048 ns · 3,568 ns · 1.28M ops/s | X86-D2E688C | pending `honest-native-*.txt` Path C |
| BENCHMARKS.md:163 | 237 ns · 910 ns | X86-D2E688C | pending `honest-pgo-*.txt` |
| BENCHMARKS.md:164 | 3.10M ops/s | X86-D2E688C | pending `honest-pgo-*.txt` |
| BENCHMARKS.md:171 | 1.34 | WITHDRAWN | pending `perf-prod-*.txt` (best effort: a hosted runner may expose no PMU) |
| BENCHMARKS.md:172 | 17.8 | WITHDRAWN | pending `perf-prod-*.txt` (best effort: a hosted runner may expose no PMU) |
| BENCHMARKS.md:173 | 152 | WITHDRAWN | pending `perf-prod-*.txt` (best effort: a hosted runner may expose no PMU) |
| BENCHMARKS.md:174 | 12,638 | WITHDRAWN | pending `perf-prod-*.txt` (best effort: a hosted runner may expose no PMU) |
| BENCHMARKS.md:192 | 12,638 instructions · ~9,430 cycles · ~3.25 µs | ARITH | none |
| BENCHMARKS.md:193,202,478 | 261 ns | X86-D2E688C | pending `honest-native-*.txt` |
| BENCHMARKS.md:204,405 | 10 ns · 62 ns | X86-D2E688C | none: an A/B of a past change; bench-x86 measures HEAD only |
| BENCHMARKS.md:217 | 41 ns · 42 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:221 | ~41.67 ns · 208 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:222,238,280,281 | 42 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:234 | 174 ns · 135 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:236 | ~0.34 ns · 120× | ARITH | n/a: not a measurement |
| BENCHMARKS.md:237 | 41.67 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:244 | 250 ns · 542 ns · 1,166 ns · 1,709 ns · 91,917 ns · 2.76M ops/s | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:245 | 250 ns · 500 ns · 791 ns · 1,208 ns · 43,292 ns · 2.95M ops/s | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:246 | 1,750 ns · 4,096 ns · 1,761,280 ns · 3,899,392 ns · 10,773,458 ns · 24,603 ops/s | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:251,351 | 250 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:252,387,479 | 8 ns | ARITH | pending `honest-native-*.txt` |
| BENCHMARKS.md:256 | ~125 ns | WITHDRAWN | none |
| BENCHMARKS.md:274 | 42 ns · 375 ns · 667 ns · 792 ns · 18,208 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:275 | 208 ns · 583 ns · 708 ns · 1,416 ns · 8,583 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:276 | 208 ns · 375 ns · 791 ns · 1,792 ns · 4,792 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:277 | 292 ns · 667 ns · 1,125 ns · 1,750 ns · 2,625 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:278 | 208 ns · 458 ns · 709 ns · 1,250 ns · 18,208 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:287 | 208 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:290 | 18 µs · 792 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| BENCHMARKS.md:310 | 100K/s · 1,166 ns · 1,250 ns · 4,192 ns · 5,280 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:311 | 500K/s · 834 ns · 1,000 ns · 2,334 ns · 5,536 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:312 | 1M/s · 750 ns · 1,000 ns · 1,958 ns · 6,016 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:313,332 | 2M/s | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:314,331,388 | 3M/s | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:315 | 4M/s · 583 ns · 27,136 ns · 1,458 ns · 56,576 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:316 | 6M/s · 500 ns · 16,777,216 ns · 1,333 ns · 17,039,360 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:318,321,375,426 | 1M/s | PARAM | n/a: not a measurement |
| BENCHMARKS.md:320 | 1.26× · 100K/s · 2.37× · 500K/s · 3.07× · 1M/s | ARITH | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:322 | 834 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:323 | 1,208 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:326 | 6M/s · 500 ns · 16.8 ms | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:333 | 2.6 ms · 10.4 ms · 3M/s | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:337 | 1M/s · 4M/s | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| BENCHMARKS.md:345 | 250 ns · 500 ns · 958 ns · 4,832 ns · 36,542 ns · 302 ns | ARM-M3 | pending `cold-cache.txt` (x86 counterpart) |
| BENCHMARKS.md:346 | 2,625 ns · 7,648 ns · 12,416 ns · 34,560 ns · 3,531,875 ns · 3,855 ns | ARM-M3 | pending `cold-cache.txt` (x86 counterpart) |
| BENCHMARKS.md:347 | 2,375 ns · 7,148 ns · 11,458 ns · 29,728 ns | ARITH | pending `cold-cache.txt` (x86 counterpart) |
| BENCHMARKS.md:349 | 10× · 41 ns | ARITH | pending `cold-cache.txt` (x86 counterpart) |
| BENCHMARKS.md:357 | 30 s | PARAM | n/a: not a measurement |
| BENCHMARKS.md:360 | 30.2 s · 999,987 orders/sec | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:361 | 843,520 KB · 1,186,016 KB | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:369 | ~780 ms · ~48 µs | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:371 | 1,073,741,824 ns | ARITH | n/a: not a measurement |
| BENCHMARKS.md:376 | 999,987/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:386 | 41 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:407 | 3,568 ns · 167 ns | X86-D2E688C | none: an A/B of a past change; bench-x86 measures HEAD only |
| BENCHMARKS.md:408 | 5-400x · ~185us · 9.9us · 240us · ~10us | ARM-M3 | none: an A/B of a past change; bench-x86 measures HEAD only |
| BENCHMARKS.md:418 | 780 ms · 48 µs | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:427 | 250k/s | PARAM | n/a: not a measurement |
| BENCHMARKS.md:428 | ~1M/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:429 | 400k/s · 400,000/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:431 | 400k/s · 1 s | PARAM | n/a: not a measurement |
| BENCHMARKS.md:436 | 10,688 ns · 10.4 ms · 953 MB | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:437 | ~790 ns · 7–11 µs · 953 MB · 1,186 MB | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:438 | ~850 ns · 330–415 µs | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:440 | 402k/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:441 | ~6 s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:445 | ~790 ns · 400k/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:446 | ~4,500 ns · 50k/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| BENCHMARKS.md:454 | 5 µs | PARAM | n/a: not a measurement |
| BENCHMARKS.md:464 | 1μs | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:465 | 2.7ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| BENCHMARKS.md:471 | 4x | ARITH | n/a: not a measurement |
| BENCHMARKS.md:480 | 346 ns | ARITH | pending `honest-native-*.txt` |
| BENCHMARKS.md:484 | ~42 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| BENCHMARKS.md:498 | 16 s | NOT-PERF | n/a: not a measurement |
| BENCHMARKS.md:533 | 49 B · 112.0 · 8.93 · 0.44 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| BENCHMARKS.md:534 | 49 B · 10.2 · 98.48 · 4.83 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| BENCHMARKS.md:535 | 36 B · 34.1 · 29.36 · 1.06 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| BENCHMARKS.md:536 | 32 B · 1.0 · 1015 · 32.49 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| BENCHMARKS.md:537 | 32 B · 0.5 · 2128 · 68.10 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| BENCHMARKS.md:545 | ~24 bytes | UNRECORDED | none |
| BENCHMARKS.md:553 | 5.97x | WITHDRAWN | none |

### PerformanceWhitepaper.md

| Where | Numbers | Source | Raw artifact |
|---|---|---|---|
| PerformanceWhitepaper.md:17 | 261 ns · 615 ns · 615 ns · ~125 ns · ~2.2× | X86-D2E688C, WITHDRAWN | pending `honest-native-*.txt` |
| PerformanceWhitepaper.md:44 | ~41.67 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| PerformanceWhitepaper.md:65 | 261 ns · 620 ns · 1,010 ns · 2.80M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| PerformanceWhitepaper.md:66 | 269 ns · 620 ns · 1,001 ns · 2.74M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| PerformanceWhitepaper.md:67 | 615 ns · 1,048 ns · 3,568 ns · 1.28M ops/s | X86-D2E688C | pending `honest-native-*.txt` Path C |
| PerformanceWhitepaper.md:71 | 3,568 ns | X86-D2E688C | pending `honest-native-*.txt` Path C |
| PerformanceWhitepaper.md:73 | 237 ns · 910 ns · 3.10M ops/s | X86-D2E688C | pending `honest-pgo-*.txt` |
| PerformanceWhitepaper.md:80 | 1.34 | WITHDRAWN | pending `perf-prod-*.txt` (best effort: a hosted runner may expose no PMU) |
| PerformanceWhitepaper.md:81 | 17.8 | WITHDRAWN | pending `perf-prod-*.txt` (best effort: a hosted runner may expose no PMU) |
| PerformanceWhitepaper.md:82 | 152 | WITHDRAWN | pending `perf-prod-*.txt` (best effort: a hosted runner may expose no PMU) |
| PerformanceWhitepaper.md:83 | 12,638 | WITHDRAWN | pending `perf-prod-*.txt` (best effort: a hosted runner may expose no PMU) |
| PerformanceWhitepaper.md:101 | 12,638 instructions · ~9,430 cycles · ~3.25 µs | ARITH | none |
| PerformanceWhitepaper.md:102,111,198 | 261 ns | X86-D2E688C | pending `honest-native-*.txt` |
| PerformanceWhitepaper.md:118 | ~42 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| PerformanceWhitepaper.md:120 | ~84 ns · ~1,040 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:121,248 | 615 ns | X86-D2E688C | pending `honest-native-*.txt` Path C |
| PerformanceWhitepaper.md:129 | 6,561,860 orders/sec | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:131,148,180 | 41 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:132,179 | 125 ns | WITHDRAWN | none |
| PerformanceWhitepaper.md:133 | 209 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:134 | 333 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:135 | 458 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:136 | 48,834 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:137 | 134 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:146 | 7,114,863 orders/sec | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:149 | 84 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:150 | 208 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:151 | 292 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:152 | 417 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:153 | 19,333 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:154 | 124 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:165 | 22,293 orders/sec | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:167 | 83 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:168,183 | 1,040 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:169 | 2,040 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:170 | 2,670,592 ns · 2.7 ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:171 | 3,981,312 ns · 4.0 ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:172 | 9,749,958 ns · 9.7 ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:173 | 44,768 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:181 | 956 ns | ARITH | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:191 | 1μs | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:192,250 | 2.7ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:199 | 4x | ARITH | n/a: not a measurement |
| PerformanceWhitepaper.md:204 | ~261ns | X86-D2E688C | pending `honest-native-*.txt` |
| PerformanceWhitepaper.md:215 | ~20μs · 4KB | ESTIMATE | none |
| PerformanceWhitepaper.md:216 | ~50-100μs | ESTIMATE | none |
| PerformanceWhitepaper.md:217 | ~2-10ms | ESTIMATE | none |
| PerformanceWhitepaper.md:219 | 4ms | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| PerformanceWhitepaper.md:247 | 261 ns · 237 ns | X86-D2E688C | pending `honest-pgo-*.txt`, `honest-native-*.txt` |
| PerformanceWhitepaper.md:249 | ~125 ns | WITHDRAWN | none |

### Project_Overview.md

| Where | Numbers | Source | Raw artifact |
|---|---|---|---|
| Project_Overview.md:5 | 237 ns · 125 ns | X86-D2E688C, WITHDRAWN | pending `honest-pgo-*.txt` |
| Project_Overview.md:99,342,693 | 165–190k appends/s | X86-CI-JOURNAL | journal-contention.yml, not bench-x86; `env.txt` records an O_DSYNC probe |
| Project_Overview.md:106 | ~1 ns/op | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| Project_Overview.md:119,681 | 48.2 µs · 55.3 µs · 24.1 µs | AWS-WIRE | scripts/wire_latency_aws.sh on two hosts, not bench-x86 |
| Project_Overview.md:190 | 35× | NOT-PERF | n/a: not a measurement |
| Project_Overview.md:283,380,518,563 | 41 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| Project_Overview.md:284,289,416,417 | 42 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| Project_Overview.md:288 | ~41.67 ns · 208 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| Project_Overview.md:313 | ~0.34 ns · 120× | ARITH | n/a: not a measurement |
| Project_Overview.md:314 | 41.67 ns | ARM-M3 | none: a property of the M3 clock; re-measure on a quiet Mac |
| Project_Overview.md:327 | 261 ns · 620 ns · 1,010 ns · 2.80M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| Project_Overview.md:328 | 269 ns · 620 ns · 1,001 ns · 2.74M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| Project_Overview.md:329 | 615 ns · 1,048 ns · 3,568 ns · 1.28M ops/s | X86-D2E688C | pending `honest-native-*.txt` Path C |
| Project_Overview.md:341 | 1.28M ops/s | X86-D2E688C | pending `honest-native-*.txt` Path C |
| Project_Overview.md:350 | 2.80M ops/s | X86-D2E688C | pending `honest-native-*.txt` |
| Project_Overview.md:353 | 237 ns · 910 ns · 3.10M ops/s | X86-D2E688C | pending `honest-pgo-*.txt` |
| Project_Overview.md:357 | 12,638 instructions | WITHDRAWN | pending `perf-prod-*.txt` (best effort: a hosted runner may expose no PMU) |
| Project_Overview.md:368 | 12,638 instructions · ~9,430 cycles | ARITH | none |
| Project_Overview.md:369 | ~3.25 µs · 261 ns | ARITH | none |
| Project_Overview.md:386 | 250 ns · 542 ns · 1,166 ns · 1,709 ns · 91,917 ns · 2.76M ops/s | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| Project_Overview.md:387 | 250 ns · 500 ns · 791 ns · 1,208 ns · 43,292 ns · 2.95M ops/s | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| Project_Overview.md:388 | 1,750 ns · 4,096 ns · 1,761,280 ns · 3,899,392 ns · 10,773,458 ns · 24,603 ops/s | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| Project_Overview.md:392,521 | 250 ns | ARM-M3 | pending `honest-*.txt` (x86 counterpart); the ARM rows need a quiet-Mac rerun |
| Project_Overview.md:394,564 | 8 ns | ARITH | pending `honest-native-*.txt` |
| Project_Overview.md:396 | 615 ns | X86-D2E688C | pending `honest-native-*.txt` Path C |
| Project_Overview.md:398 | ~125 ns | WITHDRAWN | none |
| Project_Overview.md:410 | 42 ns · 375 ns · 667 ns · 792 ns · 18,208 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| Project_Overview.md:411 | 208 ns · 583 ns · 708 ns · 1,416 ns · 8,583 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| Project_Overview.md:412 | 208 ns · 375 ns · 791 ns · 1,792 ns · 4,792 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| Project_Overview.md:413 | 292 ns · 667 ns · 1,125 ns · 1,750 ns · 2,625 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| Project_Overview.md:414 | 208 ns · 458 ns · 709 ns · 1,250 ns · 18,208 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| Project_Overview.md:423 | 208 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| Project_Overview.md:428 | 174 ns · 135 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| Project_Overview.md:432 | 18 µs | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| Project_Overview.md:433 | 792 ns | ARM-M3 | pending `realistic-flow.txt` (x86 counterpart) |
| Project_Overview.md:467 | 100K/s · 1,166 ns · 1,250 ns · 4,192 ns · 5,280 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:468 | 500K/s · 834 ns · 1,000 ns · 2,334 ns · 5,536 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:469 | 1M/s · 750 ns · 1,000 ns · 1,958 ns · 6,016 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:470,494 | 2M/s | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:471,493,565 | 3M/s | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:472 | 4M/s · 583 ns · 27,136 ns · 1,458 ns · 56,576 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:473 | 6M/s · 500 ns · 16,777,216 ns · 1,333 ns · 17,039,360 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:477,480,482 | 1M/s | PARAM | n/a: not a measurement |
| Project_Overview.md:479 | 1.26× · 100K/s · 2.37× · 500K/s · 3.07× | ARITH | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:483 | 834 ns · 1,208 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:486 | 6M/s · 500 ns | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:487 | 16.8 ms | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:490 | 8.5 ms | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:495 | 2.6 ms · 10.4 ms · 3M/s | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:501 | 1M/s · 4M/s | ARM-M3 | pending `co-*.txt` (x86 counterpart) |
| Project_Overview.md:508 | 64 MB | PARAM | n/a: not a measurement |
| Project_Overview.md:513 | 250 ns · 500 ns · 958 ns · 4,832 ns · 36,542 ns · 302 ns | ARM-M3 | pending `cold-cache.txt` (x86 counterpart) |
| Project_Overview.md:514 | 2,625 ns · 7,648 ns · 12,416 ns · 34,560 ns · 3,531,875 ns · 3,855 ns | ARM-M3 | pending `cold-cache.txt` (x86 counterpart) |
| Project_Overview.md:515 | 2,375 ns · 7,148 ns · 11,458 ns · 29,728 ns | ARITH | pending `cold-cache.txt` (x86 counterpart) |
| Project_Overview.md:517 | 10× | ARITH | pending `cold-cache.txt` (x86 counterpart) |
| Project_Overview.md:530 | 30.2 s · 999,987 orders/sec | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| Project_Overview.md:531 | 843,520 KB · 1,186,016 KB | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| Project_Overview.md:543,699 | ~780 ms · ~48 µs | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| Project_Overview.md:546 | 1,073,741,824 ns | ARITH | n/a: not a measurement |
| Project_Overview.md:551 | 500K/s · 1M/s | PARAM | n/a: not a measurement |
| Project_Overview.md:552 | 999,987/s | ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |
| Project_Overview.md:576,593 | 261 ns | X86-D2E688C | pending `honest-native-*.txt` |
| Project_Overview.md:578 | 0 ns · 10 ns · 62 ns · 261 ns | X86-D2E688C | none: an A/B of a past change; bench-x86 measures HEAD only |
| Project_Overview.md:591 | 10 ns · 62 ns | X86-D2E688C | none: an A/B of a past change; bench-x86 measures HEAD only |
| Project_Overview.md:600 | ~80–100 | WITHDRAWN | none |
| Project_Overview.md:601 | ~60–80 · 10 ns · 62 ns | WITHDRAWN, X86-D2E688C | none: an A/B of a past change; bench-x86 measures HEAD only |
| Project_Overview.md:602 | ~50–60 | WITHDRAWN | none |
| Project_Overview.md:603 | ~30–40 | WITHDRAWN | none |
| Project_Overview.md:605 | 0 ns · 10 ns · 62 ns · 3,568 ns · 237 ns · ~3.6 µs | X86-D2E688C | none: an A/B of a past change; bench-x86 measures HEAD only |
| Project_Overview.md:611 | 49B · 112.0 · 8.9 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| Project_Overview.md:612 | 36B · 34.1 · 29.4 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| Project_Overview.md:613 | 32B · 1.0 · 1015 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| Project_Overview.md:614 | 32B · 0.5 · 2128 | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| Project_Overview.md:616 | 0.5 ns | ARM-M3 | pending `codec.txt` (x86 counterpart) |
| Project_Overview.md:624 | ~325 cycles · 49 bytes | UNRECORDED | pending `codec.txt` (x86 counterpart) |
| Project_Overview.md:677 | 261 ns · 0 ns · 10 ns | X86-D2E688C | pending `honest-native-*.txt` |
| Project_Overview.md:678 | 3,568 ns · 167 ns | X86-D2E688C | pending `honest-native-*.txt` Path C |
| Project_Overview.md:698 | 80 ms · 100k msg/s | ARITH | n/a: not a measurement |
| Project_Overview.md:701 | 1M/s · 999,987/s | PARAM, ARM-M3 | pending `sustained-400k.txt`, `sustained-50k.txt` (x86 counterpart) |

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
| docs/CapacityPlanning.md:188 | 200-400 bytes · 4 MB/s · 40 MB/s | ESTIMATE | none |
| docs/CapacityPlanning.md:189 | 300-600 bytes · 6 MB/s · 60 MB/s | ESTIMATE | none |
| docs/CapacityPlanning.md:190 | 300 bytes · 3 MB/s · 30 MB/s | ESTIMATE | none |
| docs/CapacityPlanning.md:191 | ~500 bytes | ESTIMATE | none |
| docs/CapacityPlanning.md:192 | ~13 MB/s · ~130 MB/s | ESTIMATE | none |
| docs/CapacityPlanning.md:197 | ~50-100μs | ESTIMATE | none |
| docs/CapacityPlanning.md:198 | ~5-10μs | ESTIMATE | none |
| docs/CapacityPlanning.md:199 | ~1-2μs | ESTIMATE | none |
| docs/CapacityPlanning.md:208 | 50K orders/sec · 10μs | PARAM | n/a: not a measurement |
| docs/CapacityPlanning.md:211 | 512 MB · 248 bytes · 124 MB | ESTIMATE | none |
| docs/CapacityPlanning.md:212 | 200 GB · 86400s · 80 bytes · 345 GB | ESTIMATE | none |
