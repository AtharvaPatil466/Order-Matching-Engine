#pragma once

// Shared TCP client for the gateway tests. Extracted from
// GatewayIntegrationTest so the login tests drive the same framer rather than
// growing a second, subtly different one.

#include "TcpGateway.h"

#include <cstring>
#include <string_view>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

// MSG_NOSIGNAL is Linux-only; define as 0 on macOS/BSD where
// SO_NOSIGPIPE is used instead.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace OrderMatcher {


class TestClient {
public:
    TestClient() : fd_(-1) {}
    ~TestClient() { disconnect(); }

    bool connect(uint16_t port, int rcvBuf = 0) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        // Shrink the receive window before connect() so a client that stops
        // reading backs the gateway up after KBs rather than MBs.
        if (rcvBuf > 0) setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));

        int flag = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#ifdef SO_NOSIGPIPE
        setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag));
#endif

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd_); fd_ = -1;
            return false;
        }
        return true;
    }

    void disconnect() {
        if (fd_ >= 0) { close(fd_); fd_ = -1; }
    }

    bool sendOrder(const OrderRequest& req) {
        uint32_t len = htonl(sizeof(OrderRequest));
        if (!sendAll(&len, sizeof(len))) return false;
        return sendAll(&req, sizeof(req));
    }

    bool sendOrderV2(const OrderRequest& req, uint64_t clientRequestId) {
        GatewayRequestV2 frame{};
        frame.header.clientRequestId = clientRequestId;
        frame.request = req;
        uint32_t len = htonl(sizeof(frame));
        return sendAll(&len, sizeof(len)) && sendAll(&frame, sizeof(frame));
    }

    // H7 login frame. makeGatewayLogin() owns the flag bit and payload size so
    // the test cannot drift from what a real client would send.
    bool sendLogin(std::string_view user, std::string_view secret,
                   uint64_t clientRequestId = 0) {
        GatewayLoginV2 frame = makeGatewayLogin(user, secret, clientRequestId);
        uint32_t len = htonl(sizeof(frame));
        return sendAll(&len, sizeof(len)) && sendAll(&frame, sizeof(frame));
    }

    // A well-formed V2 order frame carrying a flag the gateway does not know.
    bool sendOrderWithFlags(const OrderRequest& req, uint32_t flags) {
        GatewayRequestV2 frame{};
        frame.header.flags = flags;
        frame.request = req;
        uint32_t len = htonl(sizeof(frame));
        return sendAll(&len, sizeof(len)) && sendAll(&frame, sizeof(frame));
    }

    bool sendUnsupportedV2Version(const OrderRequest& req) {
        GatewayRequestV2 frame{};
        frame.header.version = 99;
        frame.request = req;
        uint32_t len = htonl(sizeof(frame));
        return sendAll(&len, sizeof(len)) && sendAll(&frame, sizeof(frame));
    }

    bool recvResponse(GatewayResponse& resp) {
        uint32_t len;
        if (!recvAll(&len, sizeof(len))) return false;
        len = ntohl(len);
        if (len != sizeof(GatewayResponse)) return false;
        return recvAll(&resp, sizeof(resp));
    }

    bool recvResponseV2(GatewayResponseV2& resp) {
        uint32_t len;
        if (!recvAll(&len, sizeof(len))) return false;
        len = ntohl(len);
        if (len != sizeof(GatewayResponseV2)) return false;
        return recvAll(&resp, sizeof(resp));
    }

    // Bound the blocking recvs so a gateway that answers nothing fails the
    // test instead of hanging the suite.
    void setRecvTimeout(int seconds) {
        struct timeval tv{};
        tv.tv_sec = seconds;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    int fd() const { return fd_; }

private:
    bool sendAll(const void* buf, size_t len) {
        auto* ptr = static_cast<const char*>(buf);
        while (len > 0) {
            ssize_t n = send(fd_, ptr, len, MSG_NOSIGNAL);
            if (n <= 0) return false;
            ptr += n; len -= static_cast<size_t>(n);
        }
        return true;
    }

    bool recvAll(void* buf, size_t len) {
        auto* ptr = static_cast<char*>(buf);
        while (len > 0) {
            ssize_t n = recv(fd_, ptr, len, 0);
            if (n <= 0) return false;
            ptr += n; len -= static_cast<size_t>(n);
        }
        return true;
    }

    int fd_;
};

}  // namespace OrderMatcher
