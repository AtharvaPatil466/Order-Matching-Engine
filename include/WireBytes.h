#pragma once

// WireBytes — big-endian integers and fixed-width ASCII slots, the field
// encodings ITCH 5.0, MoldUDP64 and SoupBinTCP share. Lived in OuchProtocol.h
// until OUCH was deleted (roadmap 1.2); moved verbatim.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace OrderMatcher {

// Raw big-endian byte writes/reads without alignment constraints, so they are
// safe on a packed buffer at any offset.

inline void writeU16BE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void writeU32BE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void writeU64BE(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
    }
}

inline uint16_t readU16BE(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

inline uint32_t readU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

inline uint64_t readU64BE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | uint64_t(p[i]);
    return v;
}

// Write an ASCII string into a fixed-width slot, left-justified and
// space-padded. Truncates if src is longer than the slot.
inline void writeFixedAscii(uint8_t* p, size_t slot, std::string_view src) {
    size_t n = std::min(src.size(), slot);
    if (n) std::memcpy(p, src.data(), n);
    if (n < slot) std::memset(p + n, ' ', slot - n);
}

// Parse a fixed-width ASCII slot as a decimal unsigned integer.
// Treats spaces as padding (skipped at both ends). Returns false on
// any non-digit, non-space character, or empty value.
inline bool parseAsciiDecimal(const uint8_t* p, size_t slot, uint64_t& out) {
    out = 0;
    size_t i = 0;
    while (i < slot && p[i] == ' ') ++i;
    size_t end = slot;
    while (end > i && p[end - 1] == ' ') --end;
    if (i == end) return false;
    uint64_t v = 0;
    for (; i < end; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + uint64_t(p[i] - '0');
    }
    out = v;
    return true;
}

// Render a uint64 as a left-justified, space-padded ASCII slot. Returns
// false if the value doesn't fit (caller decides how to handle).
inline bool writeAsciiDecimal(uint8_t* p, size_t slot, uint64_t v) {
    char buf[24];
    int n = std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    if (n < 0 || static_cast<size_t>(n) > slot) return false;
    std::memcpy(p, buf, n);
    if (static_cast<size_t>(n) < slot) {
        std::memset(p + n, ' ', slot - static_cast<size_t>(n));
    }
    return true;
}

} // namespace OrderMatcher
