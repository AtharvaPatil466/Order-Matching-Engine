#pragma once

// SoupBinTcpFramer — reassembles complete SoupBinTCP packets from an
// arbitrarily-fragmented byte stream and hands each off via a
// callback. Same shape as FixFramer / OuchSession's accumulation
// buffer; the protocol-specific bits are the 2-byte big-endian
// length prefix and the bound on packet size.
//
// Usage:
//   SoupBinTcpFramer framer;
//   framer.setOnPacket([&](const SoupPacket& pkt){ ... });
//   framer.feed(bytes, len);   // call once per recv chunk
//
// Returns false on unrecoverable framing error (oversize claim or
// internal buffer overflow); the caller should drop the connection
// in that case.

#include "SoupBinTCP.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace OrderMatcher {

class SoupBinTcpFramer {
public:
    using OnPacket = std::function<void(const SoupPacket&)>;

    explicit SoupBinTcpFramer(size_t maxAccumBytes = 256 * 1024)
        : maxAccumBytes_(maxAccumBytes) {
        buf_.reserve(512);
    }

    void setOnPacket(OnPacket cb) { onPacket_ = std::move(cb); }

    bool feed(const char* data, size_t len) {
        if (len == 0) return true;
        if (buf_.size() + len > maxAccumBytes_) return false;
        buf_.insert(buf_.end(),
                    reinterpret_cast<const uint8_t*>(data),
                    reinterpret_cast<const uint8_t*>(data) + len);
        return drain();
    }

    uint64_t packetsEmitted()  const { return packetsEmitted_; }
    uint64_t bytesConsumed()   const { return bytesConsumed_; }

private:
    bool drain() {
        size_t pos = 0;
        while (true) {
            // Need at least 3 bytes (length + type) before we can
            // know the packet's total size.
            if (buf_.size() - pos < 3) break;

            uint16_t lenField = readU16BE(buf_.data() + pos);
            // SoupBinTCP length is type-byte + payload, so it must
            // be at least 1. A length of 0 is malformed.
            if (lenField == 0) return false;
            if (lenField - 1u > SOUP_MAX_PACKET_PAYLOAD) return false;

            size_t totalBytes = static_cast<size_t>(lenField) + 2;
            if (buf_.size() - pos < totalBytes) break;  // partial packet

            SoupPacket pkt;
            pkt.type       = buf_[pos + 2];
            pkt.payload    = (lenField > 1) ? &buf_[pos + 3] : nullptr;
            pkt.payloadLen = static_cast<size_t>(lenField) - 1;
            if (onPacket_) onPacket_(pkt);
            ++packetsEmitted_;
            pos += totalBytes;
        }
        if (pos > 0) {
            bytesConsumed_ += pos;
            buf_.erase(buf_.begin(), buf_.begin() + pos);
        }
        return true;
    }

    OnPacket             onPacket_;
    std::vector<uint8_t> buf_;
    size_t               maxAccumBytes_;
    uint64_t             packetsEmitted_{0};
    uint64_t             bytesConsumed_{0};
};

}  // namespace OrderMatcher
