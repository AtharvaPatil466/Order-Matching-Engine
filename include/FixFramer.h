#pragma once

// FixFramer — portable, header-only FIX message framer for stream sockets.
//
// FIX 4.2 frames are self-describing: every message begins with
//   8=FIX.4.2<SOH>9=<bodyLen><SOH>...
// where <bodyLen> is the byte count from immediately after the BodyLength
// field's SOH up to (not including) the CheckSum field. The trailer is then
// exactly "10=NNN<SOH>" (7 bytes). This is enough to identify boundaries
// without parsing the body.
//
// The framer maintains a bounded rolling buffer; bytes from recv() are fed
// in chunks of arbitrary size and complete messages are emitted via a
// callback. Every emitted message is structurally well-formed (BeginString,
// BodyLength, MsgType, CheckSum present) and has a valid checksum — so the
// caller can pass it straight into application logic without further
// validation.
//
// Errors:
//   feed() returns false on unrecoverable framing errors (oversize frame,
//   absurd body length, garbage between messages with no recoverable
//   resync). The caller should drop the connection. Recoverable issues
//   (incomplete frame, partial trailer) leave the buffer in a valid state
//   and feed() returns true.

#include "FIXParser.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string_view>
#include <vector>

namespace OrderMatcher {

class FixFramer {
public:
    using OnMessage = std::function<void(std::string_view rawFrame,
                                         const FixMessage& parsed)>;

    explicit FixFramer(size_t maxFrameSize = 64 * 1024)
        : maxFrameSize_(maxFrameSize) {
        buf_.reserve(maxFrameSize_);
    }

    void setOnMessage(OnMessage cb) { onMessage_ = std::move(cb); }

    // Counts of frames the framer rejected. Useful as a metric.
    uint64_t framesEmitted()   const { return framesEmitted_; }
    uint64_t framesRejected()  const { return framesRejected_; }

    void reset() {
        buf_.clear();
        framesEmitted_ = 0;
        framesRejected_ = 0;
    }

    // Feed `len` bytes. Emits each complete framed message via the callback.
    // Returns false on unrecoverable framing error.
    bool feed(const char* data, size_t len) {
        if (len == 0) return true;

        // Bound the buffer. If the new chunk would push us past max and we
        // still don't have a complete frame, the peer is sending something
        // larger than we accept — drop.
        if (buf_.size() + len > maxFrameSize_ * 2) {
            // Allow up to 2x maxFrame so a single frame plus a partial next
            // can coexist without spurious rejection. Anything past that is
            // adversarial.
            return false;
        }
        buf_.insert(buf_.end(), data, data + len);

        while (true) {
            // Find a frame start: "8=FIX.4." (anchor on the digit-agnostic
            // prefix so 4.2 / 4.4 etc. all match). If absent, we may still
            // have an incomplete prefix at the buffer tail — keep waiting.
            constexpr std::string_view kPrefix{"8=FIX.4."};
            auto* base = buf_.data();
            size_t avail = buf_.size();

            const char* start = nullptr;
            for (size_t i = 0; i + kPrefix.size() <= avail; ++i) {
                if (std::memcmp(base + i, kPrefix.data(), kPrefix.size()) == 0) {
                    start = base + i;
                    // Discard any garbage before the start (resync). Keeping
                    // it would let one malformed prefix block the stream.
                    if (i > 0) {
                        buf_.erase(buf_.begin(), buf_.begin() + i);
                        base = buf_.data();
                        avail = buf_.size();
                        start = base;
                    }
                    break;
                }
            }
            if (!start) {
                // No prefix found. Keep only the last (kPrefix.size()-1)
                // bytes — the prefix may straddle this and the next chunk.
                if (avail >= kPrefix.size()) {
                    buf_.erase(buf_.begin(),
                               buf_.begin() + (avail - (kPrefix.size() - 1)));
                }
                return true;
            }

            // Need enough bytes to read past BeginString and BodyLength.
            // Find the SOH that ends BeginString.
            const char* sohAfterBegin = static_cast<const char*>(
                std::memchr(start, FIX_SOH, avail));
            if (!sohAfterBegin) return true;  // wait for more

            // BodyLength must follow as "9=NNN<SOH>".
            const char* p = sohAfterBegin + 1;
            const char* end = base + avail;
            if (end - p < 3) return true;
            if (p[0] != '9' || p[1] != '=') {
                // Malformed — discard the prefix and resync.
                buf_.erase(buf_.begin(), buf_.begin() + 1);
                ++framesRejected_;
                continue;
            }
            p += 2;
            // Parse body length digits.
            uint32_t bodyLen = 0;
            const char* digitStart = p;
            while (p < end && *p >= '0' && *p <= '9') {
                bodyLen = bodyLen * 10 + uint32_t(*p - '0');
                if (bodyLen > maxFrameSize_) {
                    // Refuse oversized frames — defends against absurd
                    // BodyLength values that would otherwise cause us to
                    // wait forever for bytes that aren't coming.
                    return false;
                }
                ++p;
            }
            if (p == digitStart) {
                // No digits — malformed.
                buf_.erase(buf_.begin(), buf_.begin() + 1);
                ++framesRejected_;
                continue;
            }
            if (p >= end) return true;          // need the SOH terminator
            if (*p != FIX_SOH) {
                // Non-digit, non-SOH after "9=" — malformed; resync.
                buf_.erase(buf_.begin(), buf_.begin() + 1);
                ++framesRejected_;
                continue;
            }
            const char* bodyStart = p + 1;
            // Trailer is exactly "10=NNN<SOH>" — 7 bytes after body.
            constexpr size_t kTrailerLen = 7;
            const char* frameEnd = bodyStart + bodyLen + kTrailerLen;
            if (frameEnd > end) return true;     // need more bytes

            size_t frameLen = size_t(frameEnd - start);
            FixMessage msg;
            bool ok = msg.parse(start, frameLen);
            ok = ok && msg.isWellFormed();
            ok = ok && msg.validateChecksum();

            if (ok) {
                if (onMessage_) {
                    onMessage_(std::string_view(start, frameLen), msg);
                }
                ++framesEmitted_;
            } else {
                ++framesRejected_;
            }

            // Advance past this (good or bad) frame.
            buf_.erase(buf_.begin(), buf_.begin() + frameLen);
        }
    }

private:
    std::vector<char> buf_;
    size_t            maxFrameSize_;
    OnMessage         onMessage_;
    uint64_t          framesEmitted_{0};
    uint64_t          framesRejected_{0};
};

}  // namespace OrderMatcher
