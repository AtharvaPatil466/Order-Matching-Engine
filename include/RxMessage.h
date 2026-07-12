#pragma once
//
// RxMessage — the fixed-size, trivially-copyable payload POD carried across the
// DPDK UDP order-entry path. Extracted from DpdkGateway into a neutral,
// DPDK-free header so the three collaborators can share one definition WITHOUT
// pulling in any DPDK/F-Stack dependency:
//
//   * DpdkGateway (receiver, OB_HAVE_DPDK only) — MpscQueue<RxMessage> ring slot
//     and the reorder-buffer slot payload.
//   * UdpReorderBuffer (receiver-side gap recovery) — one per ring slot.
//   * UdpRetransmitBuffer (sender-side retransmit window) — one per ring slot.
//
// A datagram's OUCH payload (post 8-byte sequence header) is copied ONCE into an
// RxMessage so the mbuf can be freed immediately on the poll core. `bytes` is a
// fixed inline array (no heap, no indirection) so an RxMessage lives directly in
// a pre-allocated ring slot on both the receive and the retransmit paths.

#include <cstdint>

namespace OrderMatcher {

// Upper bound on a single datagram's OUCH payload copied into a slot. The
// largest OUCH 4.2 inbound frame (Enter Order) is well under this; a datagram
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
