# Order Matching Engine: Tier-1 Production Roadmap

This document outlines the architectural, operational, and regulatory delta between the current hyper-optimized C++ matching engine prototype and a **Tier-1 Institutional Production Exchange** (e.g., NASDAQ, CME, or a top-tier Alternative Trading System).

While the core algorithmic latency (~40ns adds, ~70ns matches) is state-of-the-art for user-space, deploying a real financial venue requires solving hardware networking, distributed consensus, and rigorous mathematical verification.

---

## 1. True Kernel Bypass & Networking
To achieve physical wire-to-wire low latency, the OS networking stack must be entirely bypassed.
- [ ] **DPDK / Solarflare ef_vi:** Implement direct NIC-to-user-space memory access. (Note: `io_uring` is kernel-assisted, not true bypass).
- [ ] **Binary Wire Protocols:** Transition from the text-based FIX parser to binary protocols like SBE (Simple Binary Encoding), NASDAQ ITCH/OUCH, or CME MDP 3.0.
- [ ] **FIX Session Layer State Machine:** Implement full session management including heartbeat, test-request, resend-request, gap-fill, sequence-reset, and logon-auth state machines.
- [ ] **Multicast Market Data:** Replace local SHM IPC with UDP + PIM-SM Multicast for market data, ensuring A/B feed redundancy and snapshot + incremental recovery.
- [ ] **PCAP Tooling:** Build packet capture (PCAP) and replay tooling for zero-latency backtesting and postmortem analysis.

## 2. Distributed Consensus & High Availability (HA)
A production exchange must survive catastrophic hardware failure without losing a single financial transaction.
- [ ] **Replicated State Machine:** Implement a consensus sequencer (e.g., Raft, Aeron Cluster, or a Chronicle-style sequencer) with N replicas.
- [ ] **Hot Standby & Failover:** Ensure deterministic state replay from the sequencer for instant failover.
- [ ] **Cross-DC Disaster Recovery:** Establish asynchronous replication to a secondary data center.
- [ ] **Split-Brain Protection:** Implement rigorous leader election to prevent duplicate trade matching during network partitions.

## 3. Advanced Exchange Semantics
Real-world venues require complex business logic and state transitions to manage market integrity.
- [ ] **Auction State Machines:** Implement Pre-Open, Opening/Closing crosses, Volatility Auctions, and Halt-Resume mechanics.
- [ ] **Market Protection:** Implement price collars and market-order protection (preventing sweeps that clear the entire book).
- [ ] **Advanced Order Types:** Support Implied, Spread, Cross, Complex orders, OCO/OSO/Bracket, MIT, MOC/LOC, and Peg-to-Primary.
- [ ] **Venue-Specific Allocation:** Formalize priority tiers, LMM/DMM privileges, and exact Iceberg refresh / hidden-order allocation priorities.
- [ ] **Expanded SMP Variants:** Add Cancel-Maker, Decrement-and-Cancel, and hierarchy-aware Self-Match Prevention (currently only Cancel-Taker).

## 4. Risk, Clearing, and Compliance
Handling institutional money requires rigorous pre-trade and post-trade guardrails.
- [ ] **Multi-Tier Limits:** Persist limits across Firm, Account, Strategy, and Trader hierarchies.
- [ ] **Pre-Trade Fat-Finger Checks:** Block orders based on maximum notional size, reference price limits, and percentage away from the NBBO.
- [ ] **Regulatory Audit Trail:** Support CAT (Consolidated Audit Trail), MiFID II transaction reports, and RTS 24/25 record retention.
- [ ] **Clock Synchronization:** Achieve PTP / NTP clock sync to MiFID II RTS 25 microsecond tolerances.
- [ ] **Clearing Handoff (STP):** Build post-trade Straight-Through Processing (STP) pipelines.
- [ ] **Fee/Rebate Engine:** Real-time calculation of maker-taker schedules.

## 5. Formal Verification & QA
Unit tests are insufficient for lock-free concurrency. The engine must survive mathematical and adversarial scrutiny.
- [ ] **TLA+ / Spin Modeling:** Mathematically model and prove the safety of the MPSC queues, sequencer, and journal interactions.
- [ ] **Long-Running Soak Tests:** Run the engine for extended periods with deterministic seed persistence to catch 1-in-a-billion concurrency bugs.
- [ ] **Coverage-Guided Fuzzing:** Utilize `libFuzzer` or `AFL` on the FIX parser and journal replay systems.
- [ ] **Chaos / Jepsen Fault Injection:** Randomly kill nodes, drop packets, and corrupt journal writes to verify system recovery.
- [ ] **Byte-Identical Determinism:** Ensure a replay harness guarantees byte-for-byte identical state reconstruction across runs.

## 6. Observability, Metrics & Ops
If you cannot measure it in production, you cannot operate it.
- [ ] **Structured Logging:** Implement stable, JSON/Protobuf event schemas for centralized log ingestion.
- [ ] **Prometheus / OpenTelemetry:** Export real-time metrics for order flow, reject rates, queue depth, and journal health.
- [ ] **Hardware Tuning Runbooks:** Document exact OS-level tuning including NUMA pinning, hugepages allocation, IRQ steering, and CPU core isolation.
- [ ] **Configuration Management:** Abstract symbols, ports, limits, and persistence rules into managed configuration files.
- [ ] **Protocol Versioning:** Ensure backward-compatible migrations for all client-facing protocols.

## 7. Performance Methodology Upgrade
Benchmarking must move beyond localized user-space averages.
- [ ] **HdrHistogram Tracking:** Implement coordinated-omission-free end-to-end latency tracking.
- [ ] **Hardware Perf Counters:** Wire LLC misses and branch mispredictions directly into the benchmark suite.
- [ ] **Linux Isolated Cores Validation:** Transition benchmarks from macOS/M-series to production Linux servers with isolated cores and disabled hyperthreading.
