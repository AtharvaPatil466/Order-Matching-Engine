#pragma once
//
// DpdkGateway — kernel-bypass OUCH order ingestion. Entirely gated by
// OB_HAVE_DPDK: when it is not defined (any non-Linux platform, or DPDK/F-Stack
// not installed) this header is empty and src/DpdkGateway.cpp is an empty
// translation unit, so the kernel TcpGateway remains the only ingestion path and
// the default build is unchanged.
//
// DATA PLANE vs CONTROL PLANE (the P2 split)
// ------------------------------------------
// The hot order-entry path is now a RAW POLL-MODE DPDK receive, NOT F-Stack's
// TCP stack:
//
//   * DATA PLANE (orders in) — a dedicated poll thread, pinned to an isolated
//     core, calls rte_eth_rx_burst() directly on a DPDK port/queue. For each
//     mbuf it skips the Ethernet(14)+IPv4(20)+UDP(8)=42B headers, reads an
//     8-byte per-datagram sequence number (UdpSequencer), and pushes the OUCH
//     payload onto an MpscQueue. A separate consumer thread drains that queue
//     into a single persistent OuchSession (the parser + engine-submit path is
//     reused UNCHANGED). No TCP state machine, no reassembly, no ACK/retransmit.
//
//   * CONTROL PLANE (admin/config) — OPTIONAL and still F-Stack. When enabled it
//     runs ff_init()/ff_run() on its own thread and serves administrative /
//     configuration traffic over F-Stack's BSD-socket shim. F-Stack is NO LONGER
//     on the order-entry hot path; it exists only for the control plane.
//
// Why UDP + raw poll for order entry
// ----------------------------------
// Co-located clients on the same LAN want the lowest possible order-entry
// latency. Raw poll-mode receive removes the kernel, the TCP stack, and every
// interrupt/wakeup from the path. UDP is message-atomic, so we lose nothing to
// stream reassembly; the reliability TCP would have given us is replaced by the
// explicit 64-bit sequence number + gap detection + resend request in
// UdpSequencer (appropriate for a short, trusted, low-loss co-lo link).
//
// !!! REVIEWER-CHECK ASSUMPTIONS (this path cannot be compiled or run locally;
//     macOS has no DPDK) — validate ALL of these against the pinned DPDK /
//     F-Stack version on AWS:
//   (A) DATAGRAM FRAMING: every UDP datagram carries the 8-byte big-endian
//       sequence header followed by one or MORE *complete* OUCH frames. An OUCH
//       frame MUST NOT straddle a datagram boundary. OuchSession::feed()
//       accumulates a byte stream and only drains whole frames, so a datagram
//       that ends mid-frame would leave a partial in the persistent buffer and
//       desynchronize the parser. On a trusted co-lo sender this is a sender-side
//       invariant, not something the receiver can recover from mid-stream.
//   (B) HEADER LAYOUT: exactly Ethernet(14)+IPv4(20, no options)+UDP(8) = 42B is
//       stripped. VLAN tags, IP options, or IPv6 would change this offset. The
//       NIC/flow config must steer ONLY the order-entry UDP flow to our queue.
//   (C) EAL OWNERSHIP: rte_eal_init() (data plane) and ff_init() (F-Stack control
//       plane) BOTH initialize DPDK's EAL, which may be initialized only once per
//       process. This build has the DATA PLANE own EAL (rte_eal_init in start())
//       and DISABLES the F-Stack control plane by default. Enabling both requires
//       reconciling EAL ownership (shared EAL / secondary process) on AWS.
//   (D) PORT BRING-UP: initPort() uses a single-RX-queue default configuration;
//       the real ENA/port needs its offloads, RSS, and queue counts validated.
//   (E) ISOLATION: the poll thread busy-spins at 100% CPU on its core BY DESIGN.
//       That core MUST be isolated (isolcpus / nohz_full / rcu_nocbs). See
//       docs/OSTuning.md.

#if defined(OB_HAVE_DPDK)

#include "MatchingEngine.h"
#include "MpscQueue.h"
#include "OuchSession.h"
#include "UdpSequencer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace OrderMatcher {

struct DpdkConfig {
    // ── Data plane (raw poll-mode UDP order entry) ──────────────────────────
    uint16_t portId  = 0;   // DPDK ethernet port id of the order-entry ENI
    uint16_t queueId = 0;   // RX queue on that port the poll thread drains
    int      pollCore = -1; // isolated logical core to pin the poll thread to
                            // (isolcpus; see docs/OSTuning.md). -1 = do not pin.
    int      consumeCore = -1;  // optional core for the decode/submit consumer
                                // thread. -1 = do not pin (leave it schedulable).

    // UDP L4 destination port of the order-entry stream. Kept as `port` (not
    // renamed) so the OB_DPDK_PORT wiring in main.cpp keeps compiling. Used only
    // for documentation / optional flow-filter validation — the raw RX path is
    // selected by portId/queueId, and NIC flow steering must direct this UDP
    // flow to queueId.
    uint16_t port = 6001;

    // ── Control plane (F-Stack TCP admin/config) — OPTIONAL ─────────────────
    // Disabled by default: see reviewer-check (C) on EAL ownership. When true,
    // a separate thread runs ff_init()/ff_run() to serve admin/config traffic.
    bool     enableControlPlane = false;
    uint16_t controlPort    = 6002;
    int      controlBacklog = 128;
};

class DpdkGateway {
public:
    DpdkGateway(MatchingEngine& engine, DpdkConfig config);
    ~DpdkGateway();

    DpdkGateway(const DpdkGateway&) = delete;
    DpdkGateway& operator=(const DpdkGateway&) = delete;

    // Initialize EAL from argc/argv (DPDK --  EAL args), bring up the port, then
    // spawn the pinned poll thread + the consumer thread (and, if enabled, the
    // F-Stack control-plane thread). Returns false on any setup failure. argc/argv
    // carry the EAL arguments exactly as the previous F-Stack path carried ff_init
    // args.
    bool start(int argc, char** argv);
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // Telemetry (approximate; read from any thread as counters, not for control).
    uint64_t packetsReceived() const { return packetsReceived_.load(std::memory_order_relaxed); }
    uint64_t malformedPackets() const { return malformedPackets_.load(std::memory_order_relaxed); }
    uint64_t queueFullDrops() const { return queueFullDrops_.load(std::memory_order_relaxed); }
    uint64_t framingResets() const { return framingResets_.load(std::memory_order_relaxed); }
    uint64_t responseBytesDropped() const { return responseBytesDropped_.load(std::memory_order_relaxed); }
    const UdpSequencer& sequencer() const { return sequencer_; }

private:
    // Bytes stripped off the front of every received frame before the OUCH
    // payload: Ethernet(14) + IPv4 no-options(20) + UDP(8). See assumption (B).
    static constexpr uint32_t kL2L3L4HeaderBytes = 14 + 20 + 8;  // 42
    static constexpr uint16_t kBurstSize   = 32;    // rte_eth_rx_burst batch
    static constexpr size_t   kRxQueueSize = 4096;  // MpscQueue capacity (pow2)
    // Upper bound on a single OUCH order message copied into a queue slot. The
    // largest OUCH 4.2 inbound frame (Enter Order) is well under this; a datagram
    // may pack several frames, so this bounds one *datagram's* OUCH payload.
    static constexpr uint16_t kMaxOrderBytes = 512;

    // One handed-off datagram payload: the OUCH bytes (post sequence header),
    // copied out of the mbuf so the mbuf can be freed immediately on the poll
    // core (holding mbufs across the queue would pin the finite mbuf pool).
    // POD + trivially copyable so it lives directly in MpscQueue slots.
    struct RxMessage {
        uint16_t len{0};
        uint8_t  bytes[kMaxOrderBytes];
    };

    bool initPort();          // mempool + configure + rx queue + start
    void pollLoop();          // pinned hot thread: rx_burst -> seq -> push
    void consumeLoop();       // decode/submit thread: pop -> OuchSession::feed
    void onResendRequest(uint64_t fromSeqInclusive, uint64_t toSeqExclusive);
    std::unique_ptr<OuchSession> makeSession();  // session + send callback

    // ── Control plane (F-Stack) — compiled but optional ─────────────────────
    void controlPlaneThread(int argc, char** argv);  // ff_init + ff_run body
    static int controlTrampoline(void* arg);
    int  controlLoop();
    void onControlReadable(int cfd);

    MatchingEngine&              engine_;
    DpdkConfig                   config_;

    // Data plane
    void*                        mbufPool_{nullptr};  // rte_mempool* (opaque here)
    std::unique_ptr<OuchSession> ouchSession_;
    MpscQueue<RxMessage>         rxQueue_;
    UdpSequencer                 sequencer_;
    std::thread                  pollThread_;
    std::thread                  consumeThread_;

    // Control plane
    std::thread                  controlThread_;
    int                          controlListenFd_{-1};  // F-Stack fd space
    int                          controlEpollFd_{-1};

    std::atomic<bool>            running_{false};
    std::atomic<bool>            stopRequested_{false};

    // Telemetry
    std::atomic<uint64_t>        packetsReceived_{0};
    std::atomic<uint64_t>        malformedPackets_{0};
    std::atomic<uint64_t>        queueFullDrops_{0};
    std::atomic<uint64_t>        framingResets_{0};
    std::atomic<uint64_t>        responseBytesDropped_{0};
};

}  // namespace OrderMatcher

#endif  // OB_HAVE_DPDK
