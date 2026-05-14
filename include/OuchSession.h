#pragma once

// OuchSession — portable OUCH-over-stream session adapter.
//
// Parallel to FixSession: same composition surface (feed bytes in,
// response bytes flow out through a SendBytes callback), different
// codec underneath. The OS-specific gateway loop is identical; only
// the codec module changes.
//
// Framing model: OUCH 4.2 is fixed-width per message type. The session
// keeps a small accumulation buffer, reads the leading type byte to
// determine the frame length, waits for the rest of the bytes to
// arrive, then decodes and dispatches. This handles arbitrary
// fragmentation off the wire (one-byte-at-a-time still works).
//
// Real OUCH deployments wrap OUCH frames in SoupBinTCP, which adds a
// 2-byte length prefix and a 1-byte packet type. This implementation
// frames directly off the OUCH message type — the SoupBinTCP layer can
// be added on top later without disturbing the codec.

#include "EventListener.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "OuchProtocol.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OrderMatcher {

// OuchSession derives from EventListener so it can be installed on an
// OrderBook to observe trades and emit OUCH 'E' (OrderExecuted)
// messages for the orders the session owns. The base class has no-op
// defaults for onOrderUpdate / onMarketData — we only consume onTrade.
class OuchSession : public EventListener {
public:
    using SendBytes = std::function<void(std::string_view)>;

    OuchSession(MatchingEngine& engine, SendBytes send,
                size_t maxAccumBytes = 64 * 1024)
        : engine_(engine), send_(std::move(send)),
          maxAccumBytes_(maxAccumBytes) {
        buf_.reserve(256);
    }

    // Override the clock used to stamp outbound messages. Without this
    // each response carries std::chrono::steady_clock::now() in ns; in
    // deterministic tests, inject a counter for byte-exact assertions.
    using ClockFn = std::function<uint64_t()>;
    void setClock(ClockFn clock) { clock_ = std::move(clock); }

    // Feed wire bytes. Returns false on unrecoverable error — the
    // gateway should drop the connection. Recoverable rejects (bad
    // single message) emit an 'J' response and continue.
    bool feed(const char* data, size_t len) {
        if (len == 0) return true;
        if (buf_.size() + len > maxAccumBytes_) {
            // Pathological accumulation: the peer is sending bytes that
            // never resolve to a complete frame. Drop the connection.
            return false;
        }
        buf_.insert(buf_.end(),
                    reinterpret_cast<const uint8_t*>(data),
                    reinterpret_cast<const uint8_t*>(data) + len);

        // Drain as many whole frames as the buffer holds.
        size_t pos = 0;
        while (pos < buf_.size()) {
            uint8_t type = buf_[pos];
            size_t need = ouchInboundFrameSize(type);
            if (need == 0) {
                // Unrecognized message type at a frame boundary —
                // unrecoverable. A real OUCH session would NAK and
                // disconnect; we report failure and let the gateway
                // close the socket.
                return false;
            }
            if (buf_.size() - pos < need) {
                // Partial frame; wait for more bytes.
                break;
            }
            dispatch(buf_.data() + pos, need);
            pos += need;
            ++framesProcessed_;
        }
        // Compact the buffer to keep memory bounded on long sessions.
        if (pos > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + pos);
        }
        return true;
    }

    uint64_t framesProcessed() const { return framesProcessed_; }
    uint64_t ordersAccepted()  const { return ordersAccepted_; }
    uint64_t ordersRejected()  const { return ordersRejected_; }
    uint64_t cancelsAccepted() const { return cancelsAccepted_; }
    uint64_t cancelsRejected() const { return cancelsRejected_; }
    uint64_t fillsEmitted()    const { return fillsEmitted_; }
    uint64_t replacesAccepted() const { return replacesAccepted_; }
    uint64_t replacesRejected() const { return replacesRejected_; }
    size_t   knownOrderCount() const { return tokenToOrderId_.size(); }

    // Install the session as the trade EventListener on the given
    // symbol's order book so fills for orders this session owns can
    // be delivered as OUCH 'E' (OrderExecuted) messages.
    //
    // Limitation: OrderBook supports only one listener slot. Calling
    // this overwrites any prior listener (structured-event sink,
    // other sessions, etc.). Production wiring needs a fan-out
    // adapter that multiplexes onTrade to every interested
    // subscriber; that's deferred to the gateway-level wiring.
    //
    // Idempotent — repeated calls with the same symbol are a no-op
    // beyond confirming the listener is installed.
    bool attachFillListener(SymbolId sym) {
        auto* book = engine_.getOrderBook(sym);
        if (!book) return false;
        book->setEventListener(this);
        return true;
    }

    // EventListener overrides. Only onTrade is interesting for OUCH —
    // onOrderUpdate / onMarketData stay as the base-class no-ops since
    // OUCH 'A'/'C'/'J' are already emitted at the dispatch site, not
    // via the listener path.
    void onTrade(const Trade& t) override {
        // Look up by engine OrderId via the reverse map: tokens may
        // have changed identity (Order Replace), so token != orderId
        // is the general case. emitExecutedForEngineId resolves the
        // currently-active token before encoding.
        emitExecutedForEngineId(t.buyOrderId, t);
        emitExecutedForEngineId(t.sellOrderId, t);
    }

private:
    void dispatch(const uint8_t* p, size_t len) {
        switch (p[0]) {
        case OUCH_MT_ENTER_ORDER:   handleEnterOrder(p, len); return;
        case OUCH_MT_CANCEL_ORDER:  handleCancelOrder(p, len); return;
        case OUCH_MT_REPLACE_ORDER: handleReplaceOrder(p, len); return;
        default: return;  // ouchInboundFrameSize would have already
                          // rejected this; defense in depth.
        }
    }

    void handleEnterOrder(const uint8_t* p, size_t len) {
        OuchEnterOrder o;
        if (!decodeEnterOrder(p, len, o)) {
            // The frame was the right length but had unparseable
            // content (non-numeric token, bad side char, etc.). Reject
            // generically — we have no orderToken to echo because
            // parsing of the token itself may have failed.
            ++ordersRejected_;
            sendReject(/*token=*/0, OUCH_REJECT_OTHER);
            return;
        }

        auto tifMap = ouchDecodeTif(o.timeInForce);

        // The OUCH OrderToken is the client-side ID. The engine assigns
        // its own internal OrderId — we use the token itself if it's
        // unused, otherwise we'd need a separate ID allocator. For this
        // baseline we map 1:1: orderToken IS the engine OrderId, so the
        // exchange reference number returned in 'A' equals the token.
        OrderId engineId = static_cast<OrderId>(o.orderToken);

        // Pre-register BOTH directions of the token↔engineId mapping
        // before submitOrder so that any immediate fills (an
        // aggressive Limit / IOC that matches resting liquidity on
        // submit) — which fire onTrade synchronously inside
        // submitOrder — can find the entry and emit 'E'. Without
        // this, an order that fully filled on submit would silently
        // swallow its execution report. Roll both back on reject.
        tokenToOrderId_[o.orderToken] =
            TokenEntry{engineId, o.stock, o.side, o.shares, o.price};
        engineIdToToken_[engineId] = o.orderToken;
        firmOf_[engineId] = o.firm;

        SubmitResult r = engine_.submitOrder(
            o.stock, engineId, o.firm, o.side, o.price, o.shares,
            tifMap.orderType, /*stopPrice=*/0, /*displayQty=*/0,
            tifMap.tif);

        if (r.isAccepted()) {
            ++ordersAccepted_;
            sendAccepted(o, engineId);
        } else {
            // Reject — undo the pre-registration. Erase by key is safe
            // even if a transient fill somehow touched the entry,
            // because a reject means no fills can have happened.
            tokenToOrderId_.erase(o.orderToken);
            engineIdToToken_.erase(engineId);
            firmOf_.erase(engineId);
            ++ordersRejected_;
            sendReject(o.orderToken, ouchRejectCode(r.rejectReason));
        }
    }

    void handleReplaceOrder(const uint8_t* p, size_t len) {
        OuchReplaceOrder r;
        if (!decodeReplaceOrder(p, len, r)) {
            ++replacesRejected_;
            sendReject(/*token=*/0, OUCH_REJECT_OTHER);
            return;
        }
        auto it = tokenToOrderId_.find(r.existingOrderToken);
        if (it == tokenToOrderId_.end()) {
            // Replacing an unknown order — reject on the NEW token
            // since the client expects a response keyed to its
            // re-request, not the missing original.
            ++replacesRejected_;
            sendReject(r.newOrderToken, OUCH_REJECT_OTHER);
            return;
        }
        OrderId engineId = it->second.orderId;
        SymbolId symbol  = it->second.symbol;
        Side side        = it->second.side;
        ParticipantId firm = firmOf_.count(engineId) ? firmOf_[engineId] : 0;

        // submitCancelReplace keeps the SAME engine orderId; only the
        // price/qty change. The OUCH token, however, changes — that's
        // the whole point of replace from the client's perspective
        // (it gets a new identifier for the new order). So we rewrite
        // the maps: drop the old token, point the new token at the
        // same engineId, update engineId→token to the new token.
        SubmitResult res = engine_.submitCancelReplace(
            symbol, engineId, r.price, r.shares);
        if (!res.isAccepted()) {
            ++replacesRejected_;
            sendReject(r.newOrderToken, ouchRejectCode(res.rejectReason));
            return;
        }

        tokenToOrderId_.erase(r.existingOrderToken);
        tokenToOrderId_[r.newOrderToken] =
            TokenEntry{engineId, symbol, side, r.shares, r.price};
        engineIdToToken_[engineId] = r.newOrderToken;

        ++replacesAccepted_;
        sendReplaced(r, side, symbol, firm, engineId);
    }

    void handleCancelOrder(const uint8_t* p, size_t len) {
        OuchCancelOrder c;
        if (!decodeCancelOrder(p, len, c)) {
            ++cancelsRejected_;
            sendReject(/*token=*/0, OUCH_REJECT_OTHER);
            return;
        }
        auto it = tokenToOrderId_.find(c.orderToken);
        if (it == tokenToOrderId_.end()) {
            // No record of this token — the order may have already
            // filled or never existed. OUCH semantics say to silently
            // do nothing here ('X' has no negative ack); a real venue
            // would log this for surveillance. We mirror that policy.
            ++cancelsRejected_;
            return;
        }
        SubmitResult r = engine_.submitCancel(it->second.symbol, it->second.orderId);
        if (r.isAccepted()) {
            ++cancelsAccepted_;
            // Canceled-shares: the leaves quantity at cancel time. The
            // session tracks this precisely now — onTrade decrements
            // `shares` on every fill, so what's left here IS the
            // accurate post-fill remainder.
            sendCanceled(c.orderToken, it->second.shares);
            OrderId engineId = it->second.orderId;
            tokenToOrderId_.erase(it);
            engineIdToToken_.erase(engineId);
            firmOf_.erase(engineId);
        } else {
            ++cancelsRejected_;
            // Cancel-of-unknown isn't a wire-visible event in OUCH;
            // intentionally silent.
        }
    }

    void emitExecutedForEngineId(OrderId engineId, const Trade& t) {
        auto rev = engineIdToToken_.find(engineId);
        if (rev == engineIdToToken_.end()) return;
        uint64_t token = rev->second;
        auto it = tokenToOrderId_.find(token);
        if (it == tokenToOrderId_.end()) return;

        // Decrement leaves. If the trade quantity somehow exceeds our
        // recorded leaves (shouldn't happen — the engine wouldn't
        // match more than the remaining), saturate to 0 rather than
        // underflowing.
        Quantity prevLeaves = it->second.shares;
        Quantity filled = (t.quantity > prevLeaves) ? prevLeaves : t.quantity;
        it->second.shares = prevLeaves - filled;

        if (send_) {
            uint8_t buf[OUCH_SIZE_ORDER_EXECUTED];
            size_t n = encodeOrderExecuted(buf, now(), token,
                                           filled, t.price, t.tradeId);
            send_(std::string_view(reinterpret_cast<const char*>(buf), n));
            ++fillsEmitted_;
        }

        // Fully filled — drop from both maps so a subsequent cancel
        // correctly behaves as cancel-of-unknown.
        if (it->second.shares == 0) {
            tokenToOrderId_.erase(it);
            engineIdToToken_.erase(rev);
            firmOf_.erase(engineId);
        }
    }

    uint64_t now() const {
        if (clock_) return clock_();
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    void sendAccepted(const OuchEnterOrder& o, OrderId engineId) {
        if (!send_) return;
        uint8_t buf[OUCH_SIZE_ORDER_ACCEPTED];
        size_t n = encodeOrderAccepted(
            buf, now(), o.orderToken, o.side, o.shares, o.stock,
            o.price, o.timeInForce, o.firm,
            static_cast<uint64_t>(engineId), o.display);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
    }

    void sendReject(uint64_t orderToken, char reasonCode) {
        if (!send_) return;
        uint8_t buf[OUCH_SIZE_ORDER_REJECTED];
        size_t n = encodeOrderRejected(buf, now(), orderToken, reasonCode);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
    }

    void sendCanceled(uint64_t orderToken, Quantity canceledShares) {
        if (!send_) return;
        uint8_t buf[OUCH_SIZE_ORDER_CANCELED];
        size_t n = encodeOrderCanceled(buf, now(), orderToken, canceledShares);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
    }

    void sendReplaced(const OuchReplaceOrder& r, Side side, SymbolId symbol,
                      ParticipantId firm, OrderId engineId) {
        if (!send_) return;
        uint8_t buf[OUCH_SIZE_ORDER_REPLACED];
        size_t n = encodeOrderReplaced(
            buf, now(), r.newOrderToken, side, r.shares, symbol,
            r.price, r.timeInForce, firm,
            static_cast<uint64_t>(engineId), r.minQuantity,
            r.existingOrderToken, r.display);
        send_(std::string_view(reinterpret_cast<const char*>(buf), n));
    }

    struct TokenEntry {
        OrderId       orderId{0};
        SymbolId      symbol{0};
        Side          side{Side::Buy};
        Quantity      shares{0};
        Price         price{0};
    };

    MatchingEngine&                              engine_;
    SendBytes                                    send_;
    ClockFn                                      clock_;
    size_t                                       maxAccumBytes_;
    std::vector<uint8_t>                         buf_;
    std::unordered_map<uint64_t, TokenEntry>     tokenToOrderId_;
    // Reverse map: engine OrderId → active OUCH token. Built and
    // maintained alongside tokenToOrderId_ so the trade-emission
    // path (which receives engine ids) can resolve the *currently
    // active* token even after an Order Replace has renamed it.
    std::unordered_map<OrderId, uint64_t>        engineIdToToken_;
    // Firm/participant for each engine order — needed by the
    // Replace ack which must echo the original Firm. Tracked here
    // so OrderUpdate (which doesn't carry firm) doesn't have to.
    std::unordered_map<OrderId, ParticipantId>   firmOf_;
    uint64_t                                     framesProcessed_{0};
    uint64_t                                     ordersAccepted_{0};
    uint64_t                                     ordersRejected_{0};
    uint64_t                                     cancelsAccepted_{0};
    uint64_t                                     cancelsRejected_{0};
    uint64_t                                     fillsEmitted_{0};
    uint64_t                                     replacesAccepted_{0};
    uint64_t                                     replacesRejected_{0};
};

}  // namespace OrderMatcher
