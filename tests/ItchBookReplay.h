#pragma once
//
// ItchBookReplay — subscriber-side book reconstruction from an ITCH 5.0 byte
// stream, shared by the feed-fidelity tests.
//
// Deliberately dumb and independent of the engine: a flat id -> order map fed
// only by wire bytes. If it needed engine internals to stay in sync, it would
// not be testing what a real subscriber can actually do.
//
// Used by:
//   ItchReconstructionTest        — replays the live publisher stream
//   GapRecoveryReconstructionTest — replays a stream with induced packet loss

#include "ItchProtocol.h"
#include "OuchProtocol.h"  // readU16BE / readU32BE / readU64BE
#include "Types.h"

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace OrderMatcher {
namespace testing {

// Wire length of one ITCH message, by type. Zero means unrecognised, which is
// unrecoverable for a stream parser — there is no way to find the next
// boundary.
inline size_t itchMessageSize(uint8_t messageType) {
    switch (messageType) {
    case ITCH_MT_SYSTEM_EVENT:      return ITCH_SIZE_SYSTEM_EVENT;
    case ITCH_MT_STOCK_DIRECTORY:   return ITCH_SIZE_STOCK_DIRECTORY;
    case ITCH_MT_TRADING_ACTION:    return ITCH_SIZE_TRADING_ACTION;
    case ITCH_MT_ADD_ORDER:         return ITCH_SIZE_ADD_ORDER;
    case ITCH_MT_ORDER_EXECUTED:    return ITCH_SIZE_ORDER_EXECUTED;
    case ITCH_MT_ORDER_EXECUTED_PX: return ITCH_SIZE_ORDER_EXECUTED_PX;
    case ITCH_MT_ORDER_CANCEL:      return ITCH_SIZE_ORDER_CANCEL;
    case ITCH_MT_ORDER_DELETE:      return ITCH_SIZE_ORDER_DELETE;
    case ITCH_MT_TRADE:             return ITCH_SIZE_TRADE;
    case ITCH_MT_CROSS_TRADE:       return ITCH_SIZE_CROSS_TRADE;
    default:                        return 0;
    }
}

class ItchBookReplay {
public:
    struct Entry {
        Side     side;
        Price    price;
        Quantity shares;
    };

    // Apply one complete ITCH message. Callers holding an already-framed
    // message (a MoldUDP64 payload, say) use this directly.
    void applyMessage(const uint8_t* p) {
        const uint8_t type = p[0];
        // Every order-scoped message carries its reference at offset 11.
        const auto ref = static_cast<OrderId>(readU64BE(p + 11));

        switch (type) {
        case ITCH_MT_ADD_ORDER: {
            Entry e{};
            e.side   = (p[19] == 'B') ? Side::Buy : Side::Sell;
            e.shares = static_cast<Quantity>(readU32BE(p + 20));
            e.price  = static_cast<Price>(readU32BE(p + 32));
            orders_[ref] = e;
            break;
        }
        case ITCH_MT_ORDER_EXECUTED:
            reduce(ref, static_cast<Quantity>(readU32BE(p + 19)));
            break;
        case ITCH_MT_ORDER_CANCEL:
            reduce(ref, static_cast<Quantity>(readU32BE(p + 19)));
            break;
        case ITCH_MT_ORDER_DELETE:
            orders_.erase(ref);
            break;
        default:
            // S / R / H / P / Q carry no displayed-book state.
            break;
        }
    }

    // Sink for a raw byte stream. Buffers because a transport is free to
    // coalesce frames; the parser must not assume one message per callback.
    void feed(std::string_view bytes) {
        buffer_.append(bytes);
        size_t offset = 0;
        while (offset < buffer_.size()) {
            const auto* p = reinterpret_cast<const uint8_t*>(buffer_.data()) + offset;
            const size_t size = itchMessageSize(p[0]);
            if (size == 0) throw std::runtime_error("unknown ITCH message type");
            if (offset + size > buffer_.size()) break;  // partial frame
            applyMessage(p);
            offset += size;
        }
        buffer_.erase(0, offset);
    }

    // Aggregate into price levels, matching what getSnapshot() reports.
    // Zero-share entries are dropped: an order whose shares reached zero via
    // 'E' is awaiting its 'D' and is no longer displayed, so it must not
    // contribute a phantom level.
    std::map<Price, Quantity> depth(Side side) const {
        std::map<Price, Quantity> levels;
        for (const auto& entry : orders_) {
            const Entry& o = entry.second;
            if (o.side != side || o.shares == 0) continue;
            levels[o.price] += o.shares;
        }
        return levels;
    }

    bool   knows(OrderId id) const { return orders_.count(id) != 0; }
    size_t liveCount()       const { return orders_.size(); }

    // Per-order detail at one level, for diagnosing a depth mismatch.
    std::string describeLevel(Side side, Price price) const {
        std::string s;
        for (const auto& entry : orders_) {
            const Entry& o = entry.second;
            if (o.side != side || o.price != price) continue;
            s += " #" + std::to_string(entry.first) + "x" + std::to_string(o.shares);
        }
        return s.empty() ? std::string(" (none)") : s;
    }

private:
    // A subscriber handed more shares than it is tracking has lost sync — the
    // bug these tests exist to catch — so it throws rather than saturating and
    // hiding the drift. Likewise a message for an order never added: that is
    // what applying a stream out of order looks like from the inside.
    void reduce(OrderId ref, Quantity qty) {
        auto it = orders_.find(ref);
        if (it == orders_.end())
            throw std::runtime_error("message for an order never added to the feed");
        if (qty > it->second.shares)
            throw std::runtime_error("reduction exceeds displayed shares (feed drift)");
        it->second.shares -= qty;
    }

    std::string                        buffer_;
    std::unordered_map<OrderId, Entry> orders_;
};

}  // namespace testing
}  // namespace OrderMatcher
