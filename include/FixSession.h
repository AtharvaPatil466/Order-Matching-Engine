#pragma once

// FixSession — portable FIX-over-stream session adapter.
//
// Composition target for any event-loop-based gateway (kqueue, epoll,
// io_uring). The OS-specific gateway:
//   * accepts a TCP connection and creates a FixSession
//   * forwards each recv() chunk to session.feed(...)
//   * forwards bytes from the session's response callback to the socket
//   * disposes the session on disconnect or feed() returning false
//
// The session itself knows nothing about sockets — it owns a FixFramer that
// reassembles complete, structurally-valid, checksum-validated FIX frames
// from arbitrary fragmented input, then dispatches each to the engine via
// fixToOrderParams. Responses preserve the accepted inbound BeginString and
// are returned via the response callback.
//
// Replaces the previous (broken) IoUringGateway.h, which referenced
// undefined functions, never framed properly, and had no consumers.

#include "FIXParser.h"
#include "FixFramer.h"
#include "MatchingEngine.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OrderMatcher {

class FixSession {
public:
    using SendBytes = std::function<void(std::string_view)>;

    FixSession(MatchingEngine& engine, SendBytes send,
               size_t maxFrameSize = 64 * 1024)
        : engine_(engine), send_(std::move(send)), framer_(maxFrameSize),
          acceptedVersions_{"FIX.4.2"} {
        framer_.setOnMessage([this](std::string_view raw, const FixMessage& msg) {
            (void)raw;
            dispatch(msg);
        });
    }

    // Override the set of accepted FIX BeginString values. Default is
    // {"FIX.4.2"}; pass e.g. {"FIX.4.2", "FIX.4.4"} to accept multiple
    // versions during a transition. A message whose BeginString is not
    // in the set is rejected with UnsupportedFixVersion BEFORE any
    // engine-side processing, so the wrong-version client can never
    // accidentally place an order under the wrong field semantics.
    void setAcceptedVersions(std::unordered_set<std::string> versions) {
        acceptedVersions_ = std::move(versions);
    }
    const std::unordered_set<std::string>& acceptedVersions() const {
        return acceptedVersions_;
    }

    // Feed bytes from the wire. Returns false on unrecoverable framing
    // error — the gateway should drop the connection.
    bool feed(const char* data, size_t len) {
        bool ok = framer_.feed(data, len);
        return ok && !closeAfterSend_;
    }

    uint64_t framesEmitted()   const { return framer_.framesEmitted(); }
    uint64_t framesRejected()  const { return framer_.framesRejected(); }
    uint64_t ordersAccepted()  const { return ordersAccepted_; }
    uint64_t ordersRejected()  const { return ordersRejected_; }
    uint64_t sessionMessagesHandled() const { return sessionMessagesHandled_; }
    uint64_t expectedInboundSeqNum() const { return expectedInboundSeqNum_; }
    bool loggedOn() const { return loggedOn_; }
    bool shouldDisconnect() const { return closeAfterSend_; }

    // No default: case so adding a new RejectReason produces a -Wswitch
    // build error (CMake sets -Werror), forcing every new reason to be
    // wired through this string mapping at compile time.
    // Public so tests can assert the mapping is wired for every enum
    // value and so other layers (logging, metrics) can reuse the labels.
    static const char* rejectReason(RejectReason r) {
        switch (r) {
        case RejectReason::None:                  return "ok";
        case RejectReason::SymbolNotFound:        return "symbol";
        case RejectReason::InvalidPrice:          return "price";
        case RejectReason::InvalidQuantity:       return "qty";
        case RejectReason::OrderNotFound:         return "not found";
        case RejectReason::QueueBackpressure:     return "backpressure";
        case RejectReason::RateLimitExceeded:     return "rate limit";
        case RejectReason::EngineStopped:         return "engine stopped";
        case RejectReason::CapacityExhausted:     return "capacity";
        case RejectReason::OutOfPriceRange:       return "price range";
        case RejectReason::RiskLimitBreached:     return "risk";
        case RejectReason::FOKInsufficientLiquidity: return "FOK no liquidity";
        case RejectReason::PostOnlyWouldCross:    return "post-only";
        case RejectReason::VolatilityCircuitBreaker: return "vol breaker";
        case RejectReason::MarketHalted:          return "halted";
        case RejectReason::OrderTypeNotAllowedInState: return "type not allowed";
        case RejectReason::OutsidePriceBand:      return "outside band";
        case RejectReason::MarketClosed:          return "closed";
        case RejectReason::DuplicateOrderId:      return "duplicate id";
        case RejectReason::UnsupportedFixVersion: return "unsupported version";
        case RejectReason::MissingRequiredField:  return "missing required field";
        }
        return "rejected";  // unreachable; silences "control reaches end".
    }

private:
    void dispatch(const FixMessage& msg) {
        // Version check FIRST — wrong-version clients must not have
        // their messages dispatched into the engine, even if they
        // happen to look well-formed under the current parser. A FIX
        // 4.4 NewOrder uses overlapping but not identical tag
        // semantics; silently accepting it would cause subtle order
        // misinterpretation.
        auto begin = msg.getString(FixTag::BeginString);
        if (acceptedVersions_.find(std::string(begin)) ==
                acceptedVersions_.end()) {
            ++ordersRejected_;
            currentBeginString_ = "FIX.4.2";
            currentVersion_ = FixVersion::FIX_4_2;
            sendReject(/*orderId=*/0, RejectReason::UnsupportedFixVersion);
            return;
        }
        currentBeginString_ = std::string(begin);
        currentVersion_ = parseFixVersion(currentBeginString_);

        auto msgType = msg.getString(FixTag::MsgType);
        if (!validateInboundSequence(msg, msgType)) {
            return;
        }
        if (isSessionMessage(msgType)) {
            handleSessionMessage(msg, msgType);
            return;
        }

        // FIX 4.4 requires TransactTime (60) on NewOrderSingle (D),
        // OrderCancelRequest (F), and OrderCancelReplaceRequest (G).
        // FIX 4.2 left it conditional, so 4.2 callers can still omit it.
        if (currentVersion_ == FixVersion::FIX_4_4 &&
            (msgType == "D" || msgType == "F" || msgType == "G") &&
            !msg.hasTag(FixTag::TransactTime)) {
            ++ordersRejected_;
            // Best-effort orderId for the reject ack. Cancel/replace
            // identify the prior order via OrigClOrdID; new orders use
            // ClOrdID. Either may be absent — fall back to 0.
            OrderId rejectId = static_cast<OrderId>(
                msg.hasTag(FixTag::OrigClOrdID)
                    ? msg.getInt(FixTag::OrigClOrdID)
                    : msg.getInt(FixTag::ClOrdID));
            sendReject(rejectId, RejectReason::MissingRequiredField);
            return;
        }

        auto params = fixToOrderParams(msg);
        if (!params.valid) {
            sendReject(/*orderId=*/0, "MsgType not supported");
            return;
        }

        SubmitResult r;
        switch (params.action) {
        case FixOrderParams::Action::NewOrder:
            r = engine_.submitOrder(params.symbolId, params.orderId,
                                    params.participantId, params.side,
                                    params.price, params.qty,
                                    params.orderType,
                                    /*stopPrice=*/0, /*displayQty=*/0,
                                    params.tif);
            break;
        case FixOrderParams::Action::Cancel:
            r = engine_.submitCancel(params.symbolId, params.orderId);
            break;
        case FixOrderParams::Action::CancelReplace:
            r = engine_.submitCancelReplace(params.symbolId, params.orderId,
                                            params.newPrice, params.newQty);
            break;
        case FixOrderParams::Action::Unknown:
        default:
            sendReject(params.orderId, "Unknown action");
            return;
        }

        if (r.isAccepted()) {
            ++ordersAccepted_;
            sendAck(params.orderId, params.side,
                    params.action == FixOrderParams::Action::CancelReplace
                        ? params.newPrice : params.price,
                    params.action == FixOrderParams::Action::CancelReplace
                        ? params.newQty : params.qty);
        } else {
            ++ordersRejected_;
            sendReject(params.orderId, r.rejectReason);
        }
    }

    static bool isSessionMessage(std::string_view msgType) {
        return msgType == "A" ||  // Logon
               msgType == "0" ||  // Heartbeat
               msgType == "1" ||  // TestRequest
               msgType == "2" ||  // ResendRequest
               msgType == "4" ||  // SequenceReset
               msgType == "5";    // Logout
    }

    bool validateInboundSequence(const FixMessage& msg, std::string_view msgType) {
        if (!msg.hasTag(FixTag::MsgSeqNum)) {
            return true;
        }

        uint64_t seq = msg.getUint64(FixTag::MsgSeqNum);
        if (seq == 0 || seq == UINT64_MAX) {
            ++ordersRejected_;
            sendReject(/*orderId=*/0, "invalid seqnum");
            return false;
        }

        bool possDup = msg.getChar(FixTag::PossDupFlag) == 'Y';
        if (seq < expectedInboundSeqNum_) {
            if (possDup) {
                return false;
            }
            ++ordersRejected_;
            sendReject(/*orderId=*/0, "seqnum too low");
            return false;
        }

        if (seq > expectedInboundSeqNum_) {
            sendAdmin("2", {
                {FixTag::BeginSeqNo, std::to_string(expectedInboundSeqNum_)},
                {FixTag::EndSeqNo, std::to_string(seq - 1)},
            });
            return false;
        }

        if (msgType == "4") {
            uint64_t newSeq = msg.getUint64(FixTag::NewSeqNo);
            expectedInboundSeqNum_ = newSeq > expectedInboundSeqNum_
                                         ? newSeq
                                         : expectedInboundSeqNum_ + 1;
            return true;
        }

        ++expectedInboundSeqNum_;
        return true;
    }

    void handleSessionMessage(const FixMessage& msg, std::string_view msgType) {
        ++sessionMessagesHandled_;

        if (msgType == "A") {
            loggedOn_ = true;
            auto hb = msg.getUint64(FixTag::HeartBtInt);
            if (hb > 0 && hb <= UINT32_MAX) {
                heartbeatIntervalSec_ = static_cast<uint32_t>(hb);
            }
            sendAdmin("A", {
                {FixTag::HeartBtInt, std::to_string(heartbeatIntervalSec_)},
            });
            return;
        }

        if (msgType == "0") {
            // Peer heartbeat: no response required.
            return;
        }

        if (msgType == "1") {
            std::vector<std::pair<int, std::string>> fields;
            auto id = msg.getString(FixTag::TestReqID);
            if (!id.empty()) {
                fields.push_back({FixTag::TestReqID, std::string(id)});
            }
            sendAdmin("0", fields);
            return;
        }

        if (msgType == "2") {
            // Stateless baseline replay policy: this gateway does not retain
            // admin/app FIX messages for replay, so it answers with a Sequence
            // Reset-GapFill that advances the peer past the requested range.
            uint64_t end = msg.getUint64(FixTag::EndSeqNo);
            uint64_t newSeq = (end == 0 || end == UINT64_MAX)
                                  ? outboundSeqNum_
                                  : end + 1;
            sendAdmin("4", {
                {FixTag::GapFillFlag, "Y"},
                {FixTag::NewSeqNo, std::to_string(newSeq)},
            });
            return;
        }

        if (msgType == "5") {
            sendAdmin("5", {{FixTag::Text, "logout acknowledged"}});
            loggedOn_ = false;
            closeAfterSend_ = true;
        }
    }

    void sendAdmin(const char* msgType,
                   const std::vector<std::pair<int, std::string>>& fields = {}) {
        if (!send_) return;
        send_(buildFixMessage(msgType, fields));
    }

    std::string buildFixMessage(
        const char* msgType,
        const std::vector<std::pair<int, std::string>>& fields) {
        std::ostringstream body;
        body << "35=" << msgType << FIX_SOH;
        body << "34=" << outboundSeqNum_++ << FIX_SOH;
        for (const auto& [tag, value] : fields) {
            body << tag << "=" << value << FIX_SOH;
        }

        std::string bodyStr = body.str();
        std::ostringstream msg;
        msg << "8=" << currentBeginString_ << FIX_SOH;
        msg << "9=" << bodyStr.size() << FIX_SOH;
        msg << bodyStr;

        std::string raw = msg.str();
        uint32_t sum = 0;
        for (char c : raw) sum += static_cast<uint8_t>(c);

        char cs[4];
        snprintf(cs, sizeof(cs), "%03d", static_cast<int>(sum % 256));
        raw += "10=";
        raw += cs;
        raw += FIX_SOH;
        return raw;
    }

    void sendAck(OrderId id, Side side, Price price, Quantity qty) {
        if (!send_) return;
        auto raw = FixSerializer::buildExecutionReport(
            id, /*execId=*/id, /*execType=*/'0', /*ordStatus=*/'0',
            side, price, qty, /*cumQty=*/0, /*leavesQty=*/qty,
            /*lastPx=*/0, /*lastQty=*/0, /*text=*/"",
            currentBeginString_, outboundSeqNum_++,
            currentVersion_);
        send_(raw);
    }

    // String-only reject path. Used when the parser fails before we have a
    // structured RejectReason (e.g. unrecognized MsgType). Emits no
    // OrdRejReason (103) — the spec allows that, and a numeric code we'd
    // have to invent would be misleading.
    void sendReject(OrderId id, const char* reason) {
        if (!send_) return;
        auto raw = FixSerializer::buildExecutionReport(
            id, /*execId=*/id, /*execType=*/'8', /*ordStatus=*/'8',
            Side::Buy, /*price=*/0, /*orderQty=*/0,
            /*cumQty=*/0, /*leavesQty=*/0, /*lastPx=*/0, /*lastQty=*/0,
            reason ? reason : "", currentBeginString_, outboundSeqNum_++,
            currentVersion_);
        send_(raw);
    }

    // Structured-reject path. Use this whenever a real RejectReason is
    // available so the wire response carries OrdRejReason (103) — required
    // for any 4.4 counterparty that actually inspects reject codes.
    void sendReject(OrderId id, RejectReason r) {
        if (!send_) return;
        auto raw = FixSerializer::buildExecutionReport(
            id, /*execId=*/id, /*execType=*/'8', /*ordStatus=*/'8',
            Side::Buy, /*price=*/0, /*orderQty=*/0,
            /*cumQty=*/0, /*leavesQty=*/0, /*lastPx=*/0, /*lastQty=*/0,
            rejectReason(r), currentBeginString_, outboundSeqNum_++,
            currentVersion_, ordRejReasonCode(r));
        send_(raw);
    }

    MatchingEngine&                  engine_;
    SendBytes                        send_;
    FixFramer                        framer_;
    std::unordered_set<std::string>  acceptedVersions_;
    FixVersion                       currentVersion_{FixVersion::FIX_4_2};
    std::string                      currentBeginString_{"FIX.4.2"};
    uint64_t                         expectedInboundSeqNum_{1};
    uint32_t                         heartbeatIntervalSec_{30};
    uint64_t                         outboundSeqNum_{1};
    uint64_t                         ordersAccepted_{0};
    uint64_t                         ordersRejected_{0};
    uint64_t                         sessionMessagesHandled_{0};
    bool                             loggedOn_{false};
    bool                             closeAfterSend_{false};
};

}  // namespace OrderMatcher
