# Order Matching Engine - Performance Notes

## 1. Executive Summary
This document records local performance measurements for the C++ order matching engine. The benchmark suite measures individual operation latency distributions instead of relying only on batch-averaged throughput. The numbers are useful for regression tracking and design comparison, but they are not a portable production latency SLA.

## 2. Methodology

### 2.1 Individual Timing
We time every engine operation independently using `std::chrono::high_resolution_clock`.
- **Refinement**: No block-averaging. This captures the true "jitter" and cost of individual map insertions and matching loops.
- **Hardware Constraint**: On Apple Silicon, the user-space counter operates at 24MHz (~41.6ns). Our results reflect this hardware quantization.

For reproducible local runs, use:

```bash
./scripts/run_benchmarks.sh
```

Record CPU model, OS version, compiler version, power mode, thermal state, and repeated-run variance with any published result.

### 2.2 Spread-Crossing Matches
'Match' benchmarks are only valid if they generate trades. 
- **Validation**: Our matching benchmark crossed the spread for 20,000 operations and produced **20,197 trades on resting liquidity**.
- **Safety**: All matching was executed within the 5% Circuit Breaker band to ensure the engine was not in a halted state.

## 3. Results (Local Apple Silicon Run)

| Metric | Adds (Limit) | Matches (Aggressive) | Cancels |
| :--- | :--- | :--- | :--- |
| **Average Latency** | **83.2 ns** | **131.7 ns** | **105.0 ns** |
| **P50 (Median)** | 83 ns | 125 ns | 83 ns |
| **P99** | 167 ns | 209 ns | 417 ns |
| **Throughput** | 12.02 M ops/s | 7.59 M ops/s | 9.52 M ops/s |

## 4. Architectural Analysis
- **Add Latency (<50ns in LEAN)**: Optimized via `FlatPriceMap` array indexing and `FlatHashMap` open-addressing ($O(1)$) and `ObjectPool` retrieval.
- **Match Latency (<100ns in LEAN)**: Includes the `match()` loop, analytics updates (VWAP), and `Trade` history recording.
- **Cancel Latency (<50ns in LEAN)**: Includes monotonic ID lookup and intrusive list removal.

## 5. Conclusion
The current engine demonstrates low-latency matching behavior on the tested local machine and has regression, stress, crash-recovery, and property tests. Before making production HFT claims, the project still needs venue-specific semantics, longer soak tests, sanitizer/TSAN coverage, reproducible benchmark reporting across machines, and operational failure-mode testing.
