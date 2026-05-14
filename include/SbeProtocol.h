#pragma once

// SbeProtocol — Simple Binary Encoding (SBE), the FIX Trading
// Community's schema-driven binary format. Used by CME (MDP 3.0,
// iLink 3), ICE (FIX/FAST → SBE), and several FPGA-optimized
// venues. Distinct paradigm from OUCH/ITCH:
//
//   OUCH/ITCH:  one message type = one fixed wire layout, period.
//               Versioning means "publish a whole new spec, rebuild
//               everyone."
//
//   SBE:        every message carries an 8-byte MessageHeader that
//               describes its schema + version + actual fixed-block
//               length. New schema versions append new fields at
//               the end of the fixed block, then bump the version.
//               Older readers know the fixed block is now longer
//               (from the header's blockLength) and skip past the
//               unknown trailing bytes — they keep reading the
//               fields they DO understand without code changes.
//
//               That's forward-compatibility-by-construction, the
//               feature that makes SBE worth the codegen overhead
//               in real deployments.
//
// Endianness: little-endian. SBE defaults to little-endian to match
// x86/ARM native byte order, removing the BE↔host byte-swap that
// OUCH/ITCH/FIX-binary all incur. (Some venues override and use
// BE; the schema declares it.)
//
// This file hand-codes one message type (NewOrder) in two
// versions, demonstrating the wire contract. A production SBE
// deployment would use the FIX-TG SBE Tool (or rebar3/sbe-tool)
// to generate codec stubs from an XML schema. The shape of those
// stubs would be the same as what's here.

#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace OrderMatcher {

// ─── Little-endian byte order helpers ──────────────────────────────────────
// Pure-byte-access implementations — safe on any alignment.
// Most modern x86/ARM hosts ARE little-endian, but writing these
// out explicitly keeps the codec portable to BE hosts and makes
// the wire contract self-documenting.

inline void writeU16LE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline void writeU32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline void writeU64LE(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

inline void writeI64LE(uint8_t* p, int64_t v) {
    writeU64LE(p, static_cast<uint64_t>(v));
}

inline uint16_t readU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t readU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

inline int64_t readI64LE(const uint8_t* p) {
    return static_cast<int64_t>(readU64LE(p));
}

// ─── MessageHeader ──────────────────────────────────────────────────────────
// Every SBE message on the wire begins with this 8-byte header. It
// fully describes how to interpret the bytes that follow:
//
//   blockLength : size in bytes of the fixed-size block that
//                 immediately follows the header. Crucial for
//                 forward compatibility — a v2 reader sees a v3
//                 blockLength larger than v2's fixed-block size,
//                 reads the v2 fields it knows, then skips ahead
//                 by (blockLength - v2BlockSize) before parsing
//                 any repeating groups or variable-length data.
//
//   templateId  : which message type this is. (1 = NewOrder in our
//                 toy schema; production schemas typically allocate
//                 one ID per message type.)
//
//   schemaId    : which schema (typically per asset class or per
//                 venue product). Multiple schemas can coexist on
//                 the wire; the reader picks the codec by ID.
//
//   version     : schema version. A reader at version N can decode
//                 messages from schemas with version ≤ N (using its
//                 own field set) AND version > N (using blockLength
//                 to skip the unknown trailing fields). It CANNOT
//                 decode messages from a schema with a different
//                 schemaId — that's a hard ABI break.

constexpr size_t SBE_MESSAGE_HEADER_BYTES = 8;

struct SbeMessageHeader {
    uint16_t blockLength;
    uint16_t templateId;
    uint16_t schemaId;
    uint16_t version;
};

inline void writeSbeMessageHeader(uint8_t* out, const SbeMessageHeader& h) {
    writeU16LE(out + 0, h.blockLength);
    writeU16LE(out + 2, h.templateId);
    writeU16LE(out + 4, h.schemaId);
    writeU16LE(out + 6, h.version);
}

inline SbeMessageHeader readSbeMessageHeader(const uint8_t* in) {
    SbeMessageHeader h;
    h.blockLength = readU16LE(in + 0);
    h.templateId  = readU16LE(in + 2);
    h.schemaId    = readU16LE(in + 4);
    h.version     = readU16LE(in + 6);
    return h;
}

// ─── Schema: this codec's identifiers ───────────────────────────────────────

constexpr uint16_t SBE_SCHEMA_ID                = 100;
constexpr uint16_t SBE_TEMPLATE_NEW_ORDER       = 1;

// ─── NewOrder schema v1 ─────────────────────────────────────────────────────
// 24-byte fixed block. Offsets are cumulative from the start of
// the block (i.e. AFTER the 8-byte MessageHeader).
//
//   0..7   orderId       (uint64 LE)
//   8..15  price         (int64  LE) -- fixed-point: PRICE_PRECISION
//   16..19 quantity      (uint32 LE)
//   20     side          (uint8 : 1=Buy, 2=Sell)
//   21     orderType     (uint8 : 1=Limit, 2=Market, 3=IOC, 4=FOK)
//   22..23 padding       (reserved for alignment of v1)

constexpr uint16_t SBE_NEW_ORDER_V1            = 1;
constexpr uint16_t SBE_NEW_ORDER_V1_BLOCK_BYTES = 24;

struct SbeNewOrderV1 {
    uint64_t  orderId;
    Price     price;       // int64
    Quantity  quantity;    // uint32, but use Quantity to match engine types
    uint8_t   side;        // 1 = Buy, 2 = Sell
    uint8_t   orderType;   // 1 = Limit, 2 = Market, 3 = IOC, 4 = FOK
};

inline void writeSbeNewOrderV1Block(uint8_t* out, const SbeNewOrderV1& msg) {
    writeU64LE(out + 0,  msg.orderId);
    writeI64LE(out + 8,  static_cast<int64_t>(msg.price));
    writeU32LE(out + 16, static_cast<uint32_t>(msg.quantity));
    out[20] = msg.side;
    out[21] = msg.orderType;
    out[22] = 0;
    out[23] = 0;
}

inline void readSbeNewOrderV1Block(const uint8_t* in, SbeNewOrderV1& out) {
    out.orderId   = readU64LE(in + 0);
    out.price     = static_cast<Price>(readI64LE(in + 8));
    out.quantity  = static_cast<Quantity>(readU32LE(in + 16));
    out.side      = in[20];
    out.orderType = in[21];
}

// ─── NewOrder schema v2 ─────────────────────────────────────────────────────
// Adds participantId and a single timeInForce byte at the end of
// the block. 32-byte block (v1 was 24). The version bumps from 1
// to 2; the schemaId stays the same.
//
// Layout — first 24 bytes are EXACTLY identical to v1, so any v1
// reader looking at a v2 message reads the original fields
// unchanged, just doesn't see the new ones. That's the SBE
// forward-compat guarantee.
//
//   0..23  (v1 fields above, unchanged)
//   24..27 participantId   (uint32 LE)
//   28     timeInForce     (uint8 : 0=GTC, 1=DAY)
//   29..31 padding

constexpr uint16_t SBE_NEW_ORDER_V2            = 2;
constexpr uint16_t SBE_NEW_ORDER_V2_BLOCK_BYTES = 32;

struct SbeNewOrderV2 : public SbeNewOrderV1 {
    uint32_t participantId;
    uint8_t  timeInForce;    // 0 = GTC, 1 = DAY (default 0 for
                             // v1-encoded messages decoded by v2)
};

inline void writeSbeNewOrderV2Block(uint8_t* out, const SbeNewOrderV2& msg) {
    writeSbeNewOrderV1Block(out, msg);            // first 24 bytes
    writeU32LE(out + 24, msg.participantId);
    out[28] = msg.timeInForce;
    out[29] = 0;
    out[30] = 0;
    out[31] = 0;
}

// Decode a v2 NewOrder block. Pass actualBlockBytes from the
// MessageHeader so this handles both:
//   * a true v2 message (actualBlockBytes == 32) — full read
//   * a v1 message read by a v2 reader (actualBlockBytes == 24)
//     — fill in defaults for the v2-only fields
//   * a future v3 message read by a v2 reader (actualBlockBytes > 32)
//     — read the v2 fields, ignore extra bytes (caller skips the
//     remainder before any repeating groups)
//
// Returns false only on a TOO-SHORT block (a v2 reader can't decode
// a block smaller than v1).
inline bool readSbeNewOrderV2Block(const uint8_t* in,
                                   uint16_t actualBlockBytes,
                                   SbeNewOrderV2& out) {
    if (actualBlockBytes < SBE_NEW_ORDER_V1_BLOCK_BYTES) return false;
    readSbeNewOrderV1Block(in, out);

    if (actualBlockBytes >= SBE_NEW_ORDER_V2_BLOCK_BYTES) {
        out.participantId = readU32LE(in + 24);
        out.timeInForce   = in[28];
    } else {
        // v1-encoded message being read by a v2 reader. The new
        // fields didn't exist on the wire — fill with defaults the
        // schema documents as "absent" sentinels.
        out.participantId = 0;
        out.timeInForce   = 0;  // 0 = GTC
    }
    return true;
}

// ─── Convenience: encode a whole v1 / v2 wire message ───────────────────────
// Returns total bytes written = MessageHeader + block. Caller-
// provided buffer must be ≥ that size.

inline size_t encodeSbeNewOrderV1(uint8_t* out, const SbeNewOrderV1& msg) {
    SbeMessageHeader h{SBE_NEW_ORDER_V1_BLOCK_BYTES,
                       SBE_TEMPLATE_NEW_ORDER,
                       SBE_SCHEMA_ID,
                       SBE_NEW_ORDER_V1};
    writeSbeMessageHeader(out, h);
    writeSbeNewOrderV1Block(out + SBE_MESSAGE_HEADER_BYTES, msg);
    return SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V1_BLOCK_BYTES;
}

inline size_t encodeSbeNewOrderV2(uint8_t* out, const SbeNewOrderV2& msg) {
    SbeMessageHeader h{SBE_NEW_ORDER_V2_BLOCK_BYTES,
                       SBE_TEMPLATE_NEW_ORDER,
                       SBE_SCHEMA_ID,
                       SBE_NEW_ORDER_V2};
    writeSbeMessageHeader(out, h);
    writeSbeNewOrderV2Block(out + SBE_MESSAGE_HEADER_BYTES, msg);
    return SBE_MESSAGE_HEADER_BYTES + SBE_NEW_ORDER_V2_BLOCK_BYTES;
}

// ─── OrderAck v1 ────────────────────────────────────────────────────────────
// Server's reply to a NewOrder. 12-byte block:
//   0..7    orderId       (uint64 LE) — echoes the inbound order id
//   8       status        (uint8)
//                            0 = Accepted
//                            1 = Rejected
//   9       rejectReason  (uint8) — engine RejectReason enum value
//   10..11  padding
//
// Like NewOrderV1, v1 is the baseline; a v2 could add a sequence
// number or per-order latency stamp at the end of the block
// without breaking existing readers.

constexpr uint16_t SBE_TEMPLATE_ORDER_ACK       = 2;
constexpr uint16_t SBE_ORDER_ACK_V1             = 1;
constexpr uint16_t SBE_ORDER_ACK_V1_BLOCK_BYTES = 12;

constexpr uint8_t SBE_ORDER_ACK_STATUS_ACCEPTED = 0;
constexpr uint8_t SBE_ORDER_ACK_STATUS_REJECTED = 1;

struct SbeOrderAckV1 {
    uint64_t orderId;
    uint8_t  status;
    uint8_t  rejectReason;  // 0 = None when status == Accepted
};

inline void writeSbeOrderAckV1Block(uint8_t* out, const SbeOrderAckV1& msg) {
    writeU64LE(out + 0, msg.orderId);
    out[8] = msg.status;
    out[9] = msg.rejectReason;
    out[10] = 0;
    out[11] = 0;
}

inline void readSbeOrderAckV1Block(const uint8_t* in, SbeOrderAckV1& out) {
    out.orderId      = readU64LE(in + 0);
    out.status       = in[8];
    out.rejectReason = in[9];
}

inline size_t encodeSbeOrderAckV1(uint8_t* out, const SbeOrderAckV1& msg) {
    SbeMessageHeader h{SBE_ORDER_ACK_V1_BLOCK_BYTES,
                       SBE_TEMPLATE_ORDER_ACK,
                       SBE_SCHEMA_ID,
                       SBE_ORDER_ACK_V1};
    writeSbeMessageHeader(out, h);
    writeSbeOrderAckV1Block(out + SBE_MESSAGE_HEADER_BYTES, msg);
    return SBE_MESSAGE_HEADER_BYTES + SBE_ORDER_ACK_V1_BLOCK_BYTES;
}

}  // namespace OrderMatcher
