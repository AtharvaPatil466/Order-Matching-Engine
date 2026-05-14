#pragma once

// SoupBinTcpSession — server-side SoupBinTCP session state machine.
//
// Composes a framer (reassembles wire bytes into SoupPackets) with
// the application-layer dispatch policy (login validation,
// heartbeat scheduling, peer-idle disconnect, payload handoff).
//
// Threading: caller-driven. The gateway feeds wire bytes via feed()
// and ticks the session clock via tick(nowMs). Heartbeats and
// disconnects are derived from those calls — no background thread,
// so the session is straightforward to test and to shut down.
//
// Application payloads (OUCH order entry, ITCH gap fill, etc.) are
// surfaced via setOnAppPayload(); the session itself is protocol-
// agnostic about the inner messages, it only enforces the SoupBinTCP
// envelope and session lifecycle. Drop in an OuchSession (or any
// other consumer) under that callback to get OUCH-over-SoupBinTCP.
//
// Default policy intervals:
//   * Server sends a heartbeat after 1s of send-side idle.
//   * Server disconnects after 15s of recv-side idle (no client
//     heartbeat or app data observed).
// Both are settable via setIntervals(). Spec-default is 1s heartbeat
// / 15s peer-timeout.

#include "SoupBinTCP.h"
#include "SoupBinTcpFramer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace OrderMatcher {

class SoupBinTcpSession {
public:
    using SendBytes      = std::function<void(std::string_view)>;
    // Called when the peer sends an UnsequencedData or SequencedData
    // packet. The session has already validated the envelope and is
    // logged in; the consumer just needs to interpret the inner
    // bytes (e.g. as an OUCH or ITCH frame).
    using OnAppPayload   = std::function<void(const uint8_t* data, size_t len, bool sequenced)>;
    // Called when the session validates a LoginRequest. Return
    // SOUP_LOGIN_REJECT_* to reject; return 0 to accept. The session
    // sends 'A' or 'J' based on the answer.
    using OnLoginRequest = std::function<char(const SoupLoginRequest&)>;

    SoupBinTcpSession(SendBytes send, std::string serverSession)
        : send_(std::move(send)), serverSession_(std::move(serverSession)) {
        framer_.setOnPacket([this](const SoupPacket& pkt) { dispatch(pkt); });
    }

    void setOnAppPayload(OnAppPayload cb)   { onAppPayload_ = std::move(cb); }
    void setOnLoginRequest(OnLoginRequest cb) { onLogin_ = std::move(cb); }

    // Configure the heartbeat and peer-idle timeout. Spec defaults
    // are 1000ms heartbeat / 15000ms peer-idle.
    void setIntervals(uint64_t heartbeatMs, uint64_t peerIdleMs) {
        heartbeatMs_ = heartbeatMs;
        peerIdleMs_  = peerIdleMs;
    }

    // Inject wire bytes from the peer. Returns false on unrecoverable
    // framing error or a protocol violation (e.g. data before login);
    // the gateway should drop the connection in that case.
    bool feed(const char* data, size_t len, uint64_t nowMs) {
        lastNowMs_ = nowMs;
        lastPeerActivityMs_ = nowMs;  // any byte = "peer is alive"
        if (!framer_.feed(data, len)) return false;
        return !sessionClosed_;
    }

    // Drive heartbeat emission and peer-idle disconnect. Call this
    // periodically from the gateway loop (every ~100ms is plenty).
    // Returns false if the session must disconnect — peer-idle
    // timeout exceeded.
    bool tick(uint64_t nowMs) {
        lastNowMs_ = nowMs;
        if (!loggedIn_) return !sessionClosed_;

        // Peer-side idle check first — closing on timeout takes
        // precedence over sending one last heartbeat.
        if (nowMs - lastPeerActivityMs_ >= peerIdleMs_) {
            return false;
        }
        // Send our heartbeat if we've been quiet for an interval.
        if (nowMs - lastOwnSendMs_ >= heartbeatMs_) {
            sendHeartbeat();
        }
        return !sessionClosed_;
    }

    // Send an application-layer message in a SequencedData (server →
    // client, gets a seqnum) packet. Increments the outbound seqnum.
    void sendSequenced(const void* payload, size_t len) {
        sendApp(SOUP_PT_SEQUENCED_DATA, payload, len);
        ++nextOutboundSeq_;
    }

    // Send in an UnsequencedData packet. No seqnum is consumed.
    // Used for messages that aren't part of the recoverable stream
    // (server status notes, debug, etc.).
    void sendUnsequenced(const void* payload, size_t len) {
        sendApp(SOUP_PT_UNSEQUENCED_DATA, payload, len);
    }

    bool loggedIn()              const { return loggedIn_; }
    bool closed()                const { return sessionClosed_; }
    uint64_t nextOutboundSeq()   const { return nextOutboundSeq_; }
    uint64_t heartbeatsSent()    const { return heartbeatsSent_; }
    uint64_t appPayloadsHandled() const { return appPayloadsHandled_; }
    uint64_t packetsDropped()    const { return packetsDropped_; }
    const std::string& session() const { return serverSession_; }

private:
    void dispatch(const SoupPacket& pkt) {
        // The framer guarantees the envelope is well-formed; this is
        // pure protocol-state-machine territory.
        switch (pkt.type) {
        case SOUP_PT_LOGIN_REQUEST:
            handleLogin(pkt);
            return;
        case SOUP_PT_LOGOUT_REQUEST:
            handleLogout();
            return;
        case SOUP_PT_CLIENT_HEARTBEAT:
            // Heartbeat already updated lastPeerActivityMs_ in feed();
            // nothing else to do.
            return;
        case SOUP_PT_UNSEQUENCED_DATA:
            handleAppData(pkt, /*sequenced=*/false);
            return;
        case SOUP_PT_SEQUENCED_DATA:
            // The server side normally doesn't *receive* sequenced
            // data — that's a server-to-client direction. Treat as
            // an app payload anyway; consumers can decide.
            handleAppData(pkt, /*sequenced=*/true);
            return;
        case SOUP_PT_DEBUG:
            // Diagnostic — accept but don't act on it. A future
            // iteration could surface this to the consumer.
            return;
        default:
            // Unknown packet type from a peer is a protocol violation.
            // Drop the packet but stay alive; the peer's idle timer
            // will catch a peer that consistently sends garbage.
            ++packetsDropped_;
            return;
        }
    }

    void handleLogin(const SoupPacket& pkt) {
        if (loggedIn_) {
            // Already logged in — protocol violation. Drop.
            ++packetsDropped_;
            return;
        }
        SoupLoginRequest req;
        if (!soupParseLoginRequest(pkt.payload, pkt.payloadLen, req)) {
            sendLoginRejected(SOUP_LOGIN_REJECT_NOT_AUTHORIZED);
            return;
        }
        char rejectCode = 0;
        if (onLogin_) rejectCode = onLogin_(req);
        if (rejectCode != 0) {
            sendLoginRejected(rejectCode);
            return;
        }
        // Honor the client's requested sequence number if provided —
        // a real implementation would replay from a persistent log
        // starting at that sequence. For this baseline session we
        // simply start the outbound seq from whatever was requested
        // (default 1 if none / unparseable / zero requested).
        if (req.requestedSeq > 0) {
            nextOutboundSeq_ = req.requestedSeq;
        } else {
            nextOutboundSeq_ = 1;
        }
        sendLoginAccepted();
        loggedIn_ = true;
    }

    void handleLogout() {
        // Spec: server acknowledges the logout by sending
        // EndOfSession and closing the connection.
        if (send_) {
            uint8_t buf[3];
            size_t n = soupWriteEndOfSession(buf);
            send_(std::string_view(reinterpret_cast<const char*>(buf), n));
            lastOwnSendMs_ = lastNowMs_;
        }
        sessionClosed_ = true;
    }

    void handleAppData(const SoupPacket& pkt, bool sequenced) {
        if (!loggedIn_) {
            // App data before login = protocol violation.
            ++packetsDropped_;
            return;
        }
        if (onAppPayload_) {
            onAppPayload_(pkt.payload, pkt.payloadLen, sequenced);
        }
        ++appPayloadsHandled_;
    }

    void sendLoginAccepted() {
        if (!send_) return;
        uint8_t buf[3 + SOUP_SIZE_LOGIN_ACCEPTED_PAYLOAD];
        size_t n = soupWriteLoginAccepted(buf, serverSession_, nextOutboundSeq_);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
        lastOwnSendMs_ = lastNowMs_;
    }

    void sendLoginRejected(char reason) {
        if (!send_) return;
        uint8_t buf[3 + SOUP_SIZE_LOGIN_REJECTED_PAYLOAD];
        size_t n = soupWriteLoginRejected(buf, reason);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
        lastOwnSendMs_ = lastNowMs_;
        // After a reject the server is expected to close the
        // connection. Mark closed so feed() returns false next call.
        sessionClosed_ = true;
    }

    void sendHeartbeat() {
        if (!send_) return;
        uint8_t buf[3];
        size_t n = soupWriteHeartbeat(buf, /*serverSide=*/true);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
        lastOwnSendMs_ = lastNowMs_;
        ++heartbeatsSent_;
    }

    void sendApp(uint8_t packetType, const void* payload, size_t len) {
        if (!send_) return;
        // Envelope is 3 bytes; we use a small stack buffer for the
        // common envelope and a heap allocation only for the
        // payload-bearing slice. Since most OUCH/ITCH messages fit
        // well under 1KB, a 1KB stack buffer is enough for the hot
        // path; oversize payloads spill to heap.
        if (len + 3 <= 1024) {
            uint8_t buf[1024];
            size_t n = soupWriteEnvelope(buf, packetType, payload, len);
            send_(std::string_view(reinterpret_cast<const char*>(buf), n));
        } else {
            std::string buf;
            buf.resize(len + 3);
            soupWriteEnvelope(reinterpret_cast<uint8_t*>(buf.data()),
                              packetType, payload, len);
            send_(std::string_view(buf));
        }
        lastOwnSendMs_ = lastNowMs_;
    }

    SendBytes        send_;
    std::string      serverSession_;
    SoupBinTcpFramer framer_;
    OnAppPayload     onAppPayload_;
    OnLoginRequest   onLogin_;

    bool             loggedIn_{false};
    bool             sessionClosed_{false};

    uint64_t         heartbeatMs_{1000};
    uint64_t         peerIdleMs_{15000};
    uint64_t         lastNowMs_{0};
    uint64_t         lastOwnSendMs_{0};
    uint64_t         lastPeerActivityMs_{0};

    uint64_t         nextOutboundSeq_{1};
    uint64_t         heartbeatsSent_{0};
    uint64_t         appPayloadsHandled_{0};
    uint64_t         packetsDropped_{0};
};

}  // namespace OrderMatcher
