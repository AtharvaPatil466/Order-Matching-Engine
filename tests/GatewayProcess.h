#pragma once

// Drive a real GatewayServer process: fork/exec it on a port with a journal,
// talk to it over the length-prefixed framing, stop it cleanly. Shared by the
// tests that can only observe the binary from outside.

#include "MatchingEngine.h"
#include "TempPath.h"
#include "TcpGateway.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gwtest {

using namespace OrderMatcher;

// The length-prefixed framing CombinedChaosTest already drives TcpGateway with.
class Client {
public:
    ~Client() { if (fd_ >= 0) close(fd_); }
    bool connect(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int flag = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#ifdef SO_NOSIGPIPE
        setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag));
#endif
        timeval tv{5, 0};   // a lost answer must fail the test, not hang it
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    bool send(const OrderRequest& req) {
        const uint32_t len = htonl(sizeof(OrderRequest));
        return sendAll(&len, sizeof(len)) && sendAll(&req, sizeof(req));
    }
    // The Ack or Error answering `orderId`, skipping any notification frames.
    bool answerFor(OrderId orderId, GatewayResponse& out) {
        for (int frames = 0; frames < 64; ++frames) {
            uint32_t len = 0;
            if (!recvAll(&len, sizeof(len)) || ntohl(len) != sizeof(GatewayResponse)) return false;
            if (!recvAll(&out, sizeof(out))) return false;
            const bool isAnswer = out.type == GatewayResponse::Type::Ack ||
                                  out.type == GatewayResponse::Type::Error;
            if (isAnswer && out.orderId == orderId) return true;
        }
        return false;
    }

private:
    bool sendAll(const void* b, size_t n) {
        auto* p = static_cast<const char*>(b);
        while (n) {
            const ssize_t r = ::send(fd_, p, n, MSG_NOSIGNAL);
            if (r <= 0) return false;
            p += r; n -= static_cast<size_t>(r);
        }
        return true;
    }
    bool recvAll(void* b, size_t n) {
        auto* p = static_cast<char*>(b);
        while (n) {
            const ssize_t r = ::recv(fd_, p, n, 0);
            if (r <= 0) return false;
            p += r; n -= static_cast<size_t>(r);
        }
        return true;
    }
    int fd_{-1};
};

// Start GatewayServer on `port` with `journal`, or return -1 if it exited
// before listening (a taken port, or a refusal to boot).
inline pid_t startGateway(const char* bin, uint16_t port, const std::string& journal) {
    const pid_t pid = fork();
    if (pid == 0) {
        // Unset, not blank: a set-but-empty OB_ENGINE_HOST sends the gateway
        // down the forwarding path, and inherited credentials would change
        // which boot this is.
        for (const char* v : {"OB_ENGINE_HOST", "OB_ENGINE_PORT", "OB_JOURNAL_PATH",
                              "OB_PARTICIPANT_CREDENTIALS", "OB_NO_PARTICIPANT_AUTH",
                              "OB_REPLAY_LEGACY_JOURNAL"})
            unsetenv(v);
        const std::string p = std::to_string(port);
        execl(bin, bin, p.c_str(), "--journal", journal.c_str(), "--no-participant-auth",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    for (int i = 0; i < 200; ++i) {                 // up to 10 s
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return -1;
        Client probe;
        if (probe.connect(port)) return pid;
        usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return -1;
}

inline pid_t startOnFreePort(const char* bin, uint16_t& port, const std::string& journal) {
    for (int attempt = 0; attempt < 10; ++attempt, ++port) {
        const pid_t pid = startGateway(bin, port, journal);
        if (pid > 0) return pid;
    }
    return -1;
}

// SIGTERM and wait for a clean exit — the path that flushes the journal.
inline bool stopCleanly(pid_t pid) {
    kill(pid, SIGTERM);
    for (int i = 0; i < 200; ++i) {                 // up to 10 s
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return false;
}

inline std::string tempJournalPath(const std::string& name) {
    return uniqueTempPath(name + ".wal");
}

}  // namespace gwtest
