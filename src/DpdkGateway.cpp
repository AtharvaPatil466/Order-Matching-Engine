#include "DpdkGateway.h"

// Entire translation unit is empty unless OB_HAVE_DPDK is defined, so this file
// compiles cleanly on macOS / any non-DPDK build and contributes no symbols.
#if defined(OB_HAVE_DPDK)

// ── DATA PLANE: raw poll-mode DPDK ──────────────────────────────────────────
// We call rte_eth_rx_burst() ourselves on a dedicated port/queue — no F-Stack,
// no TCP stack. Signatures per the DPDK public API; validate against the pinned
// DPDK version on AWS (see reviewer-check notes in DpdkGateway.h).
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_memcpy.h>
#include <rte_lcore.h>
#include <rte_pause.h>

// ── CONTROL PLANE: F-Stack (optional, off by default) ───────────────────────
// F-Stack is retained ONLY for admin/config traffic. Its socket/epoll shim is
// used exactly as before, but it is no longer on the order-entry hot path.
extern "C" {
#include "ff_api.h"
#include "ff_epoll.h"
}

#include "Utils.h"  // Utils::pinThread — the project's affinity helper

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <algorithm>  // std::swap (NAK header build)
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <endian.h>   // htobe64 / htobe32 (NAK wire fields, constraint 7)

namespace OrderMatcher {

namespace {
constexpr int      kMaxControlEvents = 64;
constexpr uint16_t kRxDescriptors    = 1024;   // RX ring depth
constexpr unsigned kNumMbufs         = 8192;    // mbufs in the RX pool (pow2-1 ok)
constexpr unsigned kMbufCacheSize    = 256;
constexpr size_t   kControlRecvBuf   = 4096;
}  // namespace

DpdkGateway::DpdkGateway(MatchingEngine& engine, DpdkConfig config)
    : engine_(engine),
      config_(config),
      rxQueue_(kRxQueueSize) {}

DpdkGateway::~DpdkGateway() { stop(); }

std::unique_ptr<OuchSession> DpdkGateway::makeSession() {
    // The send callback carries OUCH responses (accept/reject/executed/…). Over
    // the RAW UDP data plane there is no TX wired yet: emitting a response would
    // require building an Ethernet/IP/UDP frame and rte_eth_tx_burst() (or
    // routing it out via the control plane). That TX path is a deliberate
    // follow-up — the P2 deliverable is the RECEIVE data plane. Until then we
    // account for the dropped response bytes so the gap is observable rather
    // than silent. The engine still ingests every order (the point of the path).
    return std::make_unique<OuchSession>(
        engine_, [this](std::string_view sv) {
            responseBytesDropped_.fetch_add(sv.size(), std::memory_order_relaxed);
        });
}

bool DpdkGateway::start(int argc, char** argv) {
    if (running_.load(std::memory_order_acquire)) return false;

    // The DATA PLANE owns EAL (see reviewer-check (C) in the header). ff_init()
    // for the optional control plane also initializes EAL, which is why the
    // control plane is disabled by default.
    if (rte_eal_init(argc, argv) < 0) {
        std::fprintf(stderr, "[DPDK] rte_eal_init failed\n");
        return false;
    }

    if (!initPort()) {
        std::fprintf(stderr, "[DPDK] initPort(%u) failed\n", config_.portId);
        return false;
    }

    ouchSession_ = makeSession();

    // Wire the reorder buffer (done after initPort() created the NAK TX pool, so
    // sendNak has its mbufs): deliver in-order datagrams onto the MpscQueue, and
    // emit a NAK when a new gap is detected.
    reorder_.setDeliverHook([this](const RxMessage& m) {
        if (!rxQueue_.push(m)) {
            queueFullDrops_.fetch_add(1, std::memory_order_relaxed);
        }
    });
    reorder_.setNakHook([this](uint64_t seqStart, uint32_t count) {
        sendNak(seqStart, count);
    });

    stopRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    // Consumer first, so the queue is being drained before the poll thread
    // starts producing.
    consumeThread_ = std::thread([this]() { consumeLoop(); });
    pollThread_    = std::thread([this]() { pollLoop(); });

    if (config_.enableControlPlane) {
        controlThread_ = std::thread([this, argc, argv]() {
            controlPlaneThread(argc, argv);
        });
    }
    return true;
}

void DpdkGateway::stop() {
    if (!running_.load(std::memory_order_acquire) &&
        !pollThread_.joinable() && !consumeThread_.joinable() &&
        !controlThread_.joinable()) {
        return;
    }
    stopRequested_.store(true, std::memory_order_release);
    if (pollThread_.joinable())    pollThread_.join();
    if (consumeThread_.joinable()) consumeThread_.join();
    if (controlThread_.joinable()) controlThread_.join();
    running_.store(false, std::memory_order_release);
}

// ── Port bring-up ───────────────────────────────────────────────────────────
bool DpdkGateway::initPort() {
    const uint16_t port = config_.portId;
    if (!rte_eth_dev_is_valid_port(port)) {
        std::fprintf(stderr, "[DPDK] port %u is not valid\n", port);
        return false;
    }

    auto* pool = rte_pktmbuf_pool_create(
        "ob_dpdk_rx_pool", kNumMbufs, kMbufCacheSize, /*priv_size=*/0,
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (pool == nullptr) {
        std::fprintf(stderr, "[DPDK] rte_pktmbuf_pool_create failed\n");
        return false;
    }
    mbufPool_ = pool;

    // One RX queue (orders in) + one TX queue (NAK/resend requests out,
    // Component 1). Real ENA bring-up must validate offloads / RSS / queue counts
    // (reviewer-check (D)).
    struct rte_eth_conf portConf;
    std::memset(&portConf, 0, sizeof(portConf));
    portConf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

    if (rte_eth_dev_configure(port, /*nb_rx=*/1, /*nb_tx=*/1, &portConf) < 0) {
        std::fprintf(stderr, "[DPDK] rte_eth_dev_configure failed\n");
        return false;
    }

    int socketId = rte_eth_dev_socket_id(port);
    if (socketId < 0) socketId = static_cast<int>(rte_socket_id());

    if (rte_eth_rx_queue_setup(port, config_.queueId, kRxDescriptors,
                               static_cast<unsigned>(socketId),
                               /*rx_conf=*/nullptr, pool) < 0) {
        std::fprintf(stderr, "[DPDK] rte_eth_rx_queue_setup failed\n");
        return false;
    }

    // TX queue for fire-and-forget NAKs. TX queues take no mempool (mbufs come
    // from our dedicated NAK pool below).
    if (rte_eth_tx_queue_setup(port, kTxQueue, kTxDescriptors,
                               static_cast<unsigned>(socketId),
                               /*tx_conf=*/nullptr) < 0) {
        std::fprintf(stderr, "[DPDK] rte_eth_tx_queue_setup failed\n");
        return false;
    }

    // Dedicated small-mbuf pool for NAK packets, kept separate from the RX pool so
    // NAK TX never competes for RX mbufs. Data room fits one NakPacket easily.
    auto* txPool = rte_pktmbuf_pool_create(
        "ob_dpdk_nak_pool", kNakMbufs, kNakMbufCache, /*priv_size=*/0,
        RTE_PKTMBUF_HEADROOM + kNakMbufSize, socketId);
    if (txPool == nullptr) {
        std::fprintf(stderr, "[DPDK] NAK tx pool create failed\n");
        return false;
    }
    txMbufPool_ = txPool;

    if (rte_eth_dev_start(port) < 0) {
        std::fprintf(stderr, "[DPDK] rte_eth_dev_start failed\n");
        return false;
    }

    // Co-located order entry: accept the order-entry destination MAC. NIC flow
    // steering (reviewer-check (B)) is expected to direct ONLY the order-entry
    // UDP flow to config_.queueId.
    rte_eth_promiscuous_enable(port);
    return true;
}

// ── Poll thread (pinned, hot) ───────────────────────────────────────────────
// BUSY-POLL by design: this thread spins on rte_eth_rx_burst() and never sleeps,
// eliminating the interrupt/wakeup latency the whole kernel-bypass path exists
// to remove. 100% CPU on this core is EXPECTED AND CORRECT — the core MUST be
// isolated (isolcpus / nohz_full / rcu_nocbs; see docs/OSTuning.md). The loop
// does ONLY: rx_burst -> strip headers -> sequence-check -> MpscQueue::push.
// Parsing + engine submit happen on the consumer thread, off this core.
void DpdkGateway::pollLoop() {
    if (config_.pollCore >= 0) {
        // rte_thread_set_affinity() is the DPDK-native alternative; the project's
        // Utils::pinThread (pthread_setaffinity_np) pins this std::thread just as
        // well and is what the rest of the engine uses.
        Utils::pinThread(config_.pollCore);
    }

    struct rte_mbuf* pkts[kBurstSize];
    const uint16_t port  = config_.portId;
    const uint16_t queue = config_.queueId;
    constexpr uint32_t kMinFrame =
        kL2L3L4HeaderBytes + static_cast<uint32_t>(UdpSequencer::kSeqHeaderBytes);

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const uint16_t nb = rte_eth_rx_burst(port, queue, pkts, kBurstSize);
        if (nb == 0) {
            rte_pause();  // brief spin-loop relax; still no sleep
            continue;
        }

        for (uint16_t i = 0; i < nb; ++i) {
            struct rte_mbuf* m = pkts[i];
            // Single-segment small datagrams: data_len (contiguous first segment)
            // bounds what we may read via mtod. Order datagrams never fragment
            // across mbuf segments.
            const uint32_t frameLen = rte_pktmbuf_data_len(m);

            if (frameLen > kMinFrame) {
                const uint8_t* frame = rte_pktmbuf_mtod(m, const uint8_t*);
                // Learn the sender's MAC/IP once so NAK replies can be addressed
                // (Component 1). Guards internally against a short frame.
                if (!senderLearned_) [[unlikely]] learnSender(frame, frameLen);

                // Zero-copy: read straight out of the mbuf; skip
                // Ethernet(14)+IPv4(20)+UDP(8)=42B (reviewer-check (B)).
                const uint8_t* udp    = frame + kL2L3L4HeaderBytes;
                const uint32_t udpLen = frameLen - kL2L3L4HeaderBytes;

                // First 8 bytes of the UDP payload = big-endian per-datagram seq.
                const uint64_t seq = UdpSequencer::readSeqBE(udp);
                packetsReceived_.fetch_add(1, std::memory_order_relaxed);

                const uint8_t* orderBytes = udp + UdpSequencer::kSeqHeaderBytes;
                uint32_t orderLen =
                    udpLen - static_cast<uint32_t>(UdpSequencer::kSeqHeaderBytes);
                if (orderLen > kMaxOrderBytes) orderLen = kMaxOrderBytes;

                // Hand to the reorder buffer (Component 2): it delivers in order
                // (deliver hook copies into the MpscQueue slot — the single hot-
                // path copy, so the mbuf frees immediately), HOLDS out-of-order
                // datagrams until their hole fills, and emits a NAK for a newly-
                // detected gap (nak hook -> sendNak). This replaces the old
                // observe()+resync-forward that silently lost gaps.
                reorder_.onDatagram(seq, orderBytes,
                                    static_cast<uint16_t>(orderLen));
            } else {
                malformedPackets_.fetch_add(1, std::memory_order_relaxed);
            }

            rte_pktmbuf_free(m);  // return the mbuf to the pool right away
        }
    }
}

// ── Consumer thread (decode + engine submit) ────────────────────────────────
// Drains the MpscQueue and feeds a single persistent OuchSession — the parser +
// engine-submit path reused UNCHANGED. Kept OFF the isolated poll core so frame
// decoding and matching never steal cycles from NIC polling.
void DpdkGateway::consumeLoop() {
    if (config_.consumeCore >= 0) {
        Utils::pinThread(config_.consumeCore);
    }

    RxMessage msg;
    auto feedOne = [this](const RxMessage& m) {
        if (!ouchSession_->feed(reinterpret_cast<const char*>(m.bytes), m.len)) {
            // feed() reports unrecoverable framing (an unknown OUCH type at a
            // frame boundary, or a partial frame that can never complete). Over
            // UDP there is no connection to drop; the persistent parser buffer is
            // now wedged. On a trusted co-lo feed this is a serious fault, so we
            // rebuild the session to resynchronize the parser rather than loop
            // forever on the bad bytes. NOTE (reviewer-check): this discards the
            // in-flight token↔orderId map, so acks/fills for orders that were
            // live at the reset lose their OUCH token mapping. Given assumption
            // (A) — datagrams are message-atomic and whole-framed — a reset here
            // should be rare/never in normal operation.
            ouchSession_ = makeSession();
            framingResets_.fetch_add(1, std::memory_order_relaxed);
        }
    };

    while (!stopRequested_.load(std::memory_order_acquire)) {
        if (rxQueue_.pop(msg)) {
            feedOne(msg);
        } else {
            rte_pause();  // queue empty — relax briefly, then re-check
        }
    }
    // Drain whatever the poll thread produced before shutdown.
    while (rxQueue_.pop(msg)) {
        feedOne(msg);
    }
}

// Standard IPv4 header checksum (RFC 1071) over `n` bytes; returned host-order.
static uint16_t ipv4Checksum(const uint8_t* d, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        sum += (static_cast<uint32_t>(d[i]) << 8) | d[i + 1];
    }
    if (n & 1) sum += static_cast<uint32_t>(d[n - 1]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

// Learn the sender's L2/L3 addressing from the first valid RX frame and pre-build
// the NAK header template (src/dst swapped). Guards against a short/malformed
// frame (constraint 6): it returns and stays unlearned, so the RX path and gap
// counting continue — only the NAK reply waits for a valid frame to learn from.
void DpdkGateway::learnSender(const uint8_t* frame, uint32_t frameLen) {
    if (frameLen < kL2L3L4HeaderBytes) return;  // too short for full L2/L3/L4

    uint8_t* t = reinterpret_cast<uint8_t*>(&nakTemplate_);
    // Base the template on a copy of the received Ethernet(14)+IPv4(20)+UDP(8).
    std::memcpy(t, frame, kL2L3L4HeaderBytes);

    // Ethernet: swap dst[0..5] <-> src[6..11] so the reply goes back to the
    // sender (its src MAC) from us (the received dst MAC == our port MAC).
    for (int i = 0; i < 6; ++i) std::swap(t[i], t[6 + i]);

    // IPv4: src @ off 26, dst @ off 30 (14 + 12 / 14 + 16). Swap so our IP is src.
    for (int i = 0; i < 4; ++i) std::swap(t[26 + i], t[30 + i]);
    // IPv4 total length (off 16) = IP(20) + UDP(8) + payload(seq 8 + count 4).
    const uint16_t ipTotal = 20 + 8 + 12;
    t[16] = static_cast<uint8_t>(ipTotal >> 8);
    t[17] = static_cast<uint8_t>(ipTotal & 0xFF);
    // IPv4 header checksum (off 24..25): zero, then recompute over the 20 IP bytes.
    t[24] = 0;
    t[25] = 0;
    const uint16_t ck = ipv4Checksum(t + 14, 20);
    t[24] = static_cast<uint8_t>(ck >> 8);
    t[25] = static_cast<uint8_t>(ck & 0xFF);

    // UDP: src @ off 34..35, dst @ off 36..37. Swap, then force dst = nakPort so
    // the reply lands on the sender's resend-listener port.
    std::swap(t[34], t[36]);
    std::swap(t[35], t[37]);
    t[36] = static_cast<uint8_t>(config_.nakPort >> 8);
    t[37] = static_cast<uint8_t>(config_.nakPort & 0xFF);
    // UDP length (off 38..39) = UDP(8) + payload(12).
    const uint16_t udpLen = 8 + 12;
    t[38] = static_cast<uint8_t>(udpLen >> 8);
    t[39] = static_cast<uint8_t>(udpLen & 0xFF);
    // UDP checksum (off 40..41): 0 == "not computed", valid for IPv4 UDP.
    t[40] = 0;
    t[41] = 0;

    senderLearned_ = true;
}

// Fire-and-forget NAK for [seqStart, seqStart+count). Never blocks, never retries,
// allocates at most one mbuf from the dedicated NAK pool. On an exhausted pool or
// a full TX ring it frees (if needed) and counts a drop, so the RX poll loop is
// never stalled by a NAK. Returns true iff handed to rte_eth_tx_burst.
bool DpdkGateway::sendNak(uint64_t seqStart, uint32_t count) {
    if (!senderLearned_ || txMbufPool_ == nullptr) {
        // No sender address learned yet (or no pool) — cannot address a reply.
        txNakDrops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto* pool = static_cast<struct rte_mempool*>(txMbufPool_);
    struct rte_mbuf* m = rte_pktmbuf_alloc(pool);
    if (m == nullptr) {  // pool exhausted (e.g. many NAKs faster than TX drains)
        txNakDrops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    constexpr uint16_t kNakLen = static_cast<uint16_t>(sizeof(NakPacket));
    uint8_t* p = reinterpret_cast<uint8_t*>(rte_pktmbuf_append(m, kNakLen));
    if (p == nullptr) {  // mbuf data room too small (should not happen)
        rte_pktmbuf_free(m);
        txNakDrops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    rte_memcpy(p, &nakTemplate_, sizeof(NakPacket));
    auto* nak = reinterpret_cast<NakPacket*>(p);
    nak->seqStart = htobe64(seqStart);  // big-endian on the wire (constraint 7)
    nak->count    = htobe32(count);

    const uint16_t sent = rte_eth_tx_burst(config_.portId, kTxQueue, &m, 1);
    if (sent == 0) {  // TX ring full — drop, do NOT retry or block
        rte_pktmbuf_free(m);
        txNakDrops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // On success the driver owns the mbuf and frees it after transmit completes.
    txNaksSent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ── Control plane (F-Stack, optional) ───────────────────────────────────────
// Runs ff_init()/ff_run() on its own thread to serve admin/config traffic ONLY.
// See reviewer-check (C): enabling this alongside the data plane requires
// reconciling EAL ownership on AWS; it is off by default.
void DpdkGateway::controlPlaneThread(int argc, char** argv) {
    if (ff_init(argc, argv) < 0) {
        std::fprintf(stderr, "[DPDK] control-plane ff_init failed\n");
        return;
    }

    controlListenFd_ = ff_socket(AF_INET, SOCK_STREAM, 0);
    if (controlListenFd_ < 0) return;

    int on = 1;
    ff_setsockopt(controlListenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    ff_ioctl(controlListenFd_, FIONBIO, &on);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config_.controlPort);
    if (ff_bind(controlListenFd_,
                reinterpret_cast<struct linux_sockaddr*>(&addr),
                sizeof(addr)) < 0 ||
        ff_listen(controlListenFd_, config_.controlBacklog) < 0) {
        ff_close(controlListenFd_);
        controlListenFd_ = -1;
        return;
    }

    controlEpollFd_ = ff_epoll_create(0);
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = controlListenFd_;
    ff_epoll_ctl(controlEpollFd_, EPOLL_CTL_ADD, controlListenFd_, &ev);

    ff_run(&DpdkGateway::controlTrampoline, this);
}

int DpdkGateway::controlTrampoline(void* arg) {
    return static_cast<DpdkGateway*>(arg)->controlLoop();
}

int DpdkGateway::controlLoop() {
    if (stopRequested_.load(std::memory_order_acquire)) {
        return -1;  // non-zero → ff_run() returns, unwinding the control thread
    }

    struct epoll_event events[kMaxControlEvents];
    int n = ff_epoll_wait(controlEpollFd_, events, kMaxControlEvents,
                          /*timeout_ms=*/0);
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        if (fd == controlListenFd_) {
            for (;;) {
                int cfd = ff_accept(controlListenFd_, nullptr, nullptr);
                if (cfd < 0) break;
                int on = 1;
                ff_ioctl(cfd, FIONBIO, &on);
                struct epoll_event cev;
                std::memset(&cev, 0, sizeof(cev));
                cev.events = EPOLLIN;
                cev.data.fd = cfd;
                ff_epoll_ctl(controlEpollFd_, EPOLL_CTL_ADD, cfd, &cev);
            }
        } else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
            ff_epoll_ctl(controlEpollFd_, EPOLL_CTL_DEL, fd, nullptr);
            ff_close(fd);
        } else if (events[i].events & EPOLLIN) {
            onControlReadable(fd);
        }
    }
    return 0;
}

void DpdkGateway::onControlReadable(int cfd) {
    // Admin/config seam. A real control protocol handler (config reload,
    // status query, session admin) will live here. For now we drain the socket
    // so it stays edge-clean, and close on peer hangup.
    char buf[kControlRecvBuf];
    for (;;) {
        ssize_t r = ff_recv(cfd, buf, sizeof(buf), 0);
        if (r > 0) continue;             // TODO: dispatch admin/config commands
        if (r == 0) {                    // peer closed
            ff_epoll_ctl(controlEpollFd_, EPOLL_CTL_DEL, cfd, nullptr);
            ff_close(cfd);
            return;
        }
        break;                           // EAGAIN — drained
    }
}

}  // namespace OrderMatcher

#endif  // OB_HAVE_DPDK
