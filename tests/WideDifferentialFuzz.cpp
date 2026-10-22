// WideDifferentialFuzz — the widened differential fuzzer (roadmap 1.3-C1,
// 1.5 "Done"; user decision 2 of 2026-10-05: a NON-BLOCKING CI lane until
// 1.6-F5 and an event-order spec land, then blocking).
//
// Recovered from the audit's W/gtdiff/wide.cpp, which found GTDIFF-1/2. Against
// a price-time reference it drives three symbols with: STP (participants not
// partitioned; --stp-modes gives owners 2/3/4 CancelResting/CancelBoth/
// CancelIncoming), PostOnly, modify, cancelReplace (newQty is the new TOTAL,
// 1.5-E1), icebergs (--ice; display >= ceil(qty / kMaxIcebergSlices), 1.5-E2),
// kill switch (--kill), a 19-tick grid, occasional huge quantities, and an
// execution-report check (OrderUpdate.filledQty == that order's trades). Each
// step also compares ladders, FIFO queues, per-order leaves and cumQty, and
// validateIntegrity.
//
// Exits 1 on any divergence. Known classes at the time of writing: FOK + STP
// partial fills (GTDIFF-2, 1.6-F5; --fok-engine --no-fokpartial suppress them)
// and the maker's Filled update dispatched before its trade (GTDIFF-7).
// Not built into ctest: CI runs it in its own continue-on-error job.
#include "MatchingEngine.h"
#include "OrderBook.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>
using namespace OrderMatcher;

struct Tr { OrderId b, s; Price px; Quantity q; bool operator==(const Tr& o) const { return b==o.b&&s==o.s&&px==o.px&&q==o.q; } };
enum class K : uint8_t { Add, Cancel, Modify, Replace, Kill };
struct Op { K k; SymbolId sym; OrderId id; ParticipantId owner; Side side; OrderType type; Price px; Quantity qty; ParticipantId req; Quantity disp = 0; };

struct Cfg { bool ice = false, stpModes = false, kill = false, noModify = false, fokEngine = false, fokPartial = true, execSkipMod = false; bool stp = true, post = true, repl = true, exec = true, wide = true, huge = true; };
Cfg cfg;
// ---------------- reference ----------------
// qty is the leaves; cum what executed (1.5-E1: a replace's quantity is the
// new total, cum included).
struct RO { OrderId id; ParticipantId owner; Quantity qty; OrderType type; Quantity vis; Quantity disp; Quantity cum; };
static RO mk(OrderId id, ParticipantId o, Quantity q, OrderType t, Quantity d, Quantity cum) { Quantity v = t == OrderType::Iceberg ? std::min(q, d) : q; return {id, o, q, t, v, d, cum}; }
static Quantity minDisplay(Quantity total) { return total / OrderBook::kMaxIcebergSlices + (total % OrderBook::kMaxIcebergSlices != 0); }
struct Ref {
  std::map<Price, std::vector<RO>> bids, asks;
  static bool crosses(Side s, Price lim, Price rp) { return s == Side::Buy ? lim >= rp : lim <= rp; }
  std::map<Price, std::vector<RO>>& opp(Side s) { return s == Side::Buy ? asks : bids; }
  std::map<Price, std::vector<RO>>& own(Side s) { return s == Side::Buy ? bids : asks; }
  RO* find(OrderId id, Side* sd = nullptr, Price* px = nullptr) {
    for (int k = 0; k < 2; k++) { auto& b = k ? asks : bids;
      for (auto& [p, q] : b) for (auto& o : q) if (o.id == id) { if (sd) *sd = k ? Side::Sell : Side::Buy; if (px) *px = p; return &o; } }
    return nullptr;
  }
  Price best(Side s) { auto& b = own(s); if (b.empty()) return 0; return s == Side::Buy ? std::prev(b.end())->first : b.begin()->first; }
  // quantity an aggressor could take before liquidity ends or STP stops the sweep
  Quantity reachable(Side side, Price lim, bool mkt, ParticipantId owner) {
    auto& o = opp(side); Quantity t = 0;
    std::vector<Price> ps; for (auto& [p, q] : o) ps.push_back(p);
    if (side == Side::Sell) std::reverse(ps.begin(), ps.end());
    for (Price p : ps) { if (!mkt && !crosses(side, lim, p)) break;
      for (auto& r : o[p]) { if (r.owner == owner) return t; t += r.qty; } }
    return t;
  }
  // returns remaining; sets stp if the sweep stopped on self-trade
  Quantity sweep(OrderId id, ParticipantId owner, Side side, Price lim, Quantity qty, bool mkt, std::vector<Tr>& out, bool& stp) {
    auto& o = opp(side); stp = false;
    while (qty > 0 && !o.empty()) {
      auto lvl = side == Side::Buy ? o.begin() : std::prev(o.end());
      if (!mkt && !crosses(side, lim, lvl->first)) break;
      auto& q = lvl->second;
      while (qty > 0 && !q.empty()) {
        RO& m = q.front();
        if (m.owner == owner) {
          int mode = cfg.stpModes ? (int)owner : 1;
          if (mode == 2) { q.erase(q.begin()); continue; }
          if (mode == 3) { q.erase(q.begin()); if (q.empty()) o.erase(lvl); }
          stp = true; return qty; }
        Quantity t = std::min(qty, m.vis);
        out.push_back(side == Side::Buy ? Tr{id, m.id, lvl->first, t} : Tr{m.id, id, lvl->first, t});
        qty -= t; m.qty -= t; m.vis -= t; m.cum += t;
        if (m.qty == 0) q.erase(q.begin());
        else if (m.vis == 0) { RO r2 = m; r2.vis = std::min(r2.disp, r2.qty); q.erase(q.begin()); q.push_back(r2); }
      }
      if (q.empty()) o.erase(lvl);
    }
    return qty;
  }
  // 0 accepted, 1 FOK reject, 2 dup, 3 postonly reject, 4 display too small
  int add(const Op& op, std::vector<Tr>& out) {
    if (op.type == OrderType::Iceberg && op.disp < minDisplay(op.qty)) return 4;
    if (find(op.id)) return 2;
    bool mkt = op.type == OrderType::Market;
    if (op.type == OrderType::PostOnly) {
      Price ob = best(op.side == Side::Buy ? Side::Sell : Side::Buy);
      if (ob != 0 && crosses(op.side, op.px, ob)) return 3;
    }
    if (op.type == OrderType::FOK && reachable(op.side, op.px, false, cfg.fokEngine ? 0 : op.owner) < op.qty) return 1;
    bool stp; Quantity rem = sweep(op.id, op.owner, op.side, op.px, op.qty, mkt, out, stp);
    if (rem > 0 && !stp && (op.type == OrderType::Limit || op.type == OrderType::PostOnly || op.type == OrderType::Iceberg))
      own(op.side)[op.px].push_back(mk(op.id, op.owner, rem, op.type, op.disp, op.qty - rem));
    return 0;
  }
  bool erase(OrderId id) {
    for (int k = 0; k < 2; k++) { auto& b = k ? asks : bids;
      for (auto l = b.begin(); l != b.end(); ++l) for (auto o = l->second.begin(); o != l->second.end(); ++o)
        if (o->id == id) { l->second.erase(o); if (l->second.empty()) b.erase(l); return true; } }
    return false;
  }
  void killAll(ParticipantId p) { for (auto* b : {&bids, &asks}) { for (auto l = b->begin(); l != b->end();) { auto& q = l->second; q.erase(std::remove_if(q.begin(), q.end(), [&](const RO& o) { return o.owner == p; }), q.end()); l = q.empty() ? b->erase(l) : std::next(l); } } }
  bool cancel(OrderId id, ParticipantId req) { RO* o = find(id); if (!o || (req != kAnyParticipant && o->owner != req)) return false; return erase(id); }
  bool modify(OrderId id, Quantity nq, ParticipantId req) {
    RO* o = find(id); if (!o || (req != kAnyParticipant && o->owner != req)) return false;
    if (nq == 0 || nq >= o->qty) return false;
    o->qty = nq; o->vis = std::min(o->vis, nq); return true;
  }
  bool replace(const Op& op, std::vector<Tr>& out) {
    Side sd; Price px; RO* o = find(op.id, &sd, &px);
    if (!o || (op.req != kAnyParticipant && o->owner != op.req) || op.qty == 0 || op.px <= 0) return false;
    RO copy = *o;
    if (op.qty <= copy.cum) return false;  // the total must exceed what executed
    if (copy.type == OrderType::Iceberg && copy.disp < minDisplay(op.qty)) return false;
    if (copy.type == OrderType::PostOnly) { Price ob = best(sd == Side::Buy ? Side::Sell : Side::Buy); if (ob != 0 && crosses(sd, op.px, ob)) return false; }
    const Quantity leaves = op.qty - copy.cum;
    if (op.px != px) {
      erase(op.id); bool stp;
      Quantity rem = sweep(op.id, copy.owner, sd, op.px, leaves, false, out, stp);
      if (rem > 0 && !stp) own(sd)[op.px].push_back(mk(op.id, copy.owner, rem, copy.type, copy.disp, copy.cum + leaves - rem));
      return true;
    }
    if (leaves < copy.qty) { o->qty = leaves; o->vis = std::min(o->vis, leaves); return true; }
    erase(op.id); own(sd)[px].push_back(mk(op.id, copy.owner, leaves, copy.type, copy.disp, copy.cum)); return true;
  }
};

// ---------------- engine tap ----------------
struct Tap : EventListener {
  std::vector<Tr> trades; std::map<std::pair<int, Price>, std::vector<OrderId>> queues;
  std::map<OrderId, Quantity> cum; std::string execErr; struct Rep { OrderId id; Quantity f; int st; }; std::vector<Rep> reports;
  void checkReports() { for (auto& r : reports) if (execErr.empty() && r.f != cum[r.id]) { char b[160]; std::snprintf(b, sizeof b, "exec report id=%llu status=%d filledQty=%llu but trades sum=%llu", (unsigned long long)r.id, r.st, (unsigned long long)r.f, (unsigned long long)cum[r.id]); execErr = b; } reports.clear(); }
  void onTrade(const Trade& t) override { trades.push_back({t.buyOrderId, t.sellOrderId, t.price, t.quantity}); cum[t.buyOrderId] += t.quantity; cum[t.sellOrderId] += t.quantity; }
  void onOrderUpdate(const OrderUpdate& u) override {
    if (u.status == OrderStatus::Rejected) return;
    if (u.status == OrderStatus::Filled || u.status == OrderStatus::PartiallyFilled || u.status == OrderStatus::Cancelled || u.status == OrderStatus::CancelledBySTP) {
      reports.push_back({u.orderId, u.filledQty, (int)u.status});
    }
    if (isTerminalStatus(u.status)) erase(u.orderId);
  }
  void onBookVisible(const BookVisibleUpdate& u) override {
    int s = u.side == Side::Buy ? 0 : 1;
    if (u.action == BookVisibleUpdate::Action::Rest) { erase(u.orderId); queues[{s, u.price}].push_back(u.orderId); }
    else if (u.action == BookVisibleUpdate::Action::Remove) erase(u.orderId);
  }
  void erase(OrderId id) { for (auto it = queues.begin(); it != queues.end();) { auto& q = it->second; q.erase(std::remove(q.begin(), q.end(), id), q.end()); it = q.empty() ? queues.erase(it) : std::next(it); } }
};

static std::string fmt(const Op& o) {
  char b[160]; const char* t = o.type == OrderType::Limit ? "L" : o.type == OrderType::Market ? "M" : o.type == OrderType::IOC ? "I" : o.type == OrderType::FOK ? "F" : o.type == OrderType::Iceberg ? "G" : "P";
  std::string rq = o.req == kAnyParticipant ? "*" : std::to_string(o.req);
  switch (o.k) {
    case K::Add: std::snprintf(b, sizeof b, "A %u %llu %llu %c %s %lld %llu", o.sym, (unsigned long long)o.id, (unsigned long long)o.owner, o.side == Side::Buy ? 'B' : 'S', t, (long long)o.px, (unsigned long long)o.qty); if (o.disp) { std::string x = b; x += " d" + std::to_string(o.disp); std::snprintf(b, sizeof b, "%s", x.c_str()); } break;
    case K::Cancel: std::snprintf(b, sizeof b, "C %u %llu %s", o.sym, (unsigned long long)o.id, rq.c_str()); break;
    case K::Modify: std::snprintf(b, sizeof b, "M %u %llu %s %llu", o.sym, (unsigned long long)o.id, rq.c_str(), (unsigned long long)o.qty); break;
    case K::Kill: std::snprintf(b, sizeof b, "K %llu", (unsigned long long)o.owner); break;
    case K::Replace: std::snprintf(b, sizeof b, "R %u %llu %s %lld %llu", o.sym, (unsigned long long)o.id, rq.c_str(), (long long)o.px, (unsigned long long)o.qty); break;
  }
  return b;
}

constexpr SymbolId kSyms[] = {1, 2, 3}; constexpr size_t NS = 3;


struct Harness {
  std::set<OrderId> modified[NS];
  MatchingEngine eng; Tap taps[NS]; Ref ref[NS];
  Harness() { for (size_t i = 0; i < NS; i++) { eng.addSymbol(kSyms[i]); auto* b = eng.getOrderBook(kSyms[i]); b->setCircuitBreakerThreshold(1e12); b->setEventListener(&taps[i]); if (cfg.stpModes) { b->setSTPMode(2, STPMode::CancelResting); b->setSTPMode(3, STPMode::CancelBoth); b->setSTPMode(4, STPMode::CancelIncoming); } } eng.start(); }
  ~Harness() { eng.stop(); }
  std::string step(const Op& op) {
    size_t s = op.sym - 1; for (auto& t : taps) { t.trades.clear(); }
    std::vector<Tr> rt;
    if (op.k == K::Add) {
      Ref& R = ref[s]; bool live = R.find(op.id) != nullptr; if (!live) { taps[s].cum[op.id] = 0; modified[s].erase(op.id); }
      int ro = R.add(op, rt);
      auto res = eng.submitOrder(op.sym, op.id, op.owner, op.side, op.px, op.qty, op.type, 0, op.disp);
      int eo = res.isAccepted() ? 0 : res.rejectReason == RejectReason::FOKInsufficientLiquidity ? 1 : res.rejectReason == RejectReason::DuplicateOrderId ? 2 : res.rejectReason == RejectReason::PostOnlyWouldCross ? 3 : res.rejectReason == RejectReason::InvalidDisplayQty ? 4 : 100 + (int)res.rejectReason;
      if (eo != ro) return std::string(taps[s].trades.empty() ? "accept/reject(no trades)" : "accept/reject(WITH TRADES)") + ": engine=" + std::to_string(eo) + " ref=" + std::to_string(ro);
    } else if (op.k == K::Kill) { for (auto& r : ref) r.killAll(op.owner); eng.killSwitch(op.owner); }
    else if (op.k == K::Cancel) { ref[s].cancel(op.id, op.req); eng.submitCancel(op.sym, op.id, op.req); }
    else if (op.k == K::Modify) { bool r = ref[s].modify(op.id, op.qty, op.req); bool e = eng.modifyOrder(op.sym, op.id, op.qty, op.req); if (r != e) return "modify result engine=" + std::to_string(e) + " ref=" + std::to_string(r); }
    else { bool r = ref[s].replace(op, rt); bool e = eng.cancelReplace(op.sym, op.id, op.px, op.qty, op.req); if (r != e) return "replace result engine=" + std::to_string(e) + " ref=" + std::to_string(r); }
    if (cfg.fokPartial && op.k == K::Add && op.type == OrderType::FOK && !taps[s].trades.empty()) { Quantity f = 0; for (auto& t : taps[s].trades) f += t.q; if (f != op.qty) return "FOK PARTIAL FILL filled=" + std::to_string(f) + " of " + std::to_string(op.qty); }
    if (op.k == K::Modify || op.k == K::Replace) modified[s].insert(op.id);
    if (taps[s].trades != rt) { std::string a, b; for (auto& t : taps[s].trades) a += "(" + std::to_string(t.b) + "/" + std::to_string(t.s) + "@" + std::to_string(t.px) + "x" + std::to_string(t.q) + ")"; for (auto& t : rt) b += "(" + std::to_string(t.b) + "/" + std::to_string(t.s) + "@" + std::to_string(t.px) + "x" + std::to_string(t.q) + ")"; return "trades engine=" + a + " ref=" + b; }
    for (size_t i = 0; i < NS; i++) if (i != s && !taps[i].trades.empty()) return "trade leak";
    for (size_t i = 0; i < NS; i++) { std::string d = cmp(i); if (!d.empty()) return d; }
    if (cfg.exec) for (size_t ti = 0; ti < NS; ti++) { auto& t = taps[ti]; if (cfg.execSkipMod) { std::vector<Tap::Rep> keep; for (auto& r : t.reports) if (!modified[ti].count(r.id)) keep.push_back(r); t.reports = keep; } t.checkReports(); if (!t.execErr.empty()) return t.execErr; }
    return "";
  }
  std::string cmp(size_t i) {
    auto* book = eng.getOrderBook(kSyms[i]); Ref& R = ref[i];
    auto snap = eng.getSnapshot(kSyms[i], MarketDataSnapshot::MAX_DEPTH);
    for (Side sd : {Side::Buy, Side::Sell}) {
      auto& L = R.own(sd); std::vector<std::pair<Price, std::vector<RO>>> want(L.begin(), L.end());
      if (sd == Side::Buy) std::reverse(want.begin(), want.end());
      if (want.size() > MarketDataSnapshot::MAX_DEPTH) { std::fprintf(stderr, "too deep\n"); std::exit(2); }
      const PriceLevel* lv = sd == Side::Buy ? snap.bids : snap.asks; size_t n = sd == Side::Buy ? snap.bidCount : snap.askCount;
      size_t el = sd == Side::Buy ? book->getBidLevelsCount() : book->getAskLevelsCount();
      if (n != want.size() || el != want.size()) return "level count side=" + std::to_string((int)sd) + " eng=" + std::to_string(n) + "/" + std::to_string(el) + " ref=" + std::to_string(want.size());
      for (size_t k = 0; k < n; k++) {
        Quantity tq = 0; std::vector<OrderId> qids; for (auto& o : want[k].second) { tq += o.vis; qids.push_back(o.id); }
        if (lv[k].price != want[k].first || lv[k].totalQuantity != tq || lv[k].orderCount != qids.size())
          return "ladder sym" + std::to_string(kSyms[i]) + " lvl" + std::to_string(k) + " eng px=" + std::to_string(lv[k].price) + " q=" + std::to_string(lv[k].totalQuantity) + " n=" + std::to_string(lv[k].orderCount) + " ref px=" + std::to_string(want[k].first) + " q=" + std::to_string(tq) + " n=" + std::to_string(qids.size());
        auto it = taps[i].queues.find({sd == Side::Buy ? 0 : 1, lv[k].price});
        std::vector<OrderId> eq = it == taps[i].queues.end() ? std::vector<OrderId>{} : it->second;
        if (eq != qids) return "fifo queue sym" + std::to_string(kSyms[i]) + " px=" + std::to_string(lv[k].price);
      }
    }
    std::map<OrderId, std::string> em, rm;
    book->forEachOrder([&](const Order& o) { em[o.id] = std::to_string(o.price) + "x" + std::to_string(o.remainingQty) + "+" + std::to_string(o.cumQty); });
    for (Side sd : {Side::Buy, Side::Sell}) for (auto& [p, q] : R.own(sd)) for (auto& o : q) rm[o.id] = std::to_string(p) + "x" + std::to_string(o.qty) + "+" + std::to_string(o.cum);
    if (em != rm) return "resting orders differ sym" + std::to_string(kSyms[i]);
    std::string err; if (!book->validateIntegrity(&err)) return "integrity: " + err;
    return "";
  }
};

// ---------------- generator ----------------
struct Gen {
  std::mt19937_64 rng; Ref sh[NS]; explicit Gen(uint64_t s) : rng(s) {}
  uint64_t pick(uint64_t n) { return rng() % n; }
  OrderId liveId(Ref& r) { for (int a = 0; a < 24; a++) { OrderId id = 1 + pick(96); if (r.find(id)) return id; } return 1 + pick(96); }
  Price clampP(Price p) { int span = cfg.wide ? 9 : 4; Price lo = 1000000 - span * 100, hi = 1000000 + span * 100; return std::min(hi, std::max(lo, p)); }
  Price price(Ref& r, Side sd) {
    Price same = r.best(sd), oppb = r.best(sd == Side::Buy ? Side::Sell : Side::Buy); int sign = sd == Side::Buy ? 1 : -1; uint64_t x = pick(100);
    if (x < 60 && same) return clampP(same - sign * (Price)pick(4) * 100);
    if (x < 75 && oppb) return clampP(oppb);
    if (x < 85 && oppb) return clampP(oppb - sign * 100);
    if (x < 92 && oppb) return clampP(oppb + sign * (1 + (Price)pick(6)) * 100);
    int span = cfg.wide ? 9 : 4; return 1000000 + ((Price)pick(2 * span + 1) - span) * 100;
  }
  Op next() {
    size_t s = pick(NS); Ref& r = sh[s]; Op op{}; op.sym = kSyms[s]; op.req = kAnyParticipant;
    size_t cnt = 0; for (auto* b : {&r.bids, &r.asks}) for (auto& [p, q] : *b) cnt += q.size();
    uint64_t x = pick(100);
    if (cfg.kill && pick(1000) < 3) { op.k = K::Kill; op.owner = 1 + pick(4); for (auto& rr : sh) rr.killAll(op.owner); return op; }
    if (cnt >= 250 || x >= 66) {
      op.id = pick(100) < 20 ? 1 + pick(96) : liveId(r);
      op.req = pick(100) < 10 ? (ParticipantId)(1 + pick(4)) : kAnyParticipant;
      RO* o = r.find(op.id); Quantity cur = o ? o->qty : 5; Quantity tot = o ? o->qty + o->cum : 5;
      if (x >= 90 && cfg.repl) {
        op.k = K::Replace; Side sd = Side::Buy; Price px = 1000000; r.find(op.id, &sd, &px);
        uint64_t e = pick(100);
        op.px = e < 45 ? px : price(r, sd);
        uint64_t f = pick(100); op.qty = f < 40 ? 1 + pick(tot) : f < 55 ? tot : tot + 1 + pick(30);  // a total
      } else if (x >= 82 && !cfg.noModify) { op.k = K::Modify; uint64_t e = pick(100); op.qty = (cur > 1 && e < 70) ? 1 + pick(cur - 1) : e < 85 ? cur : cur + 1 + pick(5); }
      else op.k = K::Cancel;
      apply(op); return op;
    }
    op.k = K::Add; op.id = 1 + pick(96); for (int a = 0; a < 32 && r.find(op.id); a++) op.id = 1 + pick(96);
    op.side = pick(2) ? Side::Buy : Side::Sell;
    op.owner = cfg.stp ? (ParticipantId)(1 + pick(4)) : (op.side == Side::Buy ? 1 + pick(2) : 3 + pick(2));
    uint64_t t = pick(100);
    op.type = t < 75 ? OrderType::Limit : t < 80 ? OrderType::Market : t < 87 ? OrderType::IOC : t < 93 ? OrderType::FOK : (cfg.post ? OrderType::PostOnly : OrderType::Limit);
    op.px = price(r, op.side);
    Quantity cross = r.reachable(op.side, op.px, op.type == OrderType::Market, 0);
    uint64_t q = pick(100);
    if (cross && q < 30) { uint64_t e = pick(3); op.qty = e == 0 ? cross : e == 1 ? cross + 1 : (cross > 1 ? cross - 1 : 1); }
    else if (q < 92 || !cfg.huge) op.qty = 1 + pick(20);
    else if (q < 97) op.qty = 50 + pick(400);
    else op.qty = 1000000 + pick(1000000000);
    if (cfg.ice && op.type == OrderType::Limit && pick(100) < 15) {
      op.type = OrderType::Iceberg; if (op.qty < 10) op.qty += 10 + pick(60);
      // Mostly a legal display; 1 in 10 below the floor, to exercise the reject.
      op.disp = std::max<Quantity>(1 + pick(8), minDisplay(op.qty));
      if (pick(10) == 0 && minDisplay(op.qty) > 1) op.disp = minDisplay(op.qty) - 1;
    }
    if (op.type == OrderType::Market) op.px = 0;
    apply(op); return op;
  }
  void apply(const Op& op) { Ref& r = sh[op.sym - 1]; std::vector<Tr> t;
    switch (op.k) { case K::Add: r.add(op, t); break; case K::Cancel: r.cancel(op.id, op.req); break; case K::Modify: r.modify(op.id, op.qty, op.req); break; case K::Replace: r.replace(op, t); break; case K::Kill: break; } }
};

static std::optional<std::pair<size_t, std::string>> replay(const std::vector<Op>& f) {
  Harness h; for (size_t i = 0; i < f.size(); i++) { std::string d = h.step(f[i]); if (!d.empty()) return std::make_pair(i, d); } return std::nullopt;
}
static std::vector<Op> shrink(std::vector<Op> f) {
  auto fails = [](const std::vector<Op>& g) { return replay(g).has_value(); };
  auto d0 = replay(f); if (d0) f.resize(d0->first + 1);
  for (size_t ch = std::max<size_t>(f.size() / 2, 1);; ch /= 2) {
    bool prog = true; while (prog) { prog = false;
      for (size_t i = 0; i + ch <= f.size();) { std::vector<Op> c(f.begin(), f.begin() + i); c.insert(c.end(), f.begin() + i + ch, f.end());
        if (fails(c)) { f = c; prog = true; } else i += ch; } }
    if (ch == 1) break;
  }
  for (Op& o : f) { if (o.k == K::Cancel) continue; while (o.qty > 1) { Quantity sv = o.qty; o.qty = sv / 2; if (!fails(f)) { o.qty = sv; break; } } }
  return f;
}

int main(int argc, char** argv) {
  uint64_t base = 1; size_t seeds = 10, ops = 1200; int maxReport = 1;
  for (int i = 1; i < argc; i++) { std::string a = argv[i]; auto nx = [&] { return std::string(argv[++i]); };
    if (a == "--seed") base = std::stoull(nx()); else if (a == "--seeds") seeds = std::stoull(nx()); else if (a == "--ops") ops = std::stoull(nx());
    else if (a == "--report") maxReport = std::stoi(nx());
    else if (a == "--no-stp") cfg.stp = false; else if (a == "--no-post") cfg.post = false; else if (a == "--no-repl") cfg.repl = false;
    else if (a == "--no-exec") cfg.exec = false; else if (a == "--ice") cfg.ice = true; else if (a == "--stp-modes") cfg.stpModes = true; else if (a == "--kill") cfg.kill = true; else if (a == "--no-modify") cfg.noModify = true; else if (a == "--no-fokpartial") cfg.fokPartial = false; else if (a == "--fok-engine") cfg.fokEngine = true; else if (a == "--exec-skip-mod") cfg.execSkipMod = true; else if (a == "--no-wide") cfg.wide = false; else if (a == "--no-huge") cfg.huge = false; }
  std::map<std::string, int> classes; size_t total = 0; int reported = 0;
  for (size_t k = 0; k < seeds; k++) {
    Gen g(base + k); std::vector<Op> f; for (size_t i = 0; i < ops; i++) f.push_back(g.next());
    auto d = replay(f); total += d ? d->first + 1 : f.size();
    if (!d) continue;
    std::string cls = d->second.substr(0, d->second.find_first_of("=0123456789:"));
    if (classes[cls]++ == 0 && reported < maxReport) {
      reported++; auto m = shrink(f); auto dm = replay(m);
      std::printf("\n=== seed %llu op#%zu: %s\n--- minimal (%zu ops): %s\n", (unsigned long long)(base + k), d->first, d->second.c_str(), m.size(), dm ? dm->second.c_str() : "?");
      for (auto& o : m) std::printf("  %s\n", fmt(o).c_str());
    }
  }
  std::printf("\nops executed (to first divergence per seed): %zu over %zu seeds\n", total, seeds);
  for (auto& [c, n] : classes) std::printf("  class '%s': %d seeds\n", c.c_str(), n);
  return classes.empty() ? 0 : 1;
}
