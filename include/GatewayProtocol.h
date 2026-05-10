#pragma once

// GatewayProtocol — versioned binary wire format for the order entry gateway.
//
// Design goals:
//   - Zero-copy deserialize: the receiver reads a fixed-size header, then
//     reads exactly payloadSize bytes into a buffer. The header tells it
//     which version produced the payload so it can interpret it correctly.
//   - Forward compatibility: a V1 receiver encountering a V2 payload it
//     doesn't understand can skip it (payloadSize tells it how many bytes
//     to discard). This is sufficient for rolling upgrades where the new
//     gateway is deployed before the new engine.
//   - Backward compatibility: a V2 receiver encountering a V1 payload
//     fills in defaults for fields added in V2. The deserializer knows
//     the on-disk size of V1 and memcpy's the prefix.
//   - No Protobuf / FlatBuffers / Cap'n Proto dependency. The matching
//     engine is header-only C++; pulling in a codegen tool for a 200-byte
//     struct is overkill.
//
// Wire layout:
//
//   ┌─────────────────────────────────────────────────────────────┐
//   │  GatewayRequestHeader  (16 bytes, fixed across versions)   │
//   ├─────────────────────────────────────────────────────────────┤
//   │  Payload  (headerSize-declared + payloadSize bytes)        │
//   └─────────────────────────────────────────────────────────────┘
//
// The header is NEVER reordered or resized; new fields go into the payload.
// Payload layout is version-specific — see OrderRequestV1/V2 below.
//
// Versioning strategy:
//   - GATEWAY_PROTOCOL_VERSION increments when the payload gains/drops/
//     reorders fields.
//   - MIN_READABLE_VERSION is the oldest version the current codebase can
//     still deserialize. Once no live node runs a version older than N,
//     bump MIN_READABLE_VERSION to N.

#include "Types.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace OrderMatcher {

// ─── Header ──────────────────────────────────────────────────────────────────

#pragma pack(push, 1)

struct GatewayRequestHeader {
    static constexpr uint32_t MAGIC = 0x4F524451;  // "ORDQ"

    uint32_t magic;          // Must equal MAGIC
    uint16_t version;        // Protocol version that produced this message
    uint16_t headerSize;     // sizeof(GatewayRequestHeader) — allows the
                              // header itself to grow without breaking readers
    uint32_t payloadSize;    // Byte count of the payload that follows
    uint32_t flags;          // Reserved (compression, encryption, etc.)
};

static_assert(sizeof(GatewayRequestHeader) == 16,
              "Header must be exactly 16 bytes across all platforms");

// ─── V1 payload (original OrderRequest layout) ──────────────────────────────

enum class GatewayRequestType : uint8_t {
    NewOrder      = 1,
    Cancel        = 2,
    Modify        = 3,
    CancelReplace = 4,
    KillSwitch    = 5,
    Shutdown      = 6,
    ExpireCheck   = 7
};

// V1: the original field set. Matches OrderRequest exactly.
struct OrderRequestV1 {
    GatewayRequestType type;
    SymbolId        symbolId;
    OrderId         orderId;
    ParticipantId   participantId;
    Side            side;
    Price           price;
    Quantity        qty;
    OrderType       orderType;
    Price           stopPrice;
    Quantity        displayQty;
    TimeInForce     tif;
    uint64_t        expiryTime;
    Price           stopLimitPrice;
    PegType         pegType;
    Price           pegOffset;
    Price           trailAmount;
    Quantity        minQty;
    bool            hidden;

    // modify/cancel-replace
    Price           newPrice;
    Quantity        newQty;

    uint64_t        ingressNs;
};

// ─── V2 payload (V1 + session routing) ──────────────────────────────────────
// V2 adds a session ID so the gateway can multiplex multiple FIX sessions
// over a single engine connection, and a client-provided correlation tag
// for end-to-end ack tracking.

struct OrderRequestV2 {
    // ── V1 prefix (identical layout, identical offsets) ──
    GatewayRequestType type;
    SymbolId        symbolId;
    OrderId         orderId;
    ParticipantId   participantId;
    Side            side;
    Price           price;
    Quantity        qty;
    OrderType       orderType;
    Price           stopPrice;
    Quantity        displayQty;
    TimeInForce     tif;
    uint64_t        expiryTime;
    Price           stopLimitPrice;
    PegType         pegType;
    Price           pegOffset;
    Price           trailAmount;
    Quantity        minQty;
    bool            hidden;
    Price           newPrice;
    Quantity        newQty;
    uint64_t        ingressNs;

    // ── V2 additions ──
    uint64_t        sessionId;       // FIX session that originated this message
    uint64_t        correlationTag;  // opaque cookie echoed back on the ack
};

#pragma pack(pop)

// ─── Version constants ───────────────────────────────────────────────────────

static constexpr uint16_t GATEWAY_PROTOCOL_VERSION     = 2;
static constexpr uint16_t GATEWAY_MIN_READABLE_VERSION = 1;

// ─── Serialization ──────────────────────────────────────────────────────────

// Build a wire-ready frame from a V2 payload. Returns a byte vector
// containing [Header | Payload].
inline std::vector<uint8_t> serializeGatewayRequest(const OrderRequestV2& req) {
    std::vector<uint8_t> frame(sizeof(GatewayRequestHeader) + sizeof(OrderRequestV2));

    GatewayRequestHeader hdr{};
    hdr.magic       = GatewayRequestHeader::MAGIC;
    hdr.version     = GATEWAY_PROTOCOL_VERSION;
    hdr.headerSize  = sizeof(GatewayRequestHeader);
    hdr.payloadSize = sizeof(OrderRequestV2);
    hdr.flags       = 0;

    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + sizeof(hdr), &req, sizeof(req));
    return frame;
}

// ─── Deserialization ─────────────────────────────────────────────────────────

enum class GatewayDecodeResult : uint8_t {
    Ok,
    BadMagic,
    UnsupportedVersion,      // version < MIN_READABLE_VERSION
    UnknownFutureVersion,    // version > GATEWAY_PROTOCOL_VERSION (skip)
    Truncated,               // not enough bytes for header + payload
};

// Decode a wire frame into an OrderRequestV2, filling in defaults for
// fields that weren't present in older versions.
//
// On UnknownFutureVersion the caller should skip `hdr.headerSize +
// hdr.payloadSize` bytes and try the next frame — that's how a V1
// receiver survives a V2 sender during a rolling upgrade.
inline GatewayDecodeResult deserializeGatewayRequest(
        const uint8_t* data, size_t len, OrderRequestV2& out) {
    if (len < sizeof(GatewayRequestHeader)) {
        return GatewayDecodeResult::Truncated;
    }

    GatewayRequestHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));

    if (hdr.magic != GatewayRequestHeader::MAGIC) {
        return GatewayDecodeResult::BadMagic;
    }
    if (hdr.version < GATEWAY_MIN_READABLE_VERSION) {
        return GatewayDecodeResult::UnsupportedVersion;
    }
    if (hdr.version > GATEWAY_PROTOCOL_VERSION) {
        return GatewayDecodeResult::UnknownFutureVersion;
    }

    // header may have grown; skip exactly headerSize bytes.
    size_t payloadOffset = hdr.headerSize;
    if (len < payloadOffset + hdr.payloadSize) {
        return GatewayDecodeResult::Truncated;
    }

    // Zero-fill out so any fields the sender's version didn't include
    // get deterministic defaults.
    std::memset(&out, 0, sizeof(out));

    const uint8_t* payload = data + payloadOffset;

    if (hdr.version == 1) {
        // V1 payload is a strict prefix of V2. Copy exactly
        // sizeof(OrderRequestV1) bytes; V2-only fields stay zeroed.
        size_t toCopy = std::min<size_t>(hdr.payloadSize, sizeof(OrderRequestV1));
        std::memcpy(&out, payload, toCopy);
        // V2 additions default:
        out.sessionId      = 0;
        out.correlationTag = 0;
    } else {
        // V2 — full copy.
        size_t toCopy = std::min<size_t>(hdr.payloadSize, sizeof(OrderRequestV2));
        std::memcpy(&out, payload, toCopy);
    }

    return GatewayDecodeResult::Ok;
}

// ─── Gateway Response ────────────────────────────────────────────────────────
// The ack frame sent back from the engine to the gateway. Same versioning
// scheme: fixed header + versioned payload.

struct GatewayResponse {
    uint64_t correlationTag;   // echoed from the request
    uint64_t sequenceId;       // engine sequence number (0 on reject)
    uint8_t  status;           // 0 = accepted, 1 = rejected
    uint8_t  rejectReason;     // RejectReason cast to uint8_t
    uint8_t  reserved[6];      // pad to 24 bytes
};

static_assert(sizeof(GatewayResponse) == 24,
              "GatewayResponse must be a fixed 24 bytes");

}  // namespace OrderMatcher
