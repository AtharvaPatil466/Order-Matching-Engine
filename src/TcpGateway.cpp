#include "TcpGateway.h"
#include "FaultInjector.h"
#include "Metrics.h"
#include "StructuredLog.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cerrno>

// MSG_NOSIGNAL is Linux-only; on macOS/BSD SO_NOSIGPIPE is set per-socket.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace OrderMatcher {

namespace {

static_assert(sizeof(GatewayRequestHeader) == GATEWAY_REQUEST_HEADER_SIZE,
              "GatewayRequestHeader wire size changed");
static_assert(sizeof(GatewayResponseHeader) == GATEWAY_RESPONSE_HEADER_SIZE,
              "GatewayResponseHeader wire size changed");

const char* rejectReasonToString(RejectReason reason) {
    switch (reason) {
    case RejectReason::None: return "none";
    case RejectReason::VolatilityCircuitBreaker: return "volatility circuit breaker";
    case RejectReason::PostOnlyWouldCross: return "post-only would cross";
    case RejectReason::FOKInsufficientLiquidity: return "FOK insufficient liquidity";
    case RejectReason::RiskLimitBreached: return "risk limit breached";
    case RejectReason::InvalidPrice: return "invalid price";
    case RejectReason::InvalidQuantity: return "invalid quantity";
    case RejectReason::InvalidDisplayQty: return "invalid display quantity";
    case RejectReason::SymbolNotFound: return "symbol not found";
    case RejectReason::OrderNotFound: return "order not found";
    case RejectReason::NotOrderOwner: return "order not found";
    case RejectReason::OutOfPriceRange: return "out of price range";
    case RejectReason::CapacityExhausted: return "capacity exhausted";
    case RejectReason::RateLimitExceeded: return "rate limit exceeded";
    case RejectReason::QueueBackpressure: return "queue backpressure";
    case RejectReason::EngineStopped: return "engine stopped";
    case RejectReason::MarketHalted: return "market halted";
    case RejectReason::OrderTypeNotAllowedInState: return "order type not allowed in current state";
    case RejectReason::OutsidePriceBand: return "outside price band";
    case RejectReason::MarketClosed: return "market closed";
    case RejectReason::DuplicateOrderId: return "duplicate orderId";
    case RejectReason::UnsupportedFixVersion: return "unsupported FIX version";
    case RejectReason::MissingRequiredField: return "missing required field";
    case RejectReason::KillSwitchActive: return "kill switch active";
    case RejectReason::PositionLimitExceeded: return "position limit exceeded";
    case RejectReason::FatFingerReject: return "fat-finger reject";
    case RejectReason::OrderToTradeRatioExceeded: return "order-to-trade ratio exceeded";
    case RejectReason::PoolCapacityExceeded: return "pool capacity exceeded";
    }
    return "unknown reject";
}

} // namespace

void TcpGateway::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool TcpGateway::isIPAllowed(uint32_t ip) const {
    if (allowedIPs_.empty()) return true; // open mode
    return allowedIPs_.count(ip) > 0;
}

bool TcpGateway::start(uint16_t port, int backlog) {
    if (running_) return false;

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "TcpGateway socket failed: " << std::strerror(errno) << "\n";
        return false;
    }

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "TcpGateway bind failed: " << std::strerror(errno) << "\n";
        close(listenFd_); listenFd_ = -1;
        return false;
    }

    if (listen(listenFd_, backlog) < 0) {
        std::cerr << "TcpGateway listen failed: " << std::strerror(errno) << "\n";
        close(listenFd_); listenFd_ = -1;
        return false;
    }

    // Get actual bound port
    struct sockaddr_in boundAddr{};
    socklen_t boundLen = sizeof(boundAddr);
    getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&boundAddr), &boundLen);
    port_ = ntohs(boundAddr.sin_port);

    setNonBlocking(listenFd_);

    // Create event loop fd
#ifdef USE_KQUEUE
    evFd_ = kqueue();
#elif defined(USE_EPOLL)
    evFd_ = epoll_create1(0);
#endif
    if (evFd_ < 0) {
        std::cerr << "TcpGateway event loop fd failed: " << std::strerror(errno) << "\n";
        close(listenFd_); listenFd_ = -1;
        return false;
    }

    addToEventLoop(listenFd_);

    // Create self-pipe for graceful shutdown
    if (pipe(shutdownPipe_) == 0) {
        setNonBlocking(shutdownPipe_[0]);
        addToEventLoop(shutdownPipe_[0]);
    }

    running_ = true;
    eventThread_ = std::thread(&TcpGateway::eventLoop, this);
    return true;
}

void TcpGateway::stop() {
    if (!running_) return;
    running_ = false;

    // Wake event loop immediately via self-pipe
    wakeEventLoop();

    // Join event loop thread first — it will exit on next timeout since running_=false
    if (eventThread_.joinable())
        eventThread_.join();

    // Now safe to clean up — event loop is no longer running
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto& [fd, _] : clients_) {
            close(fd);
        }
        clients_.clear();
    }

    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }

    if (evFd_ >= 0) {
        close(evFd_);
        evFd_ = -1;
    }

    for (int i = 0; i < 2; ++i) {
        if (shutdownPipe_[i] >= 0) {
            close(shutdownPipe_[i]);
            shutdownPipe_[i] = -1;
        }
    }
}

void TcpGateway::addToEventLoop(int fd) {
#ifdef USE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    kevent(evFd_, &ev, 1, nullptr, 0, nullptr);
#elif defined(USE_EPOLL)
    // Level-triggered, NOT EPOLLET. Edge-triggered epoll only re-notifies when
    // the socket transitions to readable, so it is only safe if every wake-up
    // drains to a genuine EAGAIN. handleClientData() does not: it also stops
    // when the framing buffer is full, and the fault injector's spurious
    // EAGAIN stops it deliberately — with a comment saying the data will be
    // picked up on "the next epoll wake-up", which is a level-triggered
    // assumption. Under EPOLLET there was no next wake-up: the level never
    // changed, so bytes already in the socket sat there and the client waited
    // forever for a response. kqueue is level-triggered by default, which is
    // why this only ever failed on Linux.
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(evFd_, EPOLL_CTL_ADD, fd, &ev);
#endif
}

void TcpGateway::removeFromEventLoop(int fd) {
#ifdef USE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    kevent(evFd_, &ev, 1, nullptr, 0, nullptr);
#elif defined(USE_EPOLL)
    epoll_ctl(evFd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
}

void TcpGateway::addWriteToEventLoop(int fd) {
#ifdef USE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    kevent(evFd_, &ev, 1, nullptr, 0, nullptr);
#elif defined(USE_EPOLL)
    // Level-triggered here too (see addToEventLoop). EPOLLOUT stays armed only
    // while writeBuf has bytes — flushWriteBuffer() calls
    // removeWriteFromEventLoop() the moment it drains — so level-triggered
    // cannot spin on a writable idle socket.
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = fd;
    epoll_ctl(evFd_, EPOLL_CTL_MOD, fd, &ev);
#endif
}

void TcpGateway::removeWriteFromEventLoop(int fd) {
#ifdef USE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(evFd_, &ev, 1, nullptr, 0, nullptr);
#elif defined(USE_EPOLL)
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(evFd_, EPOLL_CTL_MOD, fd, &ev);
#endif
}

void TcpGateway::wakeEventLoop() {
    if (shutdownPipe_[1] < 0) return;

    // A (void) cast does NOT silence glibc's warn_unused_result on write(),
    // only Clang's — which is why this compiled on macOS and broke every Linux
    // lane under -Werror. Consume the result and say what each outcome means.
    const char c = 1;
    ssize_t n;
    do {
        n = write(shutdownPipe_[1], &c, 1);
    } while (n < 0 && errno == EINTR);

    // EAGAIN: an unread wake byte is already queued, so the loop is going to
    // wake regardless. Anything else means this wake was lost — not fatal, the
    // loop also polls on a 10ms timeout, but it should not pass unrecorded.
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        obSink().log(obEvent("gateway_wake_failed").kv("errno", (long long)errno));
    }
}

void TcpGateway::flushWriteBuffer(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    ClientState& state = it->second;

    while (!state.writeBuf.empty()) {
        ssize_t n = send(fd, state.writeBuf.data(), state.writeBuf.size(), MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                addWriteToEventLoop(fd); // register for write-ready
                return;
            }
            removeClient(fd);
            return;
        }
        state.writeBuf.erase(state.writeBuf.begin(), state.writeBuf.begin() + n);
    }
    removeWriteFromEventLoop(fd);
}

void TcpGateway::checkIdleTimeouts() {
    auto now = std::chrono::steady_clock::now();
    std::vector<int> toRemove;
    for (auto& [fd, state] : clients_) {
        if (now - state.lastActivity > idleTimeout_) {
            toRemove.push_back(fd);
        }
    }
    for (int fd : toRemove) {
        removeClient(fd);
    }
}

void TcpGateway::eventLoop() {
    constexpr int MAX_EVENTS = 64;

#ifdef USE_KQUEUE
    struct kevent events[MAX_EVENTS];
    struct timespec timeout{0, 10000000}; // 10ms

    while (running_) {
        int n = kevent(evFd_, nullptr, 0, events, MAX_EVENTS, &timeout);
        for (int i = 0; i < n; ++i) {
            int fd = static_cast<int>(events[i].ident);
            if (fd == shutdownPipe_[0]) {
                return; // graceful shutdown
            } else if (events[i].flags & EV_EOF) {
                removeClient(fd);
            } else if (fd == listenFd_) {
                handleAccept();
            } else if (events[i].filter == EVFILT_WRITE) {
                flushWriteBuffer(fd);
            } else {
                handleClientData(fd);
            }
        }
        checkIdleTimeouts();
    }

#elif defined(USE_EPOLL)
    struct epoll_event events[MAX_EVENTS];

    while (running_) {
        int n = epoll_wait(evFd_, events, MAX_EVENTS, 10);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == shutdownPipe_[0]) {
                return; // graceful shutdown
            } else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                removeClient(fd);
            } else if (fd == listenFd_) {
                handleAccept();
            } else {
                if (events[i].events & EPOLLOUT) {
                    flushWriteBuffer(fd);
                }
                if (events[i].events & EPOLLIN) {
                    handleClientData(fd);
                }
            }
        }
        checkIdleTimeouts();
    }
#endif
}

void TcpGateway::handleAccept() {
    while (true) {
        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = accept(listenFd_, reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen);
        if (clientFd < 0) break; // EAGAIN — no more pending connections

        // IP whitelist check
        if (!isIPAllowed(clientAddr.sin_addr.s_addr)) {
            close(clientFd);
            continue;
        }

        // Configure client socket
        int flag = 1;
        setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#ifdef SO_NOSIGPIPE
        setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag));
#endif
        setNonBlocking(clientFd);

        // Register client
        ClientState state;
        state.readBuf.resize(sizeof(uint32_t));
        state.readPos = 0;
        state.addr = clientAddr;
        state.lastActivity = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_[clientFd] = std::move(state);
        }

        // Stable client identifier for logging — IPv4 dotted-quad + port.
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipbuf, sizeof(ipbuf));
        char remote[64];
        std::snprintf(remote, sizeof(remote), "%s:%u", ipbuf,
                      ntohs(clientAddr.sin_port));
        obSink().log(obEvent("gateway_accept")
                         .kv("fd", (long long)clientFd)
                         .kv("remote", remote));
        static auto& kConnTotal = MetricsRegistry::instance().counter(
            "gateway_connections_total",
            "Cumulative count of accepted TCP connections");
        kConnTotal.increment();

        addToEventLoop(clientFd);
    }
}

void TcpGateway::handleClientData(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;

    ClientState& state = it->second;

    // Read as much as available
    auto& fi = FaultInjector::instance();
    while (true) {
        size_t targetSize = state.readBuf.size();
        size_t remaining = targetSize - state.readPos;
        if (remaining == 0) break;

        // Fault: simulate spurious EAGAIN — even if data is available, defer
        // to the next epoll wake-up. The framing buffer must tolerate this.
        if (fi.shouldFail("gateway.recv.eagain")) {
            break;
        }

        // Fault: simulate TCP fragmentation by asking the kernel for a single
        // byte at a time. The remaining bytes stay in the socket buffer and
        // are drained on subsequent iterations; the framing accumulator must
        // reassemble them into one OrderRequest frame.
        size_t askFor = remaining;
        if (askFor > 1 && fi.shouldFail("gateway.recv.short_read")) {
            askFor = 1;
        }

        ssize_t n = recv(fd, state.readBuf.data() + state.readPos, askFor, 0);
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                removeClient(fd);
                return;
            }
            break; // EAGAIN — no more data
        }
        state.readPos += static_cast<size_t>(n);
        state.lastActivity = std::chrono::steady_clock::now();

        // Frame + decode + submit every whole frame now buffered (shared with
        // any kernel-bypass transport). false = client removed — stop.
        if (!feedBytes(fd, state)) {
            return;
        }
    }
}

bool TcpGateway::feedBytes(int fd, ClientState& state) {
    while (state.readPos >= sizeof(uint32_t)) {
        uint32_t msgLen;
        std::memcpy(&msgLen, state.readBuf.data(), sizeof(uint32_t));
        msgLen = ntohl(msgLen);

        if (msgLen == 0 || msgLen > GATEWAY_MAX_FRAME_SIZE) {
            GatewayResponse resp{};
            resp.type = GatewayResponse::Type::Error;
            std::snprintf(resp.errorMessage, sizeof(resp.errorMessage),
                         "Invalid message size: %u", msgLen);
            sendResponse(fd, resp);
            removeClient(fd);
            return false;
        }

        size_t totalMsgSize = sizeof(uint32_t) + static_cast<size_t>(msgLen);
        if (state.readBuf.size() < totalMsgSize) {
            state.readBuf.resize(totalMsgSize);
            break;
        }
        if (state.readPos < totalMsgSize) {
            break;
        }

        DecodedFrame frame{};
        GatewayResponse errorResp{};
        if (!decodeFrame(state, msgLen, frame, errorResp)) {
            sendResponse(fd, errorResp);
            removeClient(fd);
            return false;
        }
        if (frame.kind == DecodedFrame::Kind::Login) {
            if (!handleLogin(fd, state, frame.login)) {
                removeClient(fd);
                return false;
            }
        } else {
            processMessage(fd, state, frame.order);
        }

        // processMessage() may have blown the outbound queue ceiling. Reap
        // here, where `state` is still alive, rather than inside sendResponse.
        if (state.writeBufOverflowed) {
            removeClient(fd);
            return false;
        }

        // Shift buffer (in case of pipelined messages)
        size_t consumed = totalMsgSize;
        if (state.readPos > consumed) {
            std::memmove(state.readBuf.data(), state.readBuf.data() + consumed, state.readPos - consumed);
        }
        state.readPos -= consumed;
        size_t keep = std::max(sizeof(uint32_t), state.readPos);
        if (state.readBuf.size() != keep) {
            state.readBuf.resize(keep);
        }
    }
    return true;
}

bool TcpGateway::decodeFrame(ClientState& state, uint32_t msgLen,
                             DecodedFrame& out, GatewayResponse& errorResp) {
    const char* payload = state.readBuf.data() + sizeof(uint32_t);
    errorResp.type = GatewayResponse::Type::Error;
    out.kind = DecodedFrame::Kind::Order;

    if (msgLen == sizeof(OrderRequest)) {
        // Bare V1 frame: no header, so no room for a flag and no way to log
        // in. Under auth a V1 client is simply never authenticated, and
        // processMessage() rejects it.
        std::memcpy(&out.order, payload, sizeof(OrderRequest));
        state.protocolVersion = GATEWAY_PROTOCOL_V1;
        state.lastClientRequestId = 0;
        return true;
    }

    if (msgLen < sizeof(GatewayRequestHeader)) {
        std::snprintf(errorResp.errorMessage, sizeof(errorResp.errorMessage),
                      "Invalid gateway frame: too small");
        return false;
    }

    GatewayRequestHeader header{};
    std::memcpy(&header, payload, sizeof(header));
    if (header.magic != GATEWAY_PROTOCOL_MAGIC) {
        std::snprintf(errorResp.errorMessage, sizeof(errorResp.errorMessage),
                      "Invalid gateway frame magic");
        return false;
    }
    if (header.version != GATEWAY_PROTOCOL_V2) {
        std::snprintf(errorResp.errorMessage, sizeof(errorResp.errorMessage),
                      "Unsupported gateway protocol version: %u",
                      static_cast<unsigned>(header.version));
        return false;
    }
    if (header.headerSize < sizeof(GatewayRequestHeader) ||
        header.headerSize > msgLen) {
        std::snprintf(errorResp.errorMessage, sizeof(errorResp.errorMessage),
                      "Invalid gateway frame header size");
        return false;
    }
    if ((header.flags & ~GATEWAY_FLAGS_KNOWN) != 0) {
        std::snprintf(errorResp.errorMessage, sizeof(errorResp.errorMessage),
                      "Unsupported gateway frame flags: 0x%x",
                      static_cast<unsigned>(header.flags));
        return false;
    }

    const bool isLogin = (header.flags & GATEWAY_FLAG_LOGIN) != 0;
    const uint32_t expectedPayload =
        isLogin ? sizeof(GatewayLoginRequest) : sizeof(OrderRequest);
    if (header.payloadSize != expectedPayload ||
        header.headerSize + header.payloadSize != msgLen) {
        std::snprintf(errorResp.errorMessage, sizeof(errorResp.errorMessage),
                      "Invalid gateway frame payload size");
        return false;
    }

    if (isLogin) {
        out.kind = DecodedFrame::Kind::Login;
        std::memcpy(&out.login, payload + header.headerSize, sizeof(out.login));
    } else {
        std::memcpy(&out.order, payload + header.headerSize, sizeof(out.order));
    }
    state.protocolVersion = header.version;
    state.lastClientRequestId = header.clientRequestId;
    return true;
}

// A fixed-width wire field, bounded. strnlen so an unterminated field stops at
// the struct boundary instead of reading into whatever follows.
static std::string_view wireField(const char* p, size_t cap) {
    return std::string_view(p, ::strnlen(p, cap));
}

bool TcpGateway::handleLogin(int fd, ClientState& state,
                             const GatewayLoginRequest& login) {
    GatewayResponse resp{};
    resp.orderId      = 0;
    resp.rejectReason = RejectReason::None;

    if (!auth_ || !auth_->enabled()) {
        // Say so explicitly rather than letting authenticate() fail against an
        // empty store. A client that thinks it is authenticating to a gateway
        // that enforces nothing is a misconfiguration worth surfacing at the
        // first connection, not at the first incident.
        resp.type = GatewayResponse::Type::Error;
        std::snprintf(resp.errorMessage, sizeof(resp.errorMessage),
                      "gateway has no credential store configured");
        sendResponse(fd, resp);
        return false;
    }

    const std::string_view user = wireField(login.user, sizeof(login.user));
    AuthorizedIdentity id =
        auth_->authenticate(user, wireField(login.secret, sizeof(login.secret)));

    if (!id.valid()) {
        // One attempt per connection: the client must reconnect to try again,
        // which puts the accept path's cost and the IP allow-list in front of
        // an online guessing loop.
        obSink().log(obEvent("gateway_login_rejected", LogSeverity::Warn)
                         .kv("user", user));
        resp.type = GatewayResponse::Type::Error;
        std::snprintf(resp.errorMessage, sizeof(resp.errorMessage),
                      "login rejected");
        sendResponse(fd, resp);
        return false;
    }

    state.identity = std::move(id);
    obSink().log(obEvent("gateway_login_ok")
                     .kv("user", state.identity.user())
                     .kv("participants", (long long)state.identity.allowed().size()));

    resp.type = GatewayResponse::Type::Ack;
    sendResponse(fd, resp);
    return true;
}

void TcpGateway::processMessage(int fd, ClientState& state, const OrderRequest& req) {
    // H7: req.participantId is a claim the client makes about itself. With a
    // credential store configured, the session has to have logged in and the
    // credential has to cover the participant being claimed — otherwise any
    // connection that reaches this port could trade on another firm's account
    // or, through Type::KillSwitch below, halt their trading outright.
    //
    // Still open: Cancel/Modify/CancelReplace address an order by id alone and
    // the engine does not check who owns it, so a logged-in session can still
    // act on another participant's resting order. Closing that needs the
    // requesting participant threaded into MatchingEngine's cancel path; the
    // check below does not cover it.
    if (auth_ && auth_->enabled()) {
        const char* denial = nullptr;
        if (!state.identity.valid()) {
            denial = "not logged in; send a login frame first";
        } else if (!state.identity.permits(req.participantId)) {
            denial = "credential does not cover the requested participant";
        }
        if (denial) {
            obSink().log(obEvent("gateway_unauthorized_request", LogSeverity::Warn)
                             .kv("user", state.identity.valid()
                                             ? std::string_view(state.identity.user())
                                             : std::string_view("-"))
                             .kv("claimed_participant", (long long)req.participantId)
                             .kv("request_type", (long long)static_cast<int>(req.type)));
            GatewayResponse denied{};
            denied.type = GatewayResponse::Type::Error;
            denied.orderId = req.orderId;
            denied.rejectReason = RejectReason::None;
            std::snprintf(denied.errorMessage, sizeof(denied.errorMessage), "%s", denial);
            sendResponse(fd, denied);
            return;
        }
    }

    SubmitResult result = SubmitResult::rejected(RejectReason::EngineStopped);

    // Ownership enforcement is only meaningful once the identity is verified.
    // With auth off, req.participantId is an unchecked claim, so enforcing
    // against it buys nothing and would reject clients that never filled the
    // field in — the check above has already established which case this is.
    const ParticipantId requester =
        (auth_ && auth_->enabled()) ? req.participantId : kAnyParticipant;

    switch (req.type) {
        case OrderRequest::Type::NewOrder:
            result = engine_.submitOrder(req.symbolId, req.orderId, req.participantId,
                                         req.side, req.price, req.qty, req.orderType,
                                         req.stopPrice, req.displayQty, req.tif, req.expiryTime,
                                         req.stopLimitPrice, req.pegType, req.pegOffset,
                                         req.trailAmount, req.minQty, req.hidden);
            break;
        case OrderRequest::Type::Cancel:
            result = engine_.submitCancel(req.symbolId, req.orderId, requester);
            break;
        case OrderRequest::Type::Modify:
            result = engine_.submitModify(req.symbolId, req.orderId, req.newQty, requester);
            break;
        case OrderRequest::Type::CancelReplace:
            result = engine_.submitCancelReplace(req.symbolId, req.orderId,
                                                req.newPrice, req.newQty, requester);
            break;
        case OrderRequest::Type::KillSwitch:
            engine_.killSwitch(req.participantId);
            result = SubmitResult::accepted(0);
            break;
        default:
            result = SubmitResult::rejected(RejectReason::InvalidQuantity);
            break;
    }

    GatewayResponse resp{};
    resp.orderId = req.orderId;
    // Collapse NotOrderOwner into OrderNotFound before the raw enum byte goes
    // out: this field IS the wire value, so leaking the distinction here would
    // undo the point of keeping them separate internally.
    resp.rejectReason = clientVisibleReason(result.rejectReason);
    resp.sequenceId = result.sequenceId;
    if (result.isAccepted()) {
        resp.type = GatewayResponse::Type::Ack;
    } else {
        resp.type = GatewayResponse::Type::Error;
        std::snprintf(resp.errorMessage, sizeof(resp.errorMessage),
                      "Rejected: %s",
                      rejectReasonToString(clientVisibleReason(result.rejectReason)));
    }
    sendResponse(fd, resp);
}

void TcpGateway::removeClient(int fd) {
    removeFromEventLoop(fd);
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.erase(fd);
    }
    close(fd);
    obSink().log(obEvent("gateway_disconnect").kv("fd", (long long)fd));
    static auto& kDiscTotal = MetricsRegistry::instance().counter(
        "gateway_disconnects_total",
        "Cumulative count of TCP disconnects");
    kDiscTotal.increment();
}

bool TcpGateway::sendResponse(int fd, const GatewayResponse& resp) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return false;
    ClientState& state = it->second;

    GatewayResponseV2 v2{};
    const char* payload = reinterpret_cast<const char*>(&resp);
    uint32_t payloadLen = sizeof(GatewayResponse);
    if (state.protocolVersion == GATEWAY_PROTOCOL_V2) {
        v2.header.version = GATEWAY_PROTOCOL_V2;
        v2.header.clientRequestId = state.lastClientRequestId;
        v2.response = resp;
        payload = reinterpret_cast<const char*>(&v2);
        payloadLen = sizeof(v2);
    }

    uint32_t len = htonl(payloadLen);
    std::vector<char> buf(sizeof(uint32_t) + payloadLen);
    std::memcpy(buf.data(), &len, sizeof(uint32_t));
    std::memcpy(buf.data() + sizeof(uint32_t), payload, payloadLen);

    // Refuse to queue past the per-client ceiling. Dropping the connection is
    // the only bound available: the client is not idle (it keeps sending, so
    // lastActivity keeps refreshing) and it is not reading, so the queue would
    // otherwise grow until the process dies. Flag it rather than removing here
    // — feedBytes() still holds a reference into clients_ for this fd.
    if (state.writeBuf.size() + buf.size() > kMaxWriteBufBytes) {
        state.writeBufOverflowed = true;
        state.writeBuf.clear();
        state.writeBuf.shrink_to_fit();
        obSink().log(obEvent("gateway_write_buffer_overflow").kv("fd", (long long)fd));
        return false;
    }

    // If write buffer already has data, just append
    if (!state.writeBuf.empty()) {
        state.writeBuf.insert(state.writeBuf.end(), buf.begin(), buf.end());
        return true;
    }

    // Try direct send first
    ssize_t n = send(fd, buf.data(), buf.size(), MSG_DONTWAIT);
    if (n == static_cast<ssize_t>(buf.size())) {
        return true; // full write succeeded
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) n = 0;
        else return false;
    }

    // Buffer remaining bytes and register for write events
    state.writeBuf.insert(state.writeBuf.end(), buf.begin() + n, buf.end());
    addWriteToEventLoop(fd);
    return true;
}

} // namespace OrderMatcher
