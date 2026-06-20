# Memory Ordering Audit

> Roadmap Phase 2, Issue 4, Week 5: Atomic Audit

## Summary

Systematic audit of every `std::atomic` operation across all lock-free data structures. Each entry documents: variable, operation, current memory order, justification, and recommendation.

## MpscQueue.h

| Line | Variable | Operation | Current Order | Justification | Status |
|------|----------|-----------|---------------|---------------|--------|
| 36 | `writePos_` | `store(0)` | `relaxed` | Init before any thread starts — no ordering needed | ✅ Correct |
| 56 | `writePos_` | `load` | `relaxed` | Speculative read; CAS below will fence if stale | ✅ Correct |
| 60 | `slot.sequence` | `load` | `acquire` | Must see all writes to slot data before claiming it | ✅ Correct |
| 65 | `writePos_` | `compare_exchange_weak` | `relaxed` | Only orders `writePos_` itself; the release on sequence provides the data fence | ✅ Correct |
| 67 | `slot.sequence` | `store(pos+1)` | `release` | Publishes slot data to consumer | ✅ Correct |
| 76 | `writePos_` | `load` | `relaxed` | Retry after losing CAS; no ordering needed | ✅ Correct |
| 90 | `slot.sequence` | `load` | `acquire` | Consumer must see producer's data writes | ✅ Correct |
| 98 | `slot.sequence` | `store(readPos_+mask_+1)` | `release` | Publishes slot as available to producers | ✅ Correct |

**Verdict**: MpscQueue memory orderings are correct on both x86 and ARM64.

## RingBuffer.h

| Line | Variable | Operation | Current Order | Justification | Status |
|------|----------|-----------|---------------|---------------|--------|
| 19 | `head_` | `store(0)` | `relaxed` | Init | ✅ Correct |
| 20 | `tail_` | `store(0)` | `relaxed` | Init | ✅ Correct |
| 24 | `tail_` | `load` | `relaxed` | Producer owns tail; only needs to check head | ✅ Correct |
| 27 | `head_` | `load` | `acquire` | Must see consumer's prior data read before overwriting | ✅ Correct |
| 29 | `tail_` | `store(next)` | `release` | Publishes buffer[current_tail] write | ✅ Correct |
| 36 | `head_` | `load` | `relaxed` | Consumer owns head | ✅ Correct |
| 38 | `tail_` | `load` | `acquire` | Must see producer's buffer write | ✅ Correct |
| 43 | `head_` | `store` | `release` | Publishes that we're done reading the slot | ✅ Correct |

**ABA Risk**: RingBuffer uses simple `size_t` indices that wrap via bitmask. For SPSC use (current design), this is safe because only one thread modifies each index. **No ABA protection needed for SPSC ring buffers.**

**Recommendation**: Add `static_assert(std::atomic<size_t>::is_always_lock_free)` to verify lockless on all platforms.

## FlatPriceMap.h

FlatPriceMap contains **no atomic operations**. It is single-writer (protected by `bookLock_` in OrderBook). No changes needed.

## FlatHashMap.h

FlatHashMap contains **no atomic operations**. It is not accessed concurrently — each worker thread has its own book. No changes needed.

## IntrusiveList.h

IntrusiveList contains **no atomic operations**. Protected by `bookLock_`. No changes needed.

## MatchingEngine.h / MatchingEngine.cpp

| Variable | Operations | Ordering | Status |
|----------|-----------|----------|--------|
| `running_` | load/store | `acquire/release` | ✅ Correct — controls shutdown visibility |
| `booksFrozen_` | load/store | `acquire/release` | ✅ Correct — prevents symbol mutation |
| `submittedTotal_` | `fetch_add` / `load` | `release/acquire` | ✅ Correct — drain synchronization |
| `processedTotal_` | `fetch_add` / `load` + `notify_all` | `release/acquire` | ✅ Correct — drain barrier |
| `nextSubmitSequence_` | `fetch_add` | `relaxed` | ✅ Correct — uniqueness only, no ordering |
| `droppedCount_` | `fetch_add` / `load` | `relaxed` | ✅ Correct — stats only |
| `rateLimitedCount_` | `fetch_add` / `load` | `relaxed` | ✅ Correct — stats only |
| `bpRejectCount_` | `fetch_add` / `load` | `relaxed` | ✅ Correct — stats only |
| `queueWakeups_[i]` | `fetch_add` / `wait` / `notify_one` | `release/relaxed` | ⚠️ See note below |

**Note on `queueWakeups_`**: The `wait` uses `relaxed`, which is correct because the actual data synchronization happens through the MpscQueue's sequence numbers. The wakeup is just a hint.

## main.cpp — SIGHUP Hot-Reload

| Variable | Operation | Ordering | Status |
|----------|-----------|----------|--------|
| `g_reload_config` (static atomic<bool>) | `store(true)` in signal handler | `memory_order_relaxed` | ✅ Correct — signal handlers cannot use acquire/release; relaxed is the only valid ordering in a signal handler. The main thread's `exchange(false, acq_rel)` provides the acquire fence |
| `g_reload_config` | `exchange(false)` in event loop | `memory_order_acq_rel` | ✅ Correct — ensures the main thread sees the store before acting; the release ensures config reload writes are visible after the exchange |

**Design note**: The signal handler uses `relaxed` ordering — this is intentional and correct. POSIX signal handlers are not permitted to use synchronization barriers. The acquire fence is on the consumer (main event loop), which is sufficient: once the main thread loads `true` with acquire, it sees all signal handler writes, then reloads config and stores `false` with release.

## ReplicationProtocol.h

| Variable | Operations | Ordering | Status |
|----------|-----------|----------|--------|
| `running_` (HeartbeatMonitor) | `exchange` / `load` | unordered | ⚠️ Should use `acquire/release` for visibility |
| `lastHeartbeat_` | `store` / `load` | unordered | ⚠️ Should use `release/acquire` |
| `missedCount_` | `fetch_add` / `load` | unordered | ✅ Acceptable for counters |
| `bytesSent_` / `bytesReceived_` | `fetch_add` | `relaxed` | ✅ Correct — stats |
| `receiving_` | `exchange` / `load` | unordered | ⚠️ Should use `acquire/release` |

**Recommendation for ReplicationProtocol**: Add explicit `memory_order_acquire` / `memory_order_release` to `running_`, `lastHeartbeat_`, and `receiving_` in HeartbeatMonitor and ReplicationTransport. Current code works on x86 (TSO) but may exhibit stale reads on ARM64.

## ARM64 Verification

On Apple Silicon (ARM64), all `std::atomic<uint64_t>` and `std::atomic<size_t>` operations compile to inline instructions:
- `load(acquire)` → `ldar`
- `store(release)` → `stlr`
- `fetch_add` → `ldadd` or `ldxr/stxr` loop
- `compare_exchange_weak` → `ldxr/stxr` pair

No library calls (`__atomic_load_8`) observed. Verified by inspecting `objdump -d` output on M1.

## Action Items

1. ✅ MpscQueue — no changes needed
2. ✅ RingBuffer — add `is_always_lock_free` static assert
3. ⚠️ ReplicationProtocol — fix memory orders for ARM correctness
4. ✅ All other files — no concurrent atomics
