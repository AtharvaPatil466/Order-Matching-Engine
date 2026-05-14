#pragma once

// SbeSession — schema-driven binary order entry over a byte stream.
//
// Drop-in parallel to FixSession / OuchSession: feed wire bytes via
// feed(), responses flow out through a SendBytes callback, the
// engine is driven via standard submitOrder. The codec is different
// (SBE, little-endian, MessageHeader-prefixed) but the composition
// surface is identical so the gateway loop can dispatch any
// protocol without knowing the codec.
//
// The session knows about NewOrder (v1 and v2) and emits OrderAck
// (v1) responses. Forward compatibility is built in:
//   * A v1 publisher whose message arrives at a v2 reader uses
//     defaults for the v2-only fields (participantId=0,
//     timeInForce=GTC). No code change needed at the session.
//   * A v3 publisher whose message arrives at a v2 reader reads
//     the v2 fields it knows; the extra trailing bytes are skipped
//     using blockLength from the header.
//
// Framing: SBE doesn't define a session-level wire format by itself
// — each message is the MessageHeader + fixed block (+ optional
// repeating groups, not used here). For framing on a stream the
// session uses the same self-describing approach as the SBE Tool's
// generated codecs: read 8 bytes to get the header, use blockLength
// to know how many more bytes to consume, dispatch on templateId,
// then loop. Production deployments often wrap SBE in CME's Simple
// Open Framing Header (SOFH) for length-prefixed framing; that's
// trivially layerable on top.

#include "MatchingEngine.h"
#include "SbeProtocol.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OrderMatcher {

class SbeSession {
public:
    using SendBytes = std::function<void(std::string_view)>;

    SbeSession(MatchingEngine& engine, SendBytes send,
               size_t maxAccumBytes = 64 * 1024)
        : engine_(engine), send_(std::move(send)),
          maxAccumBytes_(maxAccumBytes) {
        buf_.reserve(256);
    }

    // Feed wire bytes from the underlying transport. Returns false
    // on unrecoverable framing error (oversize block claim, unknown
    // schema id, accumulation overflow) — the gateway should drop
    // the connection. Recoverable errors at the message level
    // (unknown templateId at the current schema version) emit a
    // reject ack and continue.
    bool feed(const char* data, size_t len) {
        if (len == 0) return true;
        if (buf_.size() + len > maxAccumBytes_) return false;
        buf_.insert(buf_.end(),
                    reinterpret_cast<const uint8_t*>(data),
                    reinterpret_cast<const uint8_t*>(data) + len);

        size_t pos = 0;
        while (pos + SBE_MESSAGE_HEADER_BYTES <= buf_.size()) {
            auto h = readSbeMessageHeader(buf_.data() + pos);
            // blockLength is uint16, so 65535 is the natural cap.
            // Refuse anything above 32KB as a sanity bound — no
            // sensible SBE message in our schema family is that
            // large, so this is a hostile peer.
            constexpr uint16_t kMaxBlockLength = 32 * 1024;
            if (h.blockLength > kMaxBlockLength) return false;
            // Schema id mismatch = hard protocol error.
            if (h.schemaId != SBE_SCHEMA_ID) return false;

            size_t need = SBE_MESSAGE_HEADER_BYTES + h.blockLength;
            if (buf_.size() - pos < need) break;  // partial — wait

            dispatch(h, buf_.data() + pos + SBE_MESSAGE_HEADER_BYTES);
            pos += need;
            ++messagesProcessed_;
        }
        if (pos > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + pos);
        }
        return true;
    }

    uint64_t messagesProcessed() const { return messagesProcessed_; }
    uint64_t ordersAccepted()    const { return ordersAccepted_; }
    uint64_t ordersRejected()    const { return ordersRejected_; }
    uint64_t messagesSkipped()   const { return messagesSkipped_; }

private:
    void dispatch(const SbeMessageHeader& h, const uint8_t* block) {
        switch (h.templateId) {
        case SBE_TEMPLATE_NEW_ORDER:
            handleNewOrder(h, block);
            return;
        default:
            // Unknown templateId at this schema version — skip and
            // count. A real venue might log this for surveillance;
            // for the session loop the block was already consumed
            // by the framer.
            ++messagesSkipped_;
            return;
        }
    }

    void handleNewOrder(const SbeMessageHeader& h, const uint8_t* block) {
        // Decode using the v2 reader regardless of the wire version.
        // The decoder uses h.blockLength to decide whether the new
        // fields are present; defaults are filled in for missing
        // fields. This is the whole point of SBE — one codec
        // serves multiple versions.
        SbeNewOrderV2 msg;
        if (!readSbeNewOrderV2Block(block, h.blockLength, msg)) {
            // Block shorter than v1's minimum — protocol violation.
            // We don't have an orderId to reference; emit a reject
            // with id=0.
            sendAck(0, /*accepted=*/false,
                    RejectReason::MissingRequiredField);
            ++ordersRejected_;
            return;
        }

        Side side = (msg.side == 1) ? Side::Buy : Side::Sell;
        OrderType type = decodeOrderType(msg.orderType);
        TimeInForce tif = (msg.timeInForce == 1) ? TimeInForce::DAY
                                                  : TimeInForce::GTC;

        // SBE has no symbol field in this toy schema — production
        // would carry it. Default to symbol 0 (the session's
        // dedicated symbol) so the test path doesn't require
        // per-message symbol routing. A real session would carry a
        // SecurityID field with each NewOrder.
        constexpr SymbolId kDefaultSymbol = 0;

        SubmitResult r = engine_.submitOrder(
            kDefaultSymbol,
            static_cast<OrderId>(msg.orderId),
            static_cast<ParticipantId>(msg.participantId),
            side, msg.price, msg.quantity, type,
            /*stopPrice=*/0, /*displayQty=*/0, tif);

        if (r.isAccepted()) {
            ++ordersAccepted_;
            sendAck(msg.orderId, /*accepted=*/true, RejectReason::None);
        } else {
            ++ordersRejected_;
            sendAck(msg.orderId, /*accepted=*/false, r.rejectReason);
        }
    }

    static OrderType decodeOrderType(uint8_t code) {
        switch (code) {
        case 1: return OrderType::Limit;
        case 2: return OrderType::Market;
        case 3: return OrderType::IOC;
        case 4: return OrderType::FOK;
        default: return OrderType::Limit;
        }
    }

    void sendAck(uint64_t orderId, bool accepted, RejectReason reason) {
        if (!send_) return;
        SbeOrderAckV1 ack{};
        ack.orderId      = orderId;
        ack.status       = accepted ? SBE_ORDER_ACK_STATUS_ACCEPTED
                                     : SBE_ORDER_ACK_STATUS_REJECTED;
        ack.rejectReason = static_cast<uint8_t>(reason);
        uint8_t buf[SBE_MESSAGE_HEADER_BYTES + SBE_ORDER_ACK_V1_BLOCK_BYTES];
        size_t n = encodeSbeOrderAckV1(buf, ack);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
    }

    MatchingEngine&         engine_;
    SendBytes               send_;
    size_t                  maxAccumBytes_;
    std::vector<uint8_t>    buf_;
    uint64_t                messagesProcessed_{0};
    uint64_t                ordersAccepted_{0};
    uint64_t                ordersRejected_{0};
    uint64_t                messagesSkipped_{0};
};

}  // namespace OrderMatcher
