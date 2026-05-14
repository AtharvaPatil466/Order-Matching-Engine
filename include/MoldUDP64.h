#pragma once

// MoldUDP64 — Nasdaq's UDP multicast wrapper for ITCH and similar
// market-data feeds.
//
// One UDP datagram = one MoldUDP64 packet. Each packet carries an
// ordered sequence of application messages (typically ITCH frames),
// with a header that tells subscribers:
//   * which session they're in (so a subscriber can detect a venue
//     restart and re-subscribe),
//   * the sequence number of the FIRST message in this packet
//     (subscribers maintain a per-session next-expected counter;
//     any gap triggers re-request from the TCP retransmission
//     service),
//   * how many application messages are concatenated in this packet
//     (batching for throughput; the venue typically flushes every
//     few hundred microseconds or when a batch reaches a target
//     size).
//
// Wire format (network byte order throughout):
//   Session            [10 bytes ASCII, space-padded]
//   SequenceNumber     [8 bytes, uint64, BE]
//   MessageCount       [2 bytes, uint16, BE]
//   then N messages, each:
//     MessageLength    [2 bytes, uint16, BE]
//     MessageData      [MessageLength bytes]
//
// Special MessageCount values:
//   0      : heartbeat (no messages) — emitted every ~second by the
//            venue when no real messages are queued so subscribers
//            can distinguish "quiet market" from "network partition"
//   0xFFFF : end-of-session indicator — terminal; subscribers
//            disconnect and wait for the next trading day's session
//
// Gap recovery: subscribers don't ask the multicast publisher for
// retransmission (it's UDP — gone is gone). Instead they connect
// to a separate TCP-based "Request Server" (a SoupBinTCP-style
// service) and request the missing sequence range. This codec
// produces the multicast wire frames; the recovery channel is the
// caller's wiring.

#include "OuchProtocol.h"  // readU16BE, readU32BE, writeU16BE, writeU64BE, writeFixedAscii

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
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
    void setNextSequence(uint64_t seq) { nextSeq_ = seq; }
    uint64_t nextSequence() const { return nextSeq_; }

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
    }

    uint64_t packetsEmitted()       const { return packetsEmitted_; }
    uint64_t heartbeatsEmitted()    const { return heartbeatsEmitted_; }
    uint64_t endOfSessionsEmitted() const { return endOfSessionsEmitted_; }
    size_t   pendingCount()         const { return pending_.size(); }
    const std::string& session()    const { return session_; }

private:
    struct PendingMessage { uint16_t offset; uint16_t length; };

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
    size_t                       mtu_;
    uint64_t                     nextSeq_;
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
        if (firstSeq > expected) {
            if (onGap_) onGap_(expected, firstSeq);
            gapsObserved_.fetch_add(1, std::memory_order_relaxed);
            nextExpectedSeq_.store(firstSeq, std::memory_order_relaxed);  // resume from the new data
        } else if (firstSeq < expected) {
            // Already-seen messages — duplicate / retransmission.
            // Skip the prefix that we've already processed; deliver
            // only the suffix (if any) that's still new. For an
            // entirely-old packet, message_count steps through and
            // delivers nothing.
        }

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
            uint64_t thisSeq = firstSeq + i;
            if (thisSeq >= nextExpectedSeq_.load(std::memory_order_relaxed)) {
                if (onMessage_) onMessage_(thisSeq, p, mlen);
                nextExpectedSeq_.store(thisSeq + 1, std::memory_order_relaxed);
                messagesDelivered_.fetch_add(1, std::memory_order_relaxed);
            }
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
    std::string     expectedSession_;
    OnMessage       onMessage_;
    OnGapDetected   onGap_;
    OnEndOfSession  onEos_;
    std::atomic<uint64_t> nextExpectedSeq_{1};
    std::atomic<uint64_t> messagesDelivered_{0};
    std::atomic<uint64_t> gapsObserved_{0};
    std::atomic<uint64_t> heartbeatsReceived_{0};
    std::atomic<uint64_t> framingErrors_{0};
    std::atomic<uint64_t> sessionMismatches_{0};
};

}  // namespace OrderMatcher
