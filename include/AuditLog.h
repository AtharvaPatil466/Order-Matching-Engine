#pragma once

// AuditLog — per-order lifecycle audit trail.
//
// A regulatory/compliance record of every order's full lifecycle, kept
// SEPARATE from the replay journal (Journal.h). The journal exists to
// deterministically rebuild book state on restart; this exists to answer
// "what happened to order X / to participant Y's flow" for surveillance,
// dispute resolution and post-trade reporting. The two have different
// retention, access and format requirements, so they are deliberately
// different files.
//
// For each order we retain:
//   * identity      — order_id, participant_id, symbol, side, type, price, qty
//   * submit time   — rdtsc (cycle counter) + wall-clock nanoseconds
//   * accept time   — rdtsc + wall (0/0 if the order was never accepted)
//   * reject reason — if the order was rejected
//   * every fill    — time (rdtsc+wall), price, qty, counterparty id + order id
//   * final status  — terminal OrderStatus + time (rdtsc+wall)
//
// DURABILITY: every lifecycle event is appended, as one JSON line, to an
// append-only file at the moment it happens (open with std::ios::app; never
// rewritten or truncated). The in-memory index is a query accelerator, not the
// source of truth — the file is. Records are queryable by order_id and by
// participant_id.
//
// THREADING: all mutating and query methods take an internal mutex. The audit
// trail is intentionally OFF the matching hot path (driven from the engine's
// post-trade drain / gateway callbacks), so a coarse lock is the right
// simplicity/safety trade-off.

#include "Types.h"
#include "Utils.h"  // OrderMatcher::Utils::rdtsc()

#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace OrderMatcher {

// Wall-clock nanoseconds since the Unix epoch. Distinct from LatencyTracker's
// nowNs() (a monotonic high_resolution_clock) — an audit trail needs a real
// calendar timestamp that survives across process restarts.
inline uint64_t auditWallNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// A paired (cycle-counter, wall-clock) timestamp. rdtsc gives sub-nanosecond
// ordering resolution on the local host; wall gives a portable calendar time.
struct AuditTime {
    uint64_t rdtsc = 0;
    uint64_t wallNs = 0;

    static AuditTime now() { return {Utils::rdtsc(), auditWallNs()}; }
    bool set() const { return rdtsc != 0 || wallNs != 0; }
};

struct AuditFill {
    AuditTime time;
    Price price = 0;
    Quantity qty = 0;
    ParticipantId counterparty = 0;
    OrderId counterpartyOrderId = 0;
};

struct AuditRecord {
    OrderId orderId = 0;
    ParticipantId participantId = 0;
    SymbolId symbolId = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;
    Quantity quantity = 0;

    AuditTime submit;
    AuditTime accept;                       // unset if never accepted
    RejectReason rejectReason = RejectReason::None;

    std::vector<AuditFill> fills;

    OrderStatus finalStatus = OrderStatus::New;
    AuditTime finalTime;                    // set once terminal

    Quantity filledQty() const {
        Quantity q = 0;
        for (const auto& f : fills) q += f.qty;
        return q;
    }
    bool isTerminal() const {
        return finalStatus == OrderStatus::Filled ||
               finalStatus == OrderStatus::Cancelled ||
               finalStatus == OrderStatus::Rejected;
    }
};

class AuditLog {
public:
    AuditLog() = default;

    // Open (append) the backing file. Returns false if it can't be opened.
    // An AuditLog with no file still records in memory (useful for tests).
    explicit AuditLog(const std::string& path) { open(path); }

    bool open(const std::string& path) {
        std::lock_guard<std::mutex> lock(mu_);
        path_ = path;
        file_.open(path, std::ios::out | std::ios::app);
        return file_.is_open();
    }

    bool isOpen() const {
        std::lock_guard<std::mutex> lock(mu_);
        return file_.is_open();
    }

    // ── Lifecycle events ──────────────────────────────────────────────────
    // Each event both updates the in-memory record and appends a JSON line to
    // the file. Order of calls per order_id is expected to be:
    //   recordSubmit -> (recordAccept | recordReject)
    //                -> recordFill* -> recordFinal

    void recordSubmit(OrderId orderId, ParticipantId participantId,
                      SymbolId symbolId, Side side, OrderType type,
                      Price price, Quantity qty) {
        std::lock_guard<std::mutex> lock(mu_);
        AuditRecord& r = records_[orderId];
        r.orderId = orderId;
        r.participantId = participantId;
        r.symbolId = symbolId;
        r.side = side;
        r.type = type;
        r.price = price;
        r.quantity = qty;
        r.submit = AuditTime::now();
        r.finalStatus = OrderStatus::New;
        byParticipant_[participantId].push_back(orderId);

        std::ostringstream extra;
        extra << ",\"symbol\":" << symbolId
              << ",\"side\":\"" << (side == Side::Buy ? 'B' : 'S') << '"'
              << ",\"otype\":" << static_cast<int>(type)
              << ",\"price\":" << price
              << ",\"qty\":" << qty;
        appendLine(orderId, participantId, "submit", r.submit, extra.str());
    }

    void recordAccept(OrderId orderId) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = records_.find(orderId);
        if (it == records_.end()) return;
        AuditRecord& r = it->second;
        r.accept = AuditTime::now();
        if (r.finalStatus == OrderStatus::New) r.finalStatus = OrderStatus::Accepted;
        appendLine(orderId, r.participantId, "accept", r.accept, "");
    }

    void recordReject(OrderId orderId, RejectReason reason) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = records_.find(orderId);
        if (it == records_.end()) return;
        AuditRecord& r = it->second;
        r.rejectReason = reason;
        r.finalStatus = OrderStatus::Rejected;
        r.finalTime = AuditTime::now();
        std::ostringstream extra;
        extra << ",\"reject\":" << static_cast<int>(reason);
        appendLine(orderId, r.participantId, "reject", r.finalTime, extra.str());
    }

    void recordFill(OrderId orderId, Price price, Quantity qty,
                    ParticipantId counterparty, OrderId counterpartyOrderId) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = records_.find(orderId);
        if (it == records_.end()) return;
        AuditRecord& r = it->second;
        AuditFill f;
        f.time = AuditTime::now();
        f.price = price;
        f.qty = qty;
        f.counterparty = counterparty;
        f.counterpartyOrderId = counterpartyOrderId;
        r.fills.push_back(f);
        // Reflect partial/full fill in the running status (finalized explicitly
        // by recordFinal, but keep the live view meaningful for queries).
        if (r.filledQty() >= r.quantity && r.quantity > 0)
            r.finalStatus = OrderStatus::Filled;
        else if (r.finalStatus != OrderStatus::Filled)
            r.finalStatus = OrderStatus::PartiallyFilled;

        std::ostringstream extra;
        extra << ",\"fillPrice\":" << price
              << ",\"fillQty\":" << qty
              << ",\"cpty\":" << counterparty
              << ",\"cptyOrder\":" << counterpartyOrderId;
        appendLine(orderId, r.participantId, "fill", f.time, extra.str());
    }

    // Mark the order terminal with its final status (Filled/Cancelled/Rejected).
    void recordFinal(OrderId orderId, OrderStatus status) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = records_.find(orderId);
        if (it == records_.end()) return;
        AuditRecord& r = it->second;
        r.finalStatus = status;
        r.finalTime = AuditTime::now();
        std::ostringstream extra;
        extra << ",\"status\":" << static_cast<int>(status)
              << ",\"filledQty\":" << r.filledQty();
        appendLine(orderId, r.participantId, "final", r.finalTime, extra.str());
    }

    // ── Queries ───────────────────────────────────────────────────────────

    // Returns a COPY so the caller can inspect without holding the lock.
    bool findByOrder(OrderId orderId, AuditRecord& out) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = records_.find(orderId);
        if (it == records_.end()) return false;
        out = it->second;
        return true;
    }

    std::vector<AuditRecord> findByParticipant(ParticipantId participantId) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<AuditRecord> out;
        auto it = byParticipant_.find(participantId);
        if (it == byParticipant_.end()) return out;
        out.reserve(it->second.size());
        for (OrderId id : it->second) {
            auto rit = records_.find(id);
            if (rit != records_.end()) out.push_back(rit->second);
        }
        return out;
    }

    size_t recordCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return records_.size();
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mu_);
        if (file_.is_open()) file_.flush();
    }

private:
    // One append-only JSON line per event. Never rewrites earlier lines.
    void appendLine(OrderId orderId, ParticipantId participantId,
                    const char* event, const AuditTime& t,
                    const std::string& extra) {
        if (!file_.is_open()) return;
        file_ << "{\"event\":\"" << event << '"'
              << ",\"order\":" << orderId
              << ",\"participant\":" << participantId
              << ",\"rdtsc\":" << t.rdtsc
              << ",\"wallNs\":" << t.wallNs
              << extra << "}\n";
        file_.flush();
    }

    mutable std::mutex mu_;
    std::string path_;
    mutable std::ofstream file_;
    std::unordered_map<OrderId, AuditRecord> records_;
    std::unordered_map<ParticipantId, std::vector<OrderId>> byParticipant_;
};

}  // namespace OrderMatcher
