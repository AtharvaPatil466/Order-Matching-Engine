#pragma once
//
// RxMessage — the fixed-size, trivially-copyable payload POD shared by:
//
//   * UdpReorderBuffer (receiver-side gap recovery) — one per ring slot.
//   * UdpRetransmitBuffer (sender-side retransmit window) — one per ring slot.
//
// A datagram's payload (post 8-byte sequence header) is copied ONCE into an
// RxMessage. `bytes` is a
// fixed inline array (no heap, no indirection) so an RxMessage lives directly in
// a pre-allocated ring slot on both the receive and the retransmit paths.

#include <cstdint>

namespace OrderMatcher {

// Upper bound on a single datagram's payload copied into a slot. A datagram
// may pack several frames, so this bounds one *datagram's* payload. A datagram
// exceeding this is truncated to kMaxOrderBytes and logged — never partially
// buffered.
inline constexpr uint16_t kMaxOrderBytes = 512;

// POD, trivially copyable, standard-layout: lives directly in ring slots and is
// safe to std::memcpy. `len` is the number of valid bytes in `bytes`.
struct RxMessage {
    uint16_t len{0};
    uint8_t  bytes[kMaxOrderBytes];
};

}  // namespace OrderMatcher
