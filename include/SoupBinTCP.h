#pragma once

// SoupBinTCP — Nasdaq's lightweight TCP session-framing protocol. Wraps
// application messages (OUCH, ITCH gap-fill) with a 3-byte envelope plus a
// session state machine (login, 1s heartbeats, logout). Nasdaq uses it as the
// OUCH order-entry transport and the ITCH multicast gap-recovery channel.
//
// Wire envelope:
//   [length: 2 bytes BE, EXCLUDES the length field][type: 1 byte][payload]
// The length counts type+payload, so a 49-byte OUCH EnterOrder in a Sequenced
// Data packet is 2+1+49=52 wire bytes with the length field reading 50.
//
// Packet types:
//   'S' SequencedData    server→client, gets a seqnum
//   'U' UnsequencedData  client→server (or unsequenced server msgs), no seqnum
//   '+' Debug            diagnostic text
//   'L' LoginRequest / 'A' LoginAccepted / 'J' LoginRejected  (login handshake)
//   'O' LogoutRequest / 'Z' EndOfSession (terminal)
//   'H' ServerHeartbeat / 'R' ClientHeartbeat  every ~1s idle
//
// Heartbeat policy (per spec): emit every 1s idle in the send direction;
// disconnect after 15s of peer silence. Both timers are caller-driven via
// tick(nowMs) — no background thread — keeping the session deterministic.

#include "OuchProtocol.h"  // reuses writeU16BE / readU16BE / writeAsciiDecimal

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace OrderMatcher {

constexpr uint8_t SOUP_PT_SEQUENCED_DATA   = 'S';
constexpr uint8_t SOUP_PT_UNSEQUENCED_DATA = 'U';
constexpr uint8_t SOUP_PT_DEBUG            = '+';
constexpr uint8_t SOUP_PT_LOGIN_REQUEST    = 'L';
constexpr uint8_t SOUP_PT_LOGIN_ACCEPTED   = 'A';
constexpr uint8_t SOUP_PT_LOGIN_REJECTED   = 'J';
constexpr uint8_t SOUP_PT_LOGOUT_REQUEST   = 'O';
constexpr uint8_t SOUP_PT_END_OF_SESSION   = 'Z';
constexpr uint8_t SOUP_PT_SERVER_HEARTBEAT = 'H';
constexpr uint8_t SOUP_PT_CLIENT_HEARTBEAT = 'R';

// LoginRequest payload (client → server). Field widths are
// space-padded ASCII per spec.
//   Username                 [6 bytes]
//   Password                [10 bytes]
//   RequestedSession         [10 bytes] (blank = "give me current")
//   RequestedSequenceNumber  [20 bytes] (blank or 0 = "from end")
// Total payload: 46 bytes.
constexpr size_t SOUP_SIZE_LOGIN_REQUEST_PAYLOAD = 46;

// LoginAccepted payload (server → client).
//   Session                  [10 bytes]
//   SequenceNumber           [20 bytes]
// Total payload: 30 bytes.
constexpr size_t SOUP_SIZE_LOGIN_ACCEPTED_PAYLOAD = 30;

// LoginRejected payload: single character reason code.
constexpr size_t SOUP_SIZE_LOGIN_REJECTED_PAYLOAD = 1;
constexpr char   SOUP_LOGIN_REJECT_NOT_AUTHORIZED = 'A';
constexpr char   SOUP_LOGIN_REJECT_SESSION_NOT_AVAILABLE = 'S';

// Reasonable upper bound for any single packet to bound buffer
// growth on the recv path. The widest real packet is application
// payload-sized; 64KB-1 is the protocol-level max (2-byte length
// field), so this is the natural cap.
constexpr size_t SOUP_MAX_PACKET_PAYLOAD = 65534;  // 65535 - 1 (type byte)

// ─── Envelope helpers ───────────────────────────────────────────────────────

// Write a SoupBinTCP envelope around an application payload. Total
// bytes written = payloadLen + 3. The function does not zero-init
// the buffer or validate the payload — caller decides what type
// byte and payload to use.
inline size_t soupWriteEnvelope(uint8_t* out, uint8_t packetType,
                                const void* payload, size_t payloadLen) {
    // Length = type byte (1) + payload bytes. Excludes the 2 length
    // bytes themselves.
    uint16_t totalLen = static_cast<uint16_t>(payloadLen + 1);
    writeU16BE(out, totalLen);
    out[2] = packetType;
    if (payloadLen > 0 && payload != nullptr) {
        std::memcpy(out + 3, payload, payloadLen);
    }
    return payloadLen + 3;
}

// Convenience: heartbeat packets have no payload. Length=1, type=H or R.
inline size_t soupWriteHeartbeat(uint8_t* out, bool serverSide) {
    return soupWriteEnvelope(out,
        serverSide ? SOUP_PT_SERVER_HEARTBEAT : SOUP_PT_CLIENT_HEARTBEAT,
        nullptr, 0);
}

// Convenience: end-of-session (terminal). Empty payload.
inline size_t soupWriteEndOfSession(uint8_t* out) {
    return soupWriteEnvelope(out, SOUP_PT_END_OF_SESSION, nullptr, 0);
}

// Convenience: logout request (client → server). Empty payload.
inline size_t soupWriteLogoutRequest(uint8_t* out) {
    return soupWriteEnvelope(out, SOUP_PT_LOGOUT_REQUEST, nullptr, 0);
}

// LoginAccepted: pack the session ID and starting sequence number
// into the spec layout. Both are ASCII fields, left-justified and
// space-padded.
inline size_t soupWriteLoginAccepted(uint8_t* out,
                                     std::string_view session,
                                     uint64_t sequenceNumber) {
    uint8_t payload[SOUP_SIZE_LOGIN_ACCEPTED_PAYLOAD];
    writeFixedAscii(payload, 10, session);
    writeAsciiDecimal(payload + 10, 20, sequenceNumber);
    return soupWriteEnvelope(out, SOUP_PT_LOGIN_ACCEPTED,
                             payload, sizeof(payload));
}

inline size_t soupWriteLoginRejected(uint8_t* out, char reasonCode) {
    uint8_t reason = static_cast<uint8_t>(reasonCode);
    return soupWriteEnvelope(out, SOUP_PT_LOGIN_REJECTED, &reason, 1);
}

inline size_t soupWriteLoginRequest(uint8_t* out,
                                    std::string_view username,
                                    std::string_view password,
                                    std::string_view session,
                                    uint64_t requestedSeq) {
    uint8_t payload[SOUP_SIZE_LOGIN_REQUEST_PAYLOAD];
    writeFixedAscii(payload,       6, username);
    writeFixedAscii(payload + 6,  10, password);
    writeFixedAscii(payload + 16, 10, session);
    writeAsciiDecimal(payload + 26, 20, requestedSeq);
    return soupWriteEnvelope(out, SOUP_PT_LOGIN_REQUEST,
                             payload, sizeof(payload));
}

// ─── Decoded view types ─────────────────────────────────────────────────────
// A SoupPacket is a non-owning view over a complete packet in the
// caller's buffer. Use after the framer hands one off.

struct SoupPacket {
    uint8_t        type;
    const uint8_t* payload;
    size_t         payloadLen;
};

struct SoupLoginRequest {
    std::string username;
    std::string password;
    std::string session;     // empty = "current"
    uint64_t    requestedSeq;
};

// Parse a LoginRequest payload. Returns true on a structurally
// well-formed payload (right length). Trims trailing spaces in
// ASCII fields.
inline bool soupParseLoginRequest(const uint8_t* payload, size_t len,
                                  SoupLoginRequest& out) {
    if (len != SOUP_SIZE_LOGIN_REQUEST_PAYLOAD) return false;
    auto trim = [](const uint8_t* p, size_t slot) -> std::string {
        size_t end = slot;
        while (end > 0 && p[end - 1] == ' ') --end;
        return std::string(reinterpret_cast<const char*>(p), end);
    };
    out.username = trim(payload, 6);
    out.password = trim(payload + 6, 10);
    out.session  = trim(payload + 16, 10);
    uint64_t seq = 0;
    // Spec allows blank requestedSequence (= start from end). Treat
    // an unparseable slot as 0 (= start at next-to-be-sequenced).
    if (!parseAsciiDecimal(payload + 26, 20, seq)) seq = 0;
    out.requestedSeq = seq;
    return true;
}

}  // namespace OrderMatcher
