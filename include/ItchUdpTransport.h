#pragma once

// ItchUdpTransport — real UDP transport for ITCH-over-MoldUDP64.
//
// Publisher: opens a UDP socket; every flush()/heartbeat()/EOS call
// on the underlying MoldUDP64Publisher writes one datagram via
// sendto() to a configurable address (multicast group in
// production; unicast loopback in tests).
//
// Subscriber: binds a UDP socket; a background thread loops on
// recvfrom() and feeds MoldUDP64Subscriber. The subscriber's
// gap-detection / EOS callbacks fire from that thread.
//
// Multicast vs unicast: the codec doesn't care. Real Nasdaq feeds
// use IP multicast (e.g. 224.0.x.y:port) with IGMP join; this
// transport supports IP_ADD_MEMBERSHIP if you call
// joinMulticastGroup() on the subscriber before start(). Tests use
// plain unicast on 127.0.0.1 to avoid OS-level routing config.

#include "MoldUDP64.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace OrderMatcher {

// ─── Publisher ──────────────────────────────────────────────────────────────

class ItchUdpPublisher {
public:
    ItchUdpPublisher(std::string session, size_t mtu = MOLD_DEFAULT_MTU)
        : session_(std::move(session)), mtu_(mtu) {}

    ~ItchUdpPublisher() { stop(); }

    ItchUdpPublisher(const ItchUdpPublisher&) = delete;
    ItchUdpPublisher& operator=(const ItchUdpPublisher&) = delete;

    // Configure the destination. For multicast set destIp to the
    // group address (e.g. "239.0.0.1"); IP_MULTICAST_TTL defaults
    // to 1, override before start() if you need wider scope.
    bool start(const std::string& destIp, uint16_t destPort,
               uint8_t multicastTtl = 1) {
        if (running_) return false;
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;

        // Set multicast TTL — harmless on unicast destinations.
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                     &multicastTtl, sizeof(multicastTtl));

        std::memset(&dest_, 0, sizeof(dest_));
        dest_.sin_family = AF_INET;
        dest_.sin_port   = htons(destPort);
        if (::inet_pton(AF_INET, destIp.c_str(), &dest_.sin_addr) != 1) {
            ::close(fd_); fd_ = -1; return false;
        }

        // Create the MoldUDP64Publisher with a sink that does sendto().
        // Capturing this by value (the lambda outlives `this`-only
        // construction issues since the lambda lives in mold_) is
        // safe because mold_'s lifetime is bounded by stop()/dtor.
        mold_.reset(new MoldUDP64Publisher(
            session_,
            [this](std::string_view bytes) {
                ::sendto(fd_, bytes.data(), bytes.size(), 0,
                         reinterpret_cast<sockaddr*>(&dest_), sizeof(dest_));
                ++datagramsSent_;
            },
            mtu_));

        running_ = true;
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        if (mold_) {
            mold_->flush();          // don't lose buffered messages
        }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        mold_.reset();
    }

    // Pass-through API to MoldUDP64Publisher. Available only when
    // start() has succeeded; calling before then is a no-op.
    uint64_t publish(const void* payload, uint16_t len) {
        if (!mold_) return 0;
        return mold_->addMessage(payload, len);
    }

    void flush()             { if (mold_) mold_->flush(); }
    void sendHeartbeat()     { if (mold_) mold_->sendHeartbeat(); }
    void sendEndOfSession()  { if (mold_) mold_->sendEndOfSession(); }
    uint64_t nextSequence() const {
        return mold_ ? mold_->nextSequence() : 1;
    }
    uint64_t datagramsSent() const { return datagramsSent_; }

private:
    std::string                              session_;
    size_t                                   mtu_;
    int                                      fd_{-1};
    sockaddr_in                              dest_{};
    bool                                     running_{false};
    std::unique_ptr<MoldUDP64Publisher>      mold_;
    uint64_t                                 datagramsSent_{0};
};

// ─── Subscriber ─────────────────────────────────────────────────────────────

class ItchUdpSubscriber {
public:
    ItchUdpSubscriber() = default;
    ~ItchUdpSubscriber() { stop(); }

    ItchUdpSubscriber(const ItchUdpSubscriber&) = delete;
    ItchUdpSubscriber& operator=(const ItchUdpSubscriber&) = delete;

    // Bind to listenIp:listenPort and start the receive thread.
    // For multicast subscriptions, also call joinMulticastGroup()
    // before start() (or right after — the IP_ADD_MEMBERSHIP works
    // either way as long as the fd is bound).
    bool start(const std::string& listenIp, uint16_t listenPort) {
        if (running_.load()) return false;
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(listenPort);
        if (::inet_pton(AF_INET, listenIp.c_str(), &addr.sin_addr) != 1) {
            ::close(fd_); fd_ = -1; return false;
        }
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd_); fd_ = -1; return false;
        }
        sockaddr_in bound{};
        socklen_t boundLen = sizeof(bound);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &boundLen);
        boundPort_ = ntohs(bound.sin_port);

        // 100ms recv timeout so the recv thread can notice stop().
        timeval tv{0, 100 * 1000};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        running_.store(true);
        recvThread_ = std::thread(&ItchUdpSubscriber::recvLoop, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
        if (recvThread_.joinable()) recvThread_.join();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    // Join an IPv4 multicast group on the bound socket. groupIp is
    // e.g. "239.0.0.1"; interfaceIp can be "0.0.0.0" to let the
    // kernel pick (typical for loopback tests).
    bool joinMulticastGroup(const std::string& groupIp,
                            const std::string& interfaceIp = "0.0.0.0") {
        if (fd_ < 0) return false;
        ip_mreq req{};
        if (::inet_pton(AF_INET, groupIp.c_str(), &req.imr_multiaddr) != 1) {
            return false;
        }
        if (::inet_pton(AF_INET, interfaceIp.c_str(), &req.imr_interface) != 1) {
            return false;
        }
        return ::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                            &req, sizeof(req)) == 0;
    }

    uint16_t boundPort() const { return boundPort_; }

    // Pass-through to expose the underlying subscriber state. Note:
    // the receive thread is the writer for the subscriber's state;
    // counters are observed best-effort under no synchronization,
    // suitable for assertions in tests after the publisher has
    // finished and the subscriber has caught up.
    MoldUDP64Subscriber& mold() { return mold_; }

private:
    void recvLoop() {
        std::vector<uint8_t> buf(64 * 1024);
        while (running_.load()) {
            ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0,
                                   nullptr, nullptr);
            if (n <= 0) continue;  // timeout or interrupted
            mold_.feedPacket(buf.data(), static_cast<size_t>(n));
        }
    }

    std::atomic<bool>       running_{false};
    int                     fd_{-1};
    uint16_t                boundPort_{0};
    std::thread             recvThread_;
    MoldUDP64Subscriber     mold_;
};

}  // namespace OrderMatcher
