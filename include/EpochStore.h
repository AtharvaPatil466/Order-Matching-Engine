#pragma once

// EpochStore — durable, monotonic leader-lease epoch persistence.
//
// The replication layer fences a stale primary with a uint64_t epoch: a
// higher epoch always wins, so a node that comes back from a network
// partition with a lower epoch is rejected. Historically the epoch lived
// only in memory, which meant a restarted node reset to 0 and could
// REGRESS its epoch below a value it had already advertised to peers — a
// split-brain hazard (two nodes believing they own the same epoch range).
//
// EpochStore removes that hazard by persisting the epoch to disk durably:
// after store() returns, the value survives a process crash (and, with the
// fsync barrier below, an OS crash / power loss as well). On startup the
// node calls load() to recover its last-known epoch instead of starting
// from 0.
//
// Monotonicity is NOT enforced here. The lease only ever bumps the epoch
// upward, so EpochStore simply persists and loads the value faithfully;
// the caller owns the "never go backwards" invariant.
//
// On-disk format: the 8 raw bytes of the uint64_t in host (little-endian on
// our targets) byte order, written without any framing. The fixed 8-byte
// width is what lets load() distinguish a complete record from a torn /
// truncated write — any file whose size is not exactly 8 bytes is treated
// as absent/corrupt and reported as epoch 0.
//
// Depends only on the standard library + POSIX I/O (no engine headers), so
// it can be unit-tested and reused in isolation.

#include <cstdint>
#include <string>

namespace OrderMatcher {

class EpochStore {
public:
    explicit EpochStore(std::string path);

    // Durably persist `epoch`. Implementation: write the 8 bytes to a
    // sibling "<path>.tmp", fsync that file descriptor, then ::rename it
    // over the real path. The rename is atomic on POSIX, so a reader (or a
    // restart) ever sees either the OLD complete value or the NEW complete
    // value — never a half-written file. The fsync before the rename is the
    // durability barrier: it forces the bytes to stable storage so the
    // value survives a crash that happens immediately after store()
    // returns. Uses F_FULLFSYNC on Apple and fdatasync on Linux, matching
    // the Journal's durability strategy for each platform.
    void store(uint64_t epoch);

    // Read the persisted epoch. Returns 0 when the file is missing, empty,
    // the wrong size (a torn write), or otherwise unreadable. Never throws
    // and never invokes undefined behavior — a fresh or corrupted node
    // simply starts at epoch 0 and lets the cluster re-establish a leader.
    uint64_t load() const;

    const std::string& path() const;

private:
    std::string path_;
};

}  // namespace OrderMatcher
