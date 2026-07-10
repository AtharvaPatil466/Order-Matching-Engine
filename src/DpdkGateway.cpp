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

#include <cerrno>
#include <cstdio>
#include <cstring>

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
    sequencer_.setResendRequestHook(
        [this](uint64_t from, uint64_t to) { onResendRequest(from, to); });

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

    // Minimal single-RX-queue configuration. The data plane is receive-only, so
    // zero TX queues are requested — the OUCH-response/resend TX path is a
    // follow-up (see makeSession / onResendRequest). Real ENA bring-up must
    // validate offloads / RSS / queue counts (reviewer-check (D)).
    struct rte_eth_conf portConf;
    std::memset(&portConf, 0, sizeof(portConf));
    portConf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

    if (rte_eth_dev_configure(port, /*nb_rx=*/1, /*nb_tx=*/0, &portConf) < 0) {
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
                // Zero-copy: read straight out of the mbuf; skip
                // Ethernet(14)+IPv4(20)+UDP(8)=42B (reviewer-check (B)).
                const uint8_t* udp    = frame + kL2L3L4HeaderBytes;
                const uint32_t udpLen = frameLen - kL2L3L4HeaderBytes;

                // First 8 bytes of the UDP payload = big-endian per-datagram seq.
                const uint64_t seq = UdpSequencer::readSeqBE(udp);
                const SeqObservation obs = sequencer_.observe(seq);
                packetsReceived_.fetch_add(1, std::memory_order_relaxed);

                if (obs.accept) {
                    const uint8_t* orderBytes =
                        udp + UdpSequencer::kSeqHeaderBytes;
                    uint32_t orderLen =
                        udpLen - static_cast<uint32_t>(UdpSequencer::kSeqHeaderBytes);
                    if (orderLen > kMaxOrderBytes) orderLen = kMaxOrderBytes;

                    // The ONLY copy on the hot path: a tiny (<=kMaxOrderBytes)
                    // move into the queue slot so the mbuf can be freed
                    // immediately (holding mbufs across the queue would pin the
                    // finite RX pool). This is not the per-byte stream copy TCP
                    // reassembly would force — the receive itself stays zero-copy.
                    RxMessage msg;
                    msg.len = static_cast<uint16_t>(orderLen);
                    std::memcpy(msg.bytes, orderBytes, orderLen);
                    if (!rxQueue_.push(msg)) {
                        queueFullDrops_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
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

void DpdkGateway::onResendRequest(uint64_t fromSeqInclusive,
                                  uint64_t toSeqExclusive) {
    // A gap was detected: datagrams [fromSeqInclusive, toSeqExclusive) appear
    // lost. On a co-located LAN the gateway would emit a NAK / resend request to
    // the order-entry client (via the control plane, or an out-of-band UDP
    // request). That TX is a follow-up alongside the response TX path; for now
    // the gap is counted by UdpSequencer (gaps()/missing()) and surfaced via
    // telemetry so it is observable, not silent.
    std::fprintf(stderr, "[DPDK] order-entry gap: resend [%llu, %llu)\n",
                 static_cast<unsigned long long>(fromSeqInclusive),
                 static_cast<unsigned long long>(toSeqExclusive));
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
