#pragma once

// Drive a real OrderEngine process: fork/exec it with a journal, talk to its
// order port over the length-prefixed framing, read its book from the admin
// port, stop it cleanly. Shared by the tests that can only observe the binary
// from outside.

#include "MatchingEngine.h"
#include "TempPath.h"
#include "TcpGateway.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

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

    bool sendRaw(const void* b, size_t n) { return sendAll(b, n); }
    ssize_t recvSome(void* b, size_t n) {
        ssize_t r;
        do { r = ::recv(fd_, b, n, 0); } while (r < 0 && errno == EINTR);
        return r;
    }

private:
    bool sendAll(const void* b, size_t n) {
        auto* p = static_cast<const char*>(b);
        while (n) {
            const ssize_t r = ::send(fd_, p, n, MSG_NOSIGNAL);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            p += r; n -= static_cast<size_t>(r);
        }
        return true;
    }
    bool recvAll(void* b, size_t n) {
        auto* p = static_cast<char*>(b);
        while (n) {
            // EINTR is not an answer. A socket with SO_RCVTIMEO returns it
            // even with no handler installed, e.g. for io_uring task work
            // queued to this thread by a ring it tore down.
            const ssize_t r = ::recv(fd_, p, n, 0);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            p += r; n -= static_cast<size_t>(r);
        }
        return true;
    }
    int fd_{-1};
};

// Start OrderEngine with its order port on `gatewayPort` and `args` as the rest
// of its command line, or return -1 if it exited before that port answered (a
// taken port, or a refusal to boot). Nothing is inherited that would change
// which boot this is.
inline pid_t startEngine(const char* bin, uint16_t gatewayPort,
                         const std::vector<std::string>& args) {
    const pid_t pid = fork();
    if (pid == 0) {
        for (const char* v : {"OB_CONFIG_PATH", "OB_JOURNAL_PATH", "OB_NODE_ROLE",
                              "OB_ADMIN_TOKEN", "OB_ADMIN_NO_AUTH", "OB_ADMIN_BIND",
                              "OB_GATEWAY_PORT", "OB_GATEWAY_BIND",
                              "OB_PARTICIPANT_CREDENTIALS", "OB_NO_PARTICIPANT_AUTH",
                              "OB_REPLAY_LEGACY_JOURNAL"})
            unsetenv(v);
        const std::string port = std::to_string(gatewayPort);
        std::vector<const char*> argv{bin, "--gateway-port", port.c_str()};
        for (const std::string& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        execv(bin, const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    for (int i = 0; i < 600; ++i) {                 // up to 30 s: a sanitizer boot is slow
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return -1;
        Client probe;
        if (probe.connect(gatewayPort)) return pid;
        usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return -1;
}

// GET `path` from an admin port on loopback; the body, or "" if none came.
inline std::string httpGet(uint16_t port, const std::string& path) {
    Client c;
    if (!c.connect(port)) return {};
    const std::string req = "GET " + path + " HTTP/1.0\r\nConnection: close\r\n\r\n";
    if (!c.sendRaw(req.data(), req.size())) return {};
    std::string reply;
    char buf[4096];
    for (ssize_t n; (n = c.recvSome(buf, sizeof(buf))) > 0;) reply.append(buf, static_cast<size_t>(n));
    const size_t body = reply.find("\r\n\r\n");
    return body == std::string::npos ? std::string{} : reply.substr(body + 4);
}

// A process in these tests has its admin port this far above its order port.
inline constexpr uint16_t kAdminPortOffset = 10;

// OrderEngine with `journal`, no order-port credentials and two workers, on the
// first free port from `port` up (which it updates); the admin port follows it
// at kAdminPortOffset.
inline pid_t startOnFreePort(const char* bin, uint16_t& port, const std::string& journal) {
    for (int attempt = 0; attempt < 10; ++attempt, ++port) {
        const pid_t pid = startEngine(bin, port,
                                      {"--journal", journal, "--no-participant-auth",
                                       "--admin-no-auth",
                                       "--port", std::to_string(port + kAdminPortOffset),
                                       "--symbols", "2", "--threads", "2"});
        if (pid > 0) return pid;
    }
    return -1;
}

// The orders resting on `symbol`, summed over the levels of /book, which lists ten
// a side: a test counting with this keeps its orders within ten levels. -1 if
// /book did not answer.
inline int restingOrders(uint16_t adminPort, SymbolId symbol) {
    const std::string body = httpGet(adminPort, "/book?symbolId=" + std::to_string(symbol));
    if (body.empty()) return -1;
    static const std::string kField = "\"orders\":";
    int total = 0;
    for (size_t at = body.find(kField); at != std::string::npos; at = body.find(kField, at + 1)) {
        total += std::atoi(body.c_str() + at + kField.size());
    }
    return total;
}

// Cancel `id` and report whether /book lost an order for it. The answer alone
// cannot say: on the async engine a cancel of an unknown id also answers Ack,
// because the worker decides that (roadmap 1.9-I7), and the ack comes after the
// worker ran, so /book is settled by then.
inline bool cancelRemoved(Client& c, uint16_t adminPort, const OrderRequest& cancel) {
    const int before = restingOrders(adminPort, cancel.symbolId);
    GatewayResponse r{};
    if (!c.send(cancel) || !c.answerFor(cancel.orderId, r)) return false;
    return before >= 0 && restingOrders(adminPort, cancel.symbolId) == before - 1;
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
