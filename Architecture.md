# Order Matching Engine - Architecture & Design

This document details the architectural decisions, component lifecycles, failure modes, and engineering trade-offs of the C++20 Order Matching Engine. It serves as a guide for contributors, system architects, and researchers exploring high-performance trading infrastructure.

---

## 1. Design Philosophy & Key Trade-offs

The architecture is driven by three primary constraints: **predictable low latency**, **deterministic execution**, and **high availability**.

### Why Not Traditional Mutexes?
A naive matching engine protects the order book with a `std::mutex`. Under high load, threads spend more time context-switching and fighting for the lock than executing trades. 
**Our Trade-off:** We use a **Thread-per-Symbol (Sharded) Model**. Each thread owns specific instruments exclusively. Producers (Gateway/API) push orders into lock-free Multi-Producer Single-Consumer (`MpscQueue`) ring buffers. The matching thread never blocks on locks.

### Why Not `std::map`?
`std::map` is a Red-Black tree. Traversing it means jumping across random heap allocations, destroying CPU cache locality.
**Our Trade-off:** We built `FlatPriceMap`, a contiguous array indexed directly by price ticks. Finding the next best price is an $O(1)$ pointer arithmetic operation. It costs more memory (pre-allocating the price range) but guarantees cache-hot O(1) lookups.

### Why Intrusive Lists?
Using `std::list` or `std::vector` for orders at a price level requires dynamic heap allocations (`new`/`delete`), which induce unpredictable OS-level latency spikes.
**Our Trade-off:** We use an `ObjectPool` to pre-allocate millions of `Order` structs at startup. The `Order` struct itself contains the `next` and `prev` pointers. When an order is added, it is simply linked into the `IntrusiveList`. Zero heap allocations happen on the hot path.

---

## 2. System Topology & Data Flow

```mermaid
graph TD
    subgraph "Order Entry — Four Protocols, One Engine"
        FIX[FIX 4.2 / 4.4 Session]
        OUCH[OUCH 4.2 + SoupBinTCP]
        SBE[SBE Schema-Driven]
        OuchGW[OuchTcpGateway - real TCP]
    end

    subgraph "Ingress Layer"
        RL[Token Bucket Rate Limiter]
        BR[Batch Risk Validator]
    end

    subgraph "Concurrency / Routing"
        Hash[Symbol Hash Router]
        Q1[Lock-Free MPSC Queue 0]
        Q2[Lock-Free MPSC Queue 1...N]
    end

    subgraph "Worker Thread (Core Affinity)"
        CB[LULD / Circuit Breakers]
        OB[OrderBook Core]
        OB --> |Bids/Asks| FPM[FlatPriceMap]
        FPM --> |Levels| IL[IntrusiveList of Orders]
    end

    subgraph "Persistence & HA"
        WAL[CRC-32 Journal / WAL]
        REP[Replication Coordinator]
        BK[Backup Node]
    end

    subgraph "Market Data Egress"
        MUX[MultiplexListener Fan-out]
        ITCH[ItchPublisher - per book]
        SHM[MarketDataPublisher - SHM]
        UDP[ItchUdpPublisher - MoldUDP64]
        JRN[MoldPacketJournal]
        RETX[ItchRetransmissionService - SoupBinTCP/TCP]
        Admin[Admin Server :8080]
    end

    FIX --> RL
    OUCH --> OuchGW --> RL
    SBE --> RL
    RL --> BR --> Hash
    Hash -->|BTC/ETH| Q1
    Hash -->|SOL/AVAX| Q2
    Q1 --> CB --> OB
    OB --> WAL
    WAL --> REP --> BK
    OB --> MUX
    MUX --> ITCH
    MUX --> SHM
    ITCH --> UDP
    ITCH --> JRN
    JRN --> RETX
```

### The Component Lifecycle

**Order entry**:
1. **Wire framing**: bytes arrive on TCP. The connection's session class (`FixSession` for FIX 4.2/4.4 text, `OuchSession` for NASDAQ OUCH binary, `SbeSession` for FIX-TG SBE) handles framing. OUCH is additionally wrapped in `SoupBinTcpSession` (3-byte length+type envelope, login/heartbeat/logout).
2. **Pre-Trade Risk**: the order passes through `RateLimiter` and `BatchRiskValidator` (notional, fat-finger, kill-switch).
3. **Routing**: `MatchingEngine` hashes `SymbolId` and pushes the order into the appropriate thread's `MpscQueue`.
4. **Dequeue & Validate**: the worker thread pops the order. `LULDManager` (Limit Up-Limit Down) bands verify the instrument isn't halted.
5. **Matching**: `OrderBook` matches against resting liquidity. `WashTradeDetector` applies Self-Trade Prevention.
6. **Persistence**: trades and book updates are appended to the `Journal` (Write-Ahead Log).

**Market data egress** (parallel to order entry):
7. **Fan-out**: every engine event reaches `MultiplexListener`, which dispatches to all registered `EventListener`s simultaneously.
8. **ITCH encoding**: `ItchPublisher` translates engine events to ITCH 5.0 frames (`A` AddOrder, `E` Executed, `D` Delete, `H` TradingAction reflecting `TradingState`, `R` StockDirectory, `Q` CrossTrade, etc.).
9. **MoldUDP64 publish**: `ItchUdpPublisher` wraps each frame in a sequenced MoldUDP64 packet and `sendto`s a UDP multicast group. The same frame is recorded in `MoldPacketJournal` keyed by the assigned sequence number.
10. **Gap recovery**: a subscriber that detects a sequence gap connects to `ItchRetransmissionService` (TCP, SoupBinTCP-framed), sends a re-request packet (`R` tag + start/count), and receives the missing frames byte-exactly as journaled.

The order-entry and market-data paths share no synchronous coupling — a subscriber that's slow or absent never backpressures order matching.

---

## 3. Deep Dive: Core Components

### 3.1 OrderBook & Data Structures
The core of the engine is purely single-threaded and deterministic.
- **`Order`**: A 128-byte packed struct containing ID, price, quantity, participant ID, and intrusive pointers.
- **`FlatPriceMap`**: A fixed-size array covering the valid tick range of an instrument. If BTC trades from \$0 to \$100,000 at \$0.01 increments, the map has 10,000,000 slots. Memory is cheap; cache misses are expensive.
- **`FlatHashMap`**: Used for O(1) order cancellation by `OrderId`. It uses Robin-Hood open-addressing to maintain cache density.

### 3.2 ShardedOrderBook & Horizontal Scaling
To scale beyond a single core, the engine utilizes the `ShardedOrderBook`.
Instead of placing all instruments in one book, instruments are sharded across $N$ instances. Each worker thread runs an independent event loop pinned to a specific CPU core using `CPUAffinity`. Cross-symbol interactions are heavily restricted to maintain this isolation.

### 3.3 ShadowEngine (Dark Launching)
For algorithmic testing and capacity planning, the `ShadowEngine` allows cloning production state. It consumes production traffic (via a fork or replay) but suppresses all outbound executions and market data. By taking a read-only snapshot of the production book and replaying subsequent events from a specific journal offset, the shadow instance accurately mirrors live execution logic without corrupting the production order book or impacting live network egress. This allows safe, realistic load testing of new engine versions against live market flow.

### 3.4 Regulatory & Compliance Controls
- **WashTradeDetector (SMP)**: Prevents participants from matching against themselves.
- **LULDManager**: Dynamically updates acceptable price bands based on recent trade prices, halting matching if prices gap too quickly.
- **GraduatedKillSwitch**: Automatically disables trading for participants who breach soft/hard risk limits.

### 3.5 Wire Protocol Stack

Four wire protocols dispatch into the same `MatchingEngine`. The session class is the only thing that changes between them; the engine doesn't know which codec it's driving.

| Protocol | Spec family | Wire format | Endianness | Session layer | Used by |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **FIX 4.2 / 4.4** | FIX Trading Community | ASCII tag=value with SOH delimiter + checksum | n/a (text) | `FixSession.h` — Logon/Heartbeat/ResendRequest state machine, OrdRejReason mapping, TransactTime validation on 4.4 inbound | Most B-Ds and retail brokers |
| **OUCH 4.2** | NASDAQ | Fixed-width binary, big-endian, ASCII-decimal slots | BE | `OuchSession.h` + `SoupBinTcpSession.h` (3-byte envelope, login, heartbeat, logout) | NASDAQ direct order entry |
| **ITCH 5.0** | NASDAQ | Fixed-width binary, big-endian | BE | One-way feed (publish only) — `ItchPublisher.h` + `ItchUdpTransport.h` wrapping `MoldUDP64.h` for multicast | NASDAQ market data |
| **SBE** | FIX-TG | Schema-driven binary, length-prefixed blocks, **forward-compatible** | LE (native) | `SbeSession.h` — versioned `MessageHeader`, `templateId` dispatch | CME, ICE, several modern venues |

**SBE forward-compatibility, in detail**: every SBE message starts with an 8-byte `MessageHeader` that includes the `blockLength` of the fixed-size block that follows. When a v2 schema adds a field at the end of a block:
- v2 publishers emit `blockLength=v2_size`. Their messages include the new field.
- v1 readers ignore the unknown trailing bytes (they only read up to `v1_size`).
- v1 readers see the original fields **byte-identically** because the v1 block IS a prefix of the v2 block. No codec branch, no version dispatch — just `read up to my known size`.
- v2 readers handle a v1 message by using `blockLength=v1_size` from the header to skip the absent new fields; they fill in documented defaults.

This is proven byte-exactly via test in `SbeProtocolTest`: encoding a struct with the v2 codec, then byte-comparing the first `v1_size` bytes against the v1 encoding of the same field set, asserts equality. This is the property that makes SBE viable for incremental schema evolution at production scale.

**Gap-recovery flow** (market data side):
1. `MoldUDP64Publisher` assigns a sequence number to each ITCH frame as it enters the batch.
2. `ItchUdpPublisher::publish()` writes the MoldUDP64 datagram via `sendto`.
3. The same frame is appended to `MoldPacketJournal` keyed by its sequence.
4. A subscriber notices `received_seq > expected_seq` and fires `onGapDetected(expected, received)`.
5. The subscriber's gap handler opens a TCP connection to `ItchRetransmissionService`, completes SoupBinTCP login, and sends a re-request: `[type='R' | start_seq | count]`.
6. The service looks up the range in `MoldPacketJournal` and streams each found message back as a SoupBinTCP `SequencedData` packet.
7. The journal is bounded (ring of N most-recent messages); a subscriber too far behind hits the eviction wall and must take a full snapshot from the Glimpse/snapshot service (not implemented here — that's a separate venue concern).

---

## 4. Resilience & High Availability

To achieve institutional-grade uptime, the engine implements a Primary-Backup HA model.

### 4.1 Write-Ahead Logging (CRC-32 Journal)
Before sending execution reports to the client, the engine writes the state mutation to a binary `Journal`. 
- Every entry has a CRC-32 checksum.
- A virtual clock (GTD/DAY expirations) ensures time-based events replay deterministically.
- `Checkpointing`: Periodically, the engine writes a compressed snapshot of the entire resting book to disk and prunes the old journal to bound recovery time.

### 4.2 Replication Coordinator & Epoch Fencing
The `ReplicationCoordinator` manages TCP log-shipping to the `JournalFollower` (Backup Node).
- **Heartbeats**: Used for failure detection.
- **LeaderLease Fencing**: The primary holds an epoch-based lease. If it fails, the backup promotes itself, increments the epoch, and rejects any delayed packets from the old primary (preventing split-brain).

### 4.3 Formal Verification
Concurrency is notoriously difficult to test. We rely on **TLA+ Specifications**:
- `MpscQueue.tla`: Verifies linearizability and lack of race conditions in our lock-free ring buffer.
- `Replication.tla`: Verifies that leader election and log shipping never result in data loss or split-brain.
- `EngineConsumer.tla`: Verifies worker thread shutdown safety.

---

## 5. Failure Modes & Mitigations

Designing for low latency means knowing exactly how the system behaves when under stress.

| Failure Mode | Impact | Mitigation |
| :--- | :--- | :--- |
| **Microburst / News Event** | High ingress rate overwhelms matching thread. | `MpscQueue` depth triggers **Queue Backpressure**. Gateway immediately replies with `RejectReason::QueueFull`. Rate limiting throttles at the ingress edge. |
| **Disk I/O Stall** | Journal fsync() blocks the matching thread. | Journal uses `SyncPolicy::GroupCommit` or asynchronous OS buffers. Engine relies on network HA replication for strict durability rather than local fsync. |
| **Primary Server Crash** | Connection dropped, engine stops. | Backup node detects missing heartbeats, promotes itself via `JournalFollower::promote()`. Clients reconnect to backup gateway. |
| **Memory Exhaustion** | System runs out of RAM under load. | `ObjectPool` pre-allocates everything at startup. `CapacityMonitor` triggers soft-rejects if pool utilization exceeds 90%, preventing OS OOM killer. |
| **Corrupted Disk / Bit Flip** | Journal file is mutated by hardware failure. | CRC-32 validation on every read. `JournalReplay` halts exactly before the corrupted entry. Data is recovered from the replication peer. |

## 6. Conclusion
The Order Matching Engine prioritizes mechanical sympathy. By aligning data structures with CPU cache lines, eliminating heap allocations on the hot path, and isolating state behind lock-free message passing, the engine achieves deterministic sub-100ns processing times while retaining the necessary regulatory and HA infrastructure required by modern exchanges.
