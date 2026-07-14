#pragma once
//
// DpdkGateway — kernel-bypass OUCH order ingestion. Gated by OB_HAVE_DPDK:
// undefined (non-Linux, or DPDK/F-Stack absent) → empty header + empty .cpp, so
// the kernel TcpGateway stays the only ingestion path.
//
// Data plane (orders in): a poll thread pinned to an isolated core calls
// rte_eth_rx_burst() directly, strips the 42B Eth+IPv4+UDP headers, reads the 8B
// per-datagram sequence (UdpSequencer), and pushes the OUCH payload onto an
// MpscQueue; a consumer thread drains it into one persistent OuchSession
// (parser/submit path reused unchanged). No TCP, no reassembly, no kernel. UDP
// is message-atomic; TCP's reliability is replaced by the 64-bit sequence + gap
// detection + resend in UdpSequencer (fine for a trusted co-lo link). Control
// plane (admin/config) is OPTIONAL F-Stack, off the hot path.
//
// UNTESTABLE LOCALLY (no DPDK on macOS) — validate against the pinned DPDK/
// F-Stack version on AWS:
//   (A) Every datagram = 8B big-endian seq + one or more COMPLETE OUCH frames;
//       a frame must not straddle a datagram (sender-side invariant —
//       OuchSession::feed() desyncs on a partial, can't recover mid-stream).
//   (B) Exactly Eth(14)+IPv4-no-options(20)+UDP(8)=42B is stripped; VLAN/IP
//       options/IPv6 shift this. NIC flow steering must send ONLY this UDP flow
//       to our queue.
//   (C) rte_eal_init (data plane) and ff_init (control plane) both init EAL,
//       which inits once per process. Data plane owns EAL; control plane off by
//       default. Enabling both needs EAL-ownership reconciliation on AWS.
//   (D) initPort() uses a single-RX-queue default; validate offloads/RSS/queue
//       counts on the real ENA port.
//   (E) The poll thread busy-spins at 100% CPU by design — its core must be
//       isolated (isolcpus/nohz_full/rcu_nocbs). See docs/OSTuning.md.

#if defined(OB_HAVE_DPDK)

#include "MatchingEngine.h"
#include "MpscQueue.h"
#include "OuchSession.h"
#include "RxMessage.h"         // shared payload POD (kMaxOrderBytes)
#include "UdpReorderBuffer.h"  // Option A gap recovery (receiver side)
#include "UdpSequencer.h"      // kSeqHeaderBytes / readSeqBE (pure helpers)

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

    // UDP L4 destination port of NAK/resend requests sent back to the order-entry
    // client over the raw TX path (Component 1). The sender listens here for
    // retransmit requests; see UdpRetransmitBuffer on the sender side.
    uint16_t nakPort = 6003;

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

    // ── Option A gap-recovery counters (exposed like the telemetry above; the
    //    five rx_* live in the reorder buffer, the two tx_* here) ──────────────
    uint64_t rxGapsDetected()     const { return reorder_.gapsDetected(); }
    uint64_t rxGapDatagrams()     const { return reorder_.gapDatagrams(); }
    uint64_t rxReorderBuffered()  const { return reorder_.reorderBuffered(); }
    uint64_t rxReorderRecovered() const { return reorder_.reorderRecovered(); }
    uint64_t rxReorderOverflow()  const { return reorder_.reorderOverflow(); }
    uint64_t txNaksSent()  const { return txNaksSent_.load(std::memory_order_relaxed); }
    uint64_t txNakDrops()  const { return txNakDrops_.load(std::memory_order_relaxed); }

private:
    // Bytes stripped off the front of every received frame before the OUCH
    // payload: Ethernet(14) + IPv4 no-options(20) + UDP(8). See assumption (B).
    static constexpr uint32_t kL2L3L4HeaderBytes = 14 + 20 + 8;  // 42
    static constexpr uint16_t kBurstSize   = 32;    // rte_eth_rx_burst batch
    static constexpr size_t   kRxQueueSize = 4096;  // MpscQueue capacity (pow2)
    // (RxMessage payload POD + kMaxOrderBytes now live in RxMessage.h so the
    // reorder/retransmit buffers and tests can share them DPDK-free.)

    // ── TX / NAK path (Component 1) ─────────────────────────────────────────
    static constexpr uint16_t kTxQueue       = 1;    // TX queue for NAK/resend
    static constexpr uint16_t kTxDescriptors = 256;  // TX ring depth
    static constexpr unsigned kNakMbufs      = 256;  // dedicated NAK mbuf pool
    static constexpr uint16_t kNakMbufSize   = 128;  // per-mbuf data room (bytes)
    static constexpr unsigned kNakMbufCache  = 32;   // per-lcore cache

    // NAK / resend-request packet. The header bytes are pre-built once
    // (learnSender) from the first received frame with src/dst swapped; only
    // seqStart+count are filled per send. All multi-byte fields are big-endian
    // on the wire (htobe64/htobe32).
    struct __attribute__((packed)) NakPacket {
        uint8_t  etherHeader[14];  // dst = sender MAC, src = our MAC
        uint8_t  ipv4Header[20];   // src/dst swapped from RX; length + csum fixed
        uint8_t  udpHeader[8];     // dst = nakPort; length fixed; csum 0 (IPv4 UDP)
        uint64_t seqStart;         // big-endian: first missing sequence
        uint32_t count;            // big-endian: number of missing datagrams
    };

    bool initPort();          // mempool(s) + configure + rx/tx queue + start
    void pollLoop();          // pinned hot thread: rx_burst -> seq -> reorder
    void consumeLoop();       // decode/submit thread: pop -> OuchSession::feed
    // Build the NAK header template from the first valid RX frame (learn the
    // sender's MAC/IP, src/dst swapped). Skips silently on a too-short/malformed
    // frame (constraint 6) — never crashes.
    void learnSender(const uint8_t* frame, uint32_t frameLen);
    // Fire-and-forget NAK for [seqStart, seqStart+count). Never blocks or retries;
    // on an exhausted TX pool or full TX ring it frees and counts a drop. Returns
    // true iff the datagram was handed to rte_eth_tx_burst.
    bool sendNak(uint64_t seqStart, uint32_t count);
    std::unique_ptr<OuchSession> makeSession();  // session + send callback

    // ── Control plane (F-Stack) — compiled but optional ─────────────────────
    void controlPlaneThread(int argc, char** argv);  // ff_init + ff_run body
    static int controlTrampoline(void* arg);
    int  controlLoop();
    void onControlReadable(int cfd);

    MatchingEngine&              engine_;
    DpdkConfig                   config_;

    // Data plane
    void*                        mbufPool_{nullptr};    // rte_mempool* RX (opaque)
    void*                        txMbufPool_{nullptr};  // rte_mempool* NAK TX (opaque)
    std::unique_ptr<OuchSession> ouchSession_;
    MpscQueue<RxMessage>         rxQueue_;
    UdpReorderBuffer<>           reorder_;              // gap recovery + NAK decision
    bool                         senderLearned_{false}; // NAK template built yet?
                                                        // (poll-thread only)
    NakPacket                    nakTemplate_{};        // pre-built NAK header bytes
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
    // Option A TX-side counters (rx_* gap/reorder counters live in reorder_).
    std::atomic<uint64_t>        txNaksSent_{0};
    std::atomic<uint64_t>        txNakDrops_{0};
};

}  // namespace OrderMatcher

#endif  // OB_HAVE_DPDK
