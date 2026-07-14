#pragma once

// MoldUDP64 — Nasdaq's UDP multicast wrapper for ITCH-style market data. One
// datagram = one packet carrying an ordered batch of application messages, with
// a header giving the session (detect venue restart), the sequence number of
// the FIRST message (subscribers track next-expected; a gap triggers
// re-request), and the message count (batching for throughput).
//
// Wire format (network byte order):
//   Session          [10 bytes ASCII, space-padded]
//   SequenceNumber   [8 bytes uint64 BE]
//   MessageCount     [2 bytes uint16 BE]
//   then N × { MessageLength [2 bytes uint16 BE] + MessageData }
//
// Special MessageCount: 0 = heartbeat (no messages, ~1/s, distinguishes quiet
// market from partition); 0xFFFF = end-of-session (terminal).
//
// Gap recovery is not via multicast (UDP — gone is gone): subscribers request
// missing ranges from a separate SoupBinTCP-style Request Server. This codec
// produces the multicast frames only; the recovery channel is the caller's.

#include "OuchProtocol.h"  // readU16BE, readU32BE, writeU16BE, writeU64BE, writeFixedAscii

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OrderMatcher {

constexpr size_t   MOLD_SESSION_BYTES   = 10;
constexpr size_t   MOLD_HEADER_BYTES    = MOLD_SESSION_BYTES + 8 + 2;  // 20
constexpr uint16_t MOLD_HEARTBEAT       = 0;
constexpr uint16_t MOLD_END_OF_SESSION  = 0xFFFF;

// Bound on packet size. The protocol is byte-addressable up to 64KB,
// but typical multicast MTU is ~1500; venues usually flush far below
// that to avoid IP fragmentation. We use 1500 by default but allow
// callers to override.
constexpr size_t MOLD_DEFAULT_MTU = 1500;

// ─── Header read/write ──────────────────────────────────────────────────────

inline void moldWriteHeader(uint8_t* out, std::string_view session,
                            uint64_t sequenceNumber, uint16_t messageCount) {
    writeFixedAscii(out, MOLD_SESSION_BYTES, session);
    writeU64BE(out + 10, sequenceNumber);
    writeU16BE(out + 18, messageCount);
}

struct MoldHeader {
    std::string session;
    uint64_t    sequenceNumber;
    uint16_t    messageCount;
};

inline bool moldReadHeader(const uint8_t* data, size_t len, MoldHeader& out) {
    if (len < MOLD_HEADER_BYTES) return false;
    // Trim trailing spaces from the session field.
    size_t end = MOLD_SESSION_BYTES;
    while (end > 0 && data[end - 1] == ' ') --end;
    out.session = std::string(reinterpret_cast<const char*>(data), end);
    out.sequenceNumber = readU64BE(data + 10);
    out.messageCount   = readU16BE(data + 18);
    return true;
}

// ─── Publisher ──────────────────────────────────────────────────────────────
// Accumulates application messages into a buffered batch, then flushes
// the batch as a single MoldUDP64 packet via the sink callback. Three
// flush triggers: batched-size threshold, explicit flush(), or special
// packets (heartbeat / end-of-session).

class MoldUDP64Publisher {
public:
    using SendBytes = std::function<void(std::string_view)>;
    // Called on each group commit (flush / end-of-session) with the new
    // high-water mark = the sequence number the NEXT packet will carry.
    // Persisting this durably (e.g. via EpochStore) lets a restarted
    // publisher resume the counter instead of resetting to 1 — see
    // setNextSequence() and P3-3.
    using SeqPersistFn = std::function<void(uint64_t)>;

    MoldUDP64Publisher(std::string session, SendBytes send,
                       size_t mtu = MOLD_DEFAULT_MTU)
        : session_(std::move(session)), send_(std::move(send)), mtu_(mtu),
          nextSeq_(1) {
        if (session_.size() > MOLD_SESSION_BYTES) {
            session_.resize(MOLD_SESSION_BYTES);
        }
    }

    // Override the starting sequence number. Subscribers reconnecting
    // after a gap-recovery cycle may need replay from a specific
    // sequence; production deployments persist the high-water mark.
    //
    // On a RESTART, load the persisted high-water and call this so the
    // feed resumes contiguously — a reconnecting subscriber that held
    // seq N-1 sees N next, not a false gap back to 1. We also anchor the
    // persistence floor here so a resumed counter never regresses on
    // disk (monotonicity, matching spec/EpochDurability.tla).
    void setNextSequence(uint64_t seq) {
        nextSeq_ = seq;
        if (seq > lastPersisted_) lastPersisted_ = seq;
    }
    uint64_t nextSequence() const { return nextSeq_; }

    // Install the durability hook for the sequence high-water mark. The
    // callback fires on every group commit with a strictly increasing
    // value, so a durable sink (EpochStore) never records a regression.
    void setSequencePersistence(SeqPersistFn fn) { persistSeq_ = std::move(fn); }

    // Queue an application message into the current batch. Returns
    // the sequence number assigned to this message. If adding the
    // message would exceed the MTU, the current batch is flushed
    // first (and this message starts a new batch).
    uint64_t addMessage(const void* payload, uint16_t len) {
        size_t needed = sizeof(uint16_t) + len;
        if (batchedMessageBytes_ + needed > mtu_ - MOLD_HEADER_BYTES &&
            !pending_.empty()) {
            flush();
        }
        uint64_t seqAssigned = nextSeq_ + pending_.size();
        // Encode the length + payload directly into the pending buffer.
        size_t off = pendingBuf_.size();
        pendingBuf_.resize(off + needed);
        writeU16BE(pendingBuf_.data() + off, len);
        if (len > 0 && payload) {
            std::memcpy(pendingBuf_.data() + off + 2, payload, len);
        }
        pending_.push_back({static_cast<uint16_t>(off), len});
        batchedMessageBytes_ += needed;
        return seqAssigned;
    }

    // Flush the pending batch as a single MoldUDP64 packet.
    // Idempotent — flush() on an empty batch is a no-op (heartbeats
    // have a separate dedicated API).
    void flush() {
        if (pending_.empty()) return;
        emit(static_cast<uint16_t>(pending_.size()),
             pendingBuf_.data(), pendingBuf_.size());
        nextSeq_ += pending_.size();
        pending_.clear();
        pendingBuf_.clear();
        batchedMessageBytes_ = 0;
        ++packetsEmitted_;
        // Group commit: persist the new high-water AFTER the packet is on
        // the wire. Crashing between send and persist leaves the store one
        // batch behind, so a restart re-sends already-delivered seqs
        // (subscribers dedup) — never skips ahead into a false gap.
        persistHighWater();
    }

    // Send a heartbeat packet (MessageCount=0). Used by the venue to
    // assure subscribers the feed is alive during quiet markets. The
    // sequence number on a heartbeat is the next-to-be-published
    // (i.e., the SAME number that will appear on the next real
    // packet) — subscribers use this as a liveness check without
    // consuming a sequence slot.
    void sendHeartbeat() {
        // Flush any pending messages first so heartbeat doesn't
        // overtake real data.
        flush();
        emit(MOLD_HEARTBEAT, nullptr, 0);
        ++heartbeatsEmitted_;
    }

    // Send an end-of-session marker (MessageCount=0xFFFF). Terminal:
    // subscribers disconnect.
    void sendEndOfSession() {
        flush();
        emit(MOLD_END_OF_SESSION, nullptr, 0);
        ++endOfSessionsEmitted_;
        // Persist the terminal high-water so a next-session restart that
        // shares this counter resumes above it.
        persistHighWater();
    }

    uint64_t packetsEmitted()       const { return packetsEmitted_; }
    uint64_t heartbeatsEmitted()    const { return heartbeatsEmitted_; }
    uint64_t endOfSessionsEmitted() const { return endOfSessionsEmitted_; }
    size_t   pendingCount()         const { return pending_.size(); }
    const std::string& session()    const { return session_; }

private:
    struct PendingMessage { uint16_t offset; uint16_t length; };

    // Persist the high-water mark durably, but only when it advances —
    // the value handed to the sink is strictly monotonic, so a durable
    // store never records a regression even across restarts.
    void persistHighWater() {
        if (persistSeq_ && nextSeq_ > lastPersisted_) {
            lastPersisted_ = nextSeq_;
            persistSeq_(nextSeq_);
        }
    }

    void emit(uint16_t messageCount, const uint8_t* body, size_t bodyLen) {
        if (!send_) return;
        std::vector<uint8_t> packet(MOLD_HEADER_BYTES + bodyLen);
        moldWriteHeader(packet.data(), session_, nextSeq_, messageCount);
        if (bodyLen > 0 && body) {
            std::memcpy(packet.data() + MOLD_HEADER_BYTES, body, bodyLen);
        }
        send_(std::string_view(reinterpret_cast<const char*>(packet.data()),
                               packet.size()));
    }

    std::string                  session_;
    SendBytes                    send_;
    SeqPersistFn                 persistSeq_;
    size_t                       mtu_;
    uint64_t                     nextSeq_;
    uint64_t                     lastPersisted_{0};
    std::vector<PendingMessage>  pending_;
    std::vector<uint8_t>         pendingBuf_;
    size_t                       batchedMessageBytes_{0};
    uint64_t                     packetsEmitted_{0};
    uint64_t                     heartbeatsEmitted_{0};
    uint64_t                     endOfSessionsEmitted_{0};
};

// ─── Subscriber ─────────────────────────────────────────────────────────────
// Parses incoming MoldUDP64 packets, detects sequence gaps, and
// dispatches each application message via callback. A real
// subscriber would also drive the gap-recovery TCP request channel
// when a gap is detected; that's wired by the caller from the
// onGapDetected callback.

class MoldUDP64Subscriber {
public:
    using OnMessage     = std::function<void(uint64_t seq, const uint8_t* data, size_t len)>;
    using OnGapDetected = std::function<void(uint64_t expectedSeq, uint64_t receivedSeq)>;
    using OnEndOfSession = std::function<void()>;

    MoldUDP64Subscriber() = default;

    void setExpectedSession(std::string session) {
        expectedSession_ = std::move(session);
    }
    void setOnMessage(OnMessage cb)         { onMessage_ = std::move(cb); }
    void setOnGapDetected(OnGapDetected cb) { onGap_ = std::move(cb); }
    void setOnEndOfSession(OnEndOfSession cb) { onEos_ = std::move(cb); }

    // Feed one whole UDP datagram. Returns false on structural error
    // (header missing / message length overruns); the caller can
    // count these for diagnostics. Sequence gaps DON'T return false —
    // they fire onGapDetected and the subscriber resumes from the
    // newly-received sequence (the recovery channel is the caller's
    // responsibility).
    bool feedPacket(const uint8_t* data, size_t len) {
        MoldHeader hdr;
        if (!moldReadHeader(data, len, hdr)) {
            framingErrors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!expectedSession_.empty() && hdr.session != expectedSession_) {
            // Different session ID — venue restart or wrong feed.
            sessionMismatches_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (hdr.messageCount == MOLD_END_OF_SESSION) {
            if (onEos_) onEos_();
            return true;
        }
        if (hdr.messageCount == MOLD_HEARTBEAT) {
            // Heartbeat carries no messages and does NOT consume a
            // sequence number. Use the seq field as the "next expected"
            // to detect long-duration silence gaps.
            uint64_t expected = nextExpectedSeq_.load(std::memory_order_relaxed);
            if (hdr.sequenceNumber > expected) {
                if (onGap_) onGap_(expected, hdr.sequenceNumber);
                nextExpectedSeq_.store(hdr.sequenceNumber, std::memory_order_relaxed);
                gapsObserved_.fetch_add(1, std::memory_order_relaxed);
            }
            heartbeatsReceived_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Real-data packet. The header's sequence is the seq of the
        // FIRST message in this packet.
        uint64_t firstSeq = hdr.sequenceNumber;
        uint64_t expected = nextExpectedSeq_.load(std::memory_order_relaxed);
        if (firstSeq > expected && expected >= gapReportedTo_) {
            // A genuine new hole opens below this packet — report it
            // EXACTLY ONCE. While the gap is still outstanding (the
            // contiguous front hasn't caught up to gapReportedTo_),
            // later live packets — and the gap packet itself if re-seen
            // — must NOT keep re-firing onGapDetected. The hole is
            // closed by recovered / retransmitted messages flowing back
            // through feedPacket below, which advance nextExpectedSeq_.
            if (onGap_) onGap_(expected, firstSeq);
            gapsObserved_.fetch_add(1, std::memory_order_relaxed);
            gapReportedTo_ = firstSeq;
        }
        // NOTE: nextExpectedSeq_ is deliberately NOT bumped to firstSeq
        // here. Messages ahead of the hole are still delivered, but the
        // contiguous "next expected" front only advances once the
        // missing range is actually recovered (see deliverSequenced).

        const uint8_t* p = data + MOLD_HEADER_BYTES;
        const uint8_t* end = data + len;
        for (uint16_t i = 0; i < hdr.messageCount; ++i) {
            if (end - p < 2) { ++framingErrors_; return false; }
            uint16_t mlen = readU16BE(p);
            p += 2;
            if (static_cast<size_t>(end - p) < mlen) {
                ++framingErrors_;
                return false;
            }
            deliverSequenced(firstSeq + i, p, mlen);
            p += mlen;
        }
        return true;
    }

    uint64_t nextExpectedSequence() const { return nextExpectedSeq_.load(std::memory_order_relaxed); }
    uint64_t messagesDelivered()    const { return messagesDelivered_.load(std::memory_order_relaxed); }
    uint64_t gapsObserved()         const { return gapsObserved_.load(std::memory_order_relaxed); }
    uint64_t heartbeatsReceived()   const { return heartbeatsReceived_.load(std::memory_order_relaxed); }
    uint64_t framingErrors()        const { return framingErrors_.load(std::memory_order_relaxed); }
    uint64_t sessionMismatches()    const { return sessionMismatches_.load(std::memory_order_relaxed); }

private:
    // Deliver one sequenced message, maintaining the contiguous
    // "next expected" front. Both live and recovered / retransmitted
    // messages flow through here: a message that fills the current hole
    // advances nextExpectedSeq_ (merging any higher sequences buffered
    // early while the gap was outstanding), so once a gap is recovered
    // it is never re-detected. Messages already delivered (below the
    // front, or already buffered ahead) are dropped as duplicates.
    void deliverSequenced(uint64_t seq, const uint8_t* msg, size_t mlen) {
        uint64_t front = nextExpectedSeq_.load(std::memory_order_relaxed);
        if (seq < front || receivedAhead_.count(seq)) return;  // duplicate
        if (onMessage_) onMessage_(seq, msg, mlen);
        messagesDelivered_.fetch_add(1, std::memory_order_relaxed);
        if (seq == front) {
            uint64_t next = seq + 1;
            // Merge the contiguous run buffered ahead of the hole.
            while (!receivedAhead_.empty() &&
                   *receivedAhead_.begin() == next) {
                receivedAhead_.erase(receivedAhead_.begin());
                ++next;
            }
            nextExpectedSeq_.store(next, std::memory_order_relaxed);
        } else {
            receivedAhead_.insert(seq);  // ahead of the hole — buffer
        }
    }

    std::string     expectedSession_;
    OnMessage       onMessage_;
    OnGapDetected   onGap_;
    OnEndOfSession  onEos_;
    std::atomic<uint64_t> nextExpectedSeq_{1};
    std::set<uint64_t>    receivedAhead_;     // delivered seqs above the front
    uint64_t              gapReportedTo_{0};  // high end of last reported gap
    std::atomic<uint64_t> messagesDelivered_{0};
    std::atomic<uint64_t> gapsObserved_{0};
    std::atomic<uint64_t> heartbeatsReceived_{0};
    std::atomic<uint64_t> framingErrors_{0};
    std::atomic<uint64_t> sessionMismatches_{0};
};

}  // namespace OrderMatcher
