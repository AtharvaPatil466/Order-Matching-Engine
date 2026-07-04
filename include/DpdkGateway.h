#pragma once
//
// DpdkGateway — kernel-bypass OUCH order ingestion via F-Stack (DPDK userspace
// TCP) on a secondary ENI. Entirely gated by OB_HAVE_DPDK: when it is not
// defined (any non-Linux platform, or DPDK/F-Stack not installed) this header
// is empty and src/DpdkGateway.cpp is an empty translation unit, so the kernel
// TcpGateway remains the only ingestion path and the default build is unchanged.
//
// F-Stack owns the ENA port and runs FreeBSD's TCP stack internally (it does the
// rte_eth_rx_burst, IP/TCP reassembly, ACKs, retransmits). We never touch
// rte_eth_rx_burst or mbufs — we use F-Stack's BSD-socket-like API
// (ff_socket/ff_accept/ff_recv/ff_send) plus ff_epoll, so the OuchSession parser
// and the engine submit path are reused UNCHANGED. ff_recv() yields the same
// ordered TCP payload kernel recv() did.
//
// NOTE: F-Stack API signatures below follow its public API (ff_api.h/ff_epoll.h)
// and must be validated against the pinned F-Stack version on AWS — none of this
// path can be compiled or run locally (macOS has no DPDK).

#if defined(OB_HAVE_DPDK)

#include "MatchingEngine.h"
#include "OuchSession.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OrderMatcher {

struct DpdkConfig {
    uint16_t port = 6001;    // OUCH order-entry listen port on the DPDK ENI
    int      backlog = 128;
};

class DpdkGateway {
public:
    DpdkGateway(MatchingEngine& engine, DpdkConfig config);
    ~DpdkGateway();

    DpdkGateway(const DpdkGateway&) = delete;
    DpdkGateway& operator=(const DpdkGateway&) = delete;

    // Spawn the poll-mode lcore thread, which runs ff_init(argc, argv) then
    // ff_run(). argc/argv carry the F-Stack --conf (DPDK EAL args, port, lcore,
    // IP). Returns false only on an early spawn failure — F-Stack init runs on
    // the lcore thread (F-Stack must own its lcore), so a late ff_init failure
    // surfaces via isRunning() going false shortly after start().
    bool start(int argc, char** argv);
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

private:
    // Per-connection state: an OuchSession (parser + response encoder) whose
    // send callback routes to ff_send on this F-Stack fd, plus a TX backlog for
    // bytes ff_send could not take immediately.
    struct Conn {
        std::unique_ptr<OuchSession> session;
        std::vector<char>            pendingTx;
    };

    static int loopTrampoline(void* arg);  // ff_run callback → self->loop()
    int  loop();                           // one poll iteration (the lcore body)
    void onAccept();
    void onReadable(int cfd);
    void onWritable(int cfd);
    void closeConn(int cfd);
    void queueTx(int cfd, std::string_view bytes);

    MatchingEngine&               engine_;
    DpdkConfig                    config_;
    int                           listenFd_{-1};  // F-Stack fd space
    int                           epollFd_{-1};   // ff_epoll fd
    std::unordered_map<int, Conn> conns_;         // F-Stack fd → connection
    std::thread                   lcoreThread_;
    std::atomic<bool>             running_{false};
    std::atomic<bool>             stopRequested_{false};
};

}  // namespace OrderMatcher

#endif  // OB_HAVE_DPDK
