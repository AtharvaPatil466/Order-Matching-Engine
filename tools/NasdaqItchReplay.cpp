// NasdaqItchReplay — replay a NASDAQ TotalView-ITCH 5.0 feed through the
// matching engine and validate the engine's reconstructed book against an
// independent reference book built directly from the same feed.
//
// WHY THIS EXISTS
// ---------------
// ITCH is an order-by-order market-data feed: every displayed order's add,
// execution, cancel, delete and replace is published. Feeding that stream
// into our engine (adds -> submitOrder, deletes/cancels/executions ->
// submitCancel / quantity reduction, replaces -> cancel+add) and comparing
// the resulting book against a second, minimal book reconstructed straight
// from the messages is a strong end-to-end correctness check: any divergence
// in best-bid/ask or aggregated depth is a bug in the engine's book
// maintenance (FlatPriceMap aggregation, cancel bookkeeping, etc.).
//
// COMPARISON POINT
// ----------------
// After every Trade ('P') message the harness compares the engine's
// getSnapshot() BBO + depth against the reference book. Trade messages are a
// natural, feed-driven checkpoint; they reference hidden/non-display liquidity
// and so must NOT perturb the displayed book — which is exactly the invariant
// we assert (the displayed book is unchanged across a 'P').
//
// REAL DATA
// ---------
// Authoritative validation requires a real sample from
// ftp.nasdaqtrader.com/Trades/ (e.g. 01302019.NASDAQ_ITCH50.gz — gunzip
// first). Run:  NasdaqItchReplay <path-to-decompressed-itch50-file>
// The file format is a sequence of records, each a 2-byte big-endian length
// prefix followed by that many message bytes (the message's first byte is the
// type). This is the standard NASDAQ binary-file framing.
//
// Because real NASDAQ data cannot be bundled here, running the tool with NO
// arguments executes a SYNTHETIC self-test: it generates a small, valid
// ITCH 5.0 byte stream (real wire layout, same parser path real data hits),
// replays it, and asserts the engine book matches the reference at every
// checkpoint. This exercises the full replay+compare path deterministically;
// it does NOT substitute for a real-sample run.
//
// The message layouts parsed/emitted below follow the NASDAQ TotalView-ITCH
// 5.0 specification exactly (big-endian, price is 4-implied-decimal u32 which
// maps 1:1 onto this engine's PRICE_PRECISION=10000 fixed point).

#include "ItchProtocol.h"   // readU16BE/readU32BE/readU64BE/readU48BE, writeU*BE
#include "MatchingEngine.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace OrderMatcher {
namespace itchreplay {

// ─── Real NASDAQ ITCH 5.0 message sizes (payload, excluding length prefix) ───
// Only the message types this reconstruction consumes are enumerated; the
// full spec also defines 'S'(12), 'R'(39) and 'Q'(40), which carry no
// incremental displayed-book delta here.
constexpr size_t NITCH_ADD_ORDER      = 36;
constexpr size_t NITCH_ADD_ORDER_MPID = 40;
constexpr size_t NITCH_ORDER_EXEC     = 31;
constexpr size_t NITCH_ORDER_EXEC_PX  = 36;
constexpr size_t NITCH_ORDER_CANCEL   = 23;
constexpr size_t NITCH_ORDER_DELETE   = 19;
constexpr size_t NITCH_ORDER_REPLACE  = 35;
constexpr size_t NITCH_TRADE          = 44;

// ─── Reference book ──────────────────────────────────────────────────────────
// An independent, deliberately simple order book reconstructed straight from
// the ITCH messages. std::map keyed by price gives sorted levels for free.
// This is the "source of truth" the engine's book is compared against.

struct RefLevel {
    Quantity qty = 0;      // aggregate displayed shares at this price
    uint32_t orders = 0;   // number of resting orders at this price
};

struct RefOrder {
    SymbolId symbol = 0;
    Side     side = Side::Buy;
    Price    price = 0;
    Quantity shares = 0;   // remaining displayed shares
};

class ReferenceBook {
public:
    // bids sorted descending, asks ascending — matches engine snapshot order.
    struct SymbolBook {
        std::map<Price, RefLevel, std::greater<Price>> bids;
        std::map<Price, RefLevel, std::less<Price>>    asks;
    };

    void add(uint64_t ref, SymbolId sym, Side side, Price price, Quantity shares) {
        RefOrder o{sym, side, price, shares};
        orders_[ref] = o;
        auto& b = books_[sym];
        if (side == Side::Buy) applyLevel(b.bids, price, static_cast<int64_t>(shares), +1);
        else                   applyLevel(b.asks, price, static_cast<int64_t>(shares), +1);
    }

    // Reduce a resting order by `delta` shares (execution or partial cancel).
    // Returns the remaining shares after reduction (0 => order fully gone).
    Quantity reduce(uint64_t ref, Quantity delta) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return 0;
        RefOrder& o = it->second;
        Quantity take = delta > o.shares ? o.shares : delta;
        o.shares -= take;
        auto& b = books_[o.symbol];
        if (o.side == Side::Buy)
            applyLevel(b.bids, o.price, -static_cast<int64_t>(take), o.shares == 0 ? -1 : 0);
        else
            applyLevel(b.asks, o.price, -static_cast<int64_t>(take), o.shares == 0 ? -1 : 0);
        Quantity remaining = o.shares;
        if (remaining == 0) orders_.erase(it);
        return remaining;
    }

    // Fully remove a resting order (Delete). Returns shares removed.
    Quantity remove(uint64_t ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return 0;
        Quantity had = it->second.shares;
        reduce(ref, had);
        return had;
    }

    bool has(uint64_t ref) const { return orders_.count(ref) != 0; }
    const RefOrder* find(uint64_t ref) const {
        auto it = orders_.find(ref);
        return it == orders_.end() ? nullptr : &it->second;
    }

    const SymbolBook* book(SymbolId sym) const {
        auto it = books_.find(sym);
        return it == books_.end() ? nullptr : &it->second;
    }

private:
    template <typename Map>
    static void applyLevel(Map& m, Price price, int64_t qtyDelta, int orderDelta) {
        auto& lvl = m[price];
        lvl.qty = static_cast<Quantity>(static_cast<int64_t>(lvl.qty) + qtyDelta);
        lvl.orders = static_cast<uint32_t>(static_cast<int64_t>(lvl.orders) + orderDelta);
        if (lvl.orders == 0 || lvl.qty == 0) m.erase(price);
    }

    std::unordered_map<uint64_t, RefOrder> orders_;
    std::unordered_map<SymbolId, SymbolBook> books_;
};

// ─── Symbol table: ITCH ticker (8-char ASCII) -> engine SymbolId ─────────────

class SymbolTable {
public:
    // Returns the SymbolId for a ticker, allocating (and registering with the
    // engine) on first sight. `newlyAdded` is set when a fresh symbol was made.
    SymbolId intern(const std::string& ticker, MatchingEngine& engine, bool& newlyAdded) {
        auto it = map_.find(ticker);
        if (it != map_.end()) { newlyAdded = false; return it->second; }
        SymbolId id = next_++;
        map_[ticker] = id;
        engine.addSymbol(id);
        newlyAdded = true;
        return id;
    }
    bool known(const std::string& ticker) const { return map_.count(ticker) != 0; }

private:
    std::unordered_map<std::string, SymbolId> map_;
    SymbolId next_ = 0;
};

// Parse an 8-byte ITCH stock field into a trimmed ticker string.
inline std::string parseTicker(const uint8_t* p, size_t len = 8) {
    size_t end = len;
    while (end > 0 && (p[end - 1] == ' ' || p[end - 1] == '\0')) --end;
    return std::string(reinterpret_cast<const char*>(p), end);
}

// ─── Divergence report ───────────────────────────────────────────────────────

struct CompareStats {
    uint64_t checkpoints = 0;
    uint64_t mismatches = 0;
};

// Compare engine snapshot vs reference book for one symbol. Logs the first
// few divergences. Returns true when they agree to `depth` levels.
inline bool compareBook(MatchingEngine& engine, SymbolId sym,
                        const ReferenceBook& ref, size_t depth,
                        uint64_t msgIndex) {
    MarketDataSnapshot snap = engine.getSnapshot(sym, depth);
    const ReferenceBook::SymbolBook* rb = ref.book(sym);

    // Gather reference top-of-book levels (already sorted by the maps).
    std::vector<PriceLevel> refBids, refAsks;
    if (rb) {
        for (auto& [px, lvl] : rb->bids) {
            if (refBids.size() >= depth) break;
            refBids.push_back({px, lvl.qty, lvl.orders});
        }
        for (auto& [px, lvl] : rb->asks) {
            if (refAsks.size() >= depth) break;
            refAsks.push_back({px, lvl.qty, lvl.orders});
        }
    }

    auto sideOk = [&](const char* label, const PriceLevel* eng, size_t engN,
                      const std::vector<PriceLevel>& r) -> bool {
        if (engN != r.size()) {
            std::printf("  [msg %llu] %s DEPTH mismatch: engine=%zu ref=%zu\n",
                        (unsigned long long)msgIndex, label, engN, r.size());
            return false;
        }
        for (size_t i = 0; i < engN; ++i) {
            if (eng[i].price != r[i].price ||
                eng[i].totalQuantity != r[i].totalQuantity ||
                eng[i].orderCount != r[i].orderCount) {
                std::printf("  [msg %llu] %s L%zu mismatch: engine{px=%lld,qty=%llu,n=%u} "
                            "ref{px=%lld,qty=%llu,n=%u}\n",
                            (unsigned long long)msgIndex, label, i,
                            (long long)eng[i].price,
                            (unsigned long long)eng[i].totalQuantity, eng[i].orderCount,
                            (long long)r[i].price,
                            (unsigned long long)r[i].totalQuantity, r[i].orderCount);
                return false;
            }
        }
        return true;
    };

    bool ok = sideOk("BID", snap.bids, snap.bidCount, refBids);
    ok = sideOk("ASK", snap.asks, snap.askCount, refAsks) && ok;
    return ok;
}

// ─── The replay harness ──────────────────────────────────────────────────────

class ReplayHarness {
public:
    explicit ReplayHarness(size_t depth = 10) : depth_(depth) {}

    // Replay a whole length-prefixed ITCH byte buffer. Two passes:
    //   1. Register every symbol (addSymbol is frozen once start() runs, and
    //      real ITCH files front-load Stock Directory / Add messages, so we
    //      intern all tickers up front — matching venue behavior).
    //   2. start() the engine (sync mode: deterministic, no worker threads)
    //      and stream the messages through the book-maintenance handlers.
    // Returns the number of records consumed.
    uint64_t replay(const uint8_t* buf, size_t len) {
        prePassRegisterSymbols(buf, len);
        engine_.start();
        size_t off = 0;
        uint64_t idx = 0;
        while (off + 2 <= len) {
            uint16_t msgLen = readU16BE(buf + off);
            off += 2;
            if (msgLen == 0 || off + msgLen > len) break;  // truncated tail
            onMessage(buf + off, msgLen, idx++);
            off += msgLen;
        }
        return idx;
    }

    // Feed one raw ITCH message (without the length prefix). Returns false on a
    // malformed/short message (the caller can decide to stop).
    bool onMessage(const uint8_t* m, size_t len, uint64_t msgIndex) {
        if (len == 0) return false;
        switch (m[0]) {
        case 'A': return onAddOrder(m, len, /*mpid=*/false, msgIndex);
        case 'F': return onAddOrder(m, len, /*mpid=*/true,  msgIndex);
        case 'E': return onExecuted(m, len);
        case 'C': return onExecutedWithPrice(m, len);
        case 'X': return onCancel(m, len);
        case 'D': return onDelete(m, len);
        case 'U': return onReplace(m, len, msgIndex);
        case 'P': return onTrade(m, len, msgIndex);
        // Informational / no book effect for reconstruction purposes.
        case 'S':  // System event
        case 'R':  // Stock directory
        case 'H':  // Trading action
        case 'Q':  // Cross trade (auction print — no incremental book delta here)
        default:
            return true;
        }
    }

    const CompareStats& stats() const { return stats_; }
    uint64_t addCount() const { return adds_; }
    uint64_t execCount() const { return execs_; }
    uint64_t rejectCount() const { return engineRejects_; }

private:
    // Pass 1: register every ticker-bearing symbol before the engine freezes
    // its symbol set at start(). Covers 'R' (Stock Directory), 'A'/'F' (Add),
    // 'P' (Trade) and 'Q' (Cross) — every message that carries a stock field.
    void prePassRegisterSymbols(const uint8_t* buf, size_t len) {
        size_t off = 0;
        while (off + 2 <= len) {
            uint16_t msgLen = readU16BE(buf + off);
            off += 2;
            if (msgLen == 0 || off + msgLen > len) break;
            const uint8_t* m = buf + off;
            off += msgLen;
            bool dummy = false;
            switch (m[0]) {
            case 'R': if (msgLen >= 19) symbols_.intern(parseTicker(m + 11), engine_, dummy); break;
            case 'A':
            case 'F': if (msgLen >= NITCH_ADD_ORDER) symbols_.intern(parseTicker(m + 24), engine_, dummy); break;
            case 'P': if (msgLen >= NITCH_TRADE) symbols_.intern(parseTicker(m + 24), engine_, dummy); break;
            case 'Q': if (msgLen >= 27) symbols_.intern(parseTicker(m + 19), engine_, dummy); break;
            default: break;
            }
        }
    }

    bool onAddOrder(const uint8_t* m, size_t len, bool mpid, uint64_t) {
        if (len < (mpid ? NITCH_ADD_ORDER_MPID : NITCH_ADD_ORDER)) return false;
        uint64_t ref = readU64BE(m + 11);
        Side side = (m[19] == 'B') ? Side::Buy : Side::Sell;
        Quantity shares = readU32BE(m + 20);
        std::string ticker = parseTicker(m + 24);
        Price price = static_cast<Price>(readU32BE(m + 32));

        bool added = false;
        SymbolId sym = symbols_.intern(ticker, engine_, added);

        // ITCH order-ref numbers are unique per trading day → use directly as
        // the engine OrderId. participant is irrelevant for book geometry.
        SubmitResult r = engine_.submitOrder(sym, ref, /*participant=*/1, side,
                                             price, shares, OrderType::Limit);
        if (!r.isAccepted()) {
            ++engineRejects_;
            // Do NOT record in the reference book: an add the engine refused
            // (e.g. price outside the FlatPriceMap window) legitimately should
            // not be in either book. Recording it would manufacture a spurious
            // mismatch and hide the real (out-of-range) signal.
            return true;
        }
        ref_.add(ref, sym, side, price, shares);
        ++adds_;
        return true;
    }

    bool onExecuted(const uint8_t* m, size_t len) {
        if (len < NITCH_ORDER_EXEC) return false;
        uint64_t ref = readU64BE(m + 11);
        Quantity execShares = readU32BE(m + 19);
        return applyReduction(ref, execShares, /*isExec=*/true);
    }

    bool onExecutedWithPrice(const uint8_t* m, size_t len) {
        if (len < NITCH_ORDER_EXEC_PX) return false;
        uint64_t ref = readU64BE(m + 11);
        Quantity execShares = readU32BE(m + 19);
        // Execution price (bytes 32..35) is the print price; it does not change
        // the resting order's own limit, so the book delta is just a reduction.
        return applyReduction(ref, execShares, /*isExec=*/true);
    }

    bool onCancel(const uint8_t* m, size_t len) {
        if (len < NITCH_ORDER_CANCEL) return false;
        uint64_t ref = readU64BE(m + 11);
        Quantity cancelledShares = readU32BE(m + 19);
        return applyReduction(ref, cancelledShares, /*isExec=*/false);
    }

    bool onDelete(const uint8_t* m, size_t len) {
        if (len < NITCH_ORDER_DELETE) return false;
        uint64_t ref = readU64BE(m + 11);
        const RefOrder* o = ref_.find(ref);
        if (!o) return true;              // unknown / already gone
        SymbolId sym = o->symbol;
        engine_.submitCancel(sym, ref);
        ref_.remove(ref);
        return true;
    }

    bool onReplace(const uint8_t* m, size_t len, uint64_t) {
        if (len < NITCH_ORDER_REPLACE) return false;
        uint64_t origRef = readU64BE(m + 11);
        uint64_t newRef  = readU64BE(m + 19);
        Quantity shares  = readU32BE(m + 27);
        Price price      = static_cast<Price>(readU32BE(m + 31));

        const RefOrder* o = ref_.find(origRef);
        if (!o) return true;              // can't replace what we never saw
        SymbolId sym = o->symbol;
        Side side = o->side;              // replace keeps side

        engine_.submitCancel(sym, origRef);
        ref_.remove(origRef);

        SubmitResult r = engine_.submitOrder(sym, newRef, /*participant=*/1, side,
                                             price, shares, OrderType::Limit);
        if (!r.isAccepted()) { ++engineRejects_; return true; }
        ref_.add(newRef, sym, side, price, shares);
        return true;
    }

    // Reduce a resting order in BOTH books by execShares, keeping them in lock
    // step: full execution/cancel -> engine cancel; partial -> engine modify
    // (modifyOrder only shrinks remaining qty, which is exactly a reduction).
    bool applyReduction(uint64_t ref, Quantity delta, bool isExec) {
        const RefOrder* o = ref_.find(ref);
        if (!o) return true;              // unknown order — ignore
        SymbolId sym = o->symbol;
        Quantity before = o->shares;
        Quantity remaining = before > delta ? before - delta : 0;
        if (remaining == 0) engine_.submitCancel(sym, ref);
        else                engine_.submitModify(sym, ref, remaining);
        ref_.reduce(ref, delta);
        if (isExec) ++execs_;
        return true;
    }

    bool onTrade(const uint8_t* m, size_t len, uint64_t msgIndex) {
        if (len < NITCH_TRADE) return false;
        // 'P' is a non-display trade: it references hidden liquidity and must
        // not change the displayed book. Use it as a checkpoint: assert the
        // engine's displayed book still equals the reference for the symbol.
        std::string ticker = parseTicker(m + 24);
        if (!symbols_.known(ticker)) return true;
        bool dummy = false;
        SymbolId sym = symbols_.intern(ticker, engine_, dummy);
        ++stats_.checkpoints;
        if (!compareBook(engine_, sym, ref_, depth_, msgIndex)) {
            ++stats_.mismatches;
        }
        return true;
    }

    MatchingEngine engine_;
    SymbolTable    symbols_;
    ReferenceBook  ref_;
    size_t         depth_;
    CompareStats   stats_;
    uint64_t       adds_ = 0;
    uint64_t       execs_ = 0;
    uint64_t       engineRejects_ = 0;
};

// ─── File framing: [u16 BE length][message bytes] records ────────────────────

inline int replayFile(const std::string& path, size_t depth) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "NasdaqItchReplay: cannot open %s\n", path.c_str());
        return 2;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    std::printf("Loaded %zu bytes from %s\n", buf.size(), path.c_str());

    ReplayHarness harness(depth);
    uint64_t msgIndex = harness.replay(buf.data(), buf.size());

    const CompareStats& s = harness.stats();
    std::printf("\nReplay complete: %llu messages, %llu adds, %llu executions, "
                "%llu engine rejects\n",
                (unsigned long long)msgIndex,
                (unsigned long long)harness.addCount(),
                (unsigned long long)harness.execCount(),
                (unsigned long long)harness.rejectCount());
    std::printf("Checkpoints (Trade msgs): %llu, mismatches: %llu\n",
                (unsigned long long)s.checkpoints,
                (unsigned long long)s.mismatches);
    if (s.mismatches != 0) {
        std::printf("RESULT: FAIL — engine book diverged from feed reconstruction\n");
        return 1;
    }
    std::printf("RESULT: PASS — engine book matched feed reconstruction at every "
                "checkpoint\n");
    return 0;
}

// ─── Synthetic ITCH 5.0 stream generator (real wire layout) ──────────────────
// Emits length-prefixed records into `out`, mirroring the on-disk NASDAQ file
// format. Prices are 4-implied-decimal (PRICE_PRECISION). Kept intentionally
// tiny and human-checkable; NOT a substitute for real-sample validation.

class SyntheticStream {
public:
    void addOrder(uint64_t ref, Side side, uint32_t shares,
                  const char* ticker, Price price) {
        uint8_t m[NITCH_ADD_ORDER];
        m[0] = 'A';
        writeU16BE(m + 1, 1);            // stock locate
        writeU16BE(m + 3, 0);            // tracking
        writeU48BE(m + 5, ts_++);        // timestamp
        writeU64BE(m + 11, ref);
        m[19] = (side == Side::Buy) ? 'B' : 'S';
        writeU32BE(m + 20, shares);
        writeTicker(m + 24, ticker);
        writeU32BE(m + 32, static_cast<uint32_t>(price));
        emit(m, sizeof(m));
    }

    void executed(uint64_t ref, uint32_t execShares, uint64_t matchNum) {
        uint8_t m[NITCH_ORDER_EXEC];
        m[0] = 'E';
        writeU16BE(m + 1, 1);
        writeU16BE(m + 3, 0);
        writeU48BE(m + 5, ts_++);
        writeU64BE(m + 11, ref);
        writeU32BE(m + 19, execShares);
        writeU64BE(m + 23, matchNum);
        emit(m, sizeof(m));
    }

    void cancel(uint64_t ref, uint32_t cancelShares) {
        uint8_t m[NITCH_ORDER_CANCEL];
        m[0] = 'X';
        writeU16BE(m + 1, 1);
        writeU16BE(m + 3, 0);
        writeU48BE(m + 5, ts_++);
        writeU64BE(m + 11, ref);
        writeU32BE(m + 19, cancelShares);
        emit(m, sizeof(m));
    }

    void del(uint64_t ref) {
        uint8_t m[NITCH_ORDER_DELETE];
        m[0] = 'D';
        writeU16BE(m + 1, 1);
        writeU16BE(m + 3, 0);
        writeU48BE(m + 5, ts_++);
        writeU64BE(m + 11, ref);
        emit(m, sizeof(m));
    }

    void replace(uint64_t origRef, uint64_t newRef, uint32_t shares, Price price) {
        uint8_t m[NITCH_ORDER_REPLACE];
        m[0] = 'U';
        writeU16BE(m + 1, 1);
        writeU16BE(m + 3, 0);
        writeU48BE(m + 5, ts_++);
        writeU64BE(m + 11, origRef);
        writeU64BE(m + 19, newRef);
        writeU32BE(m + 27, shares);
        writeU32BE(m + 31, static_cast<uint32_t>(price));
        emit(m, sizeof(m));
    }

    void trade(const char* ticker, Side side, uint32_t shares, Price price,
               uint64_t matchNum) {
        uint8_t m[NITCH_TRADE];
        m[0] = 'P';
        writeU16BE(m + 1, 1);
        writeU16BE(m + 3, 0);
        writeU48BE(m + 5, ts_++);
        writeU64BE(m + 11, 0);           // order ref (0 for non-display)
        m[19] = (side == Side::Buy) ? 'B' : 'S';
        writeU32BE(m + 20, shares);
        writeTicker(m + 24, ticker);
        writeU32BE(m + 32, static_cast<uint32_t>(price));
        writeU64BE(m + 36, matchNum);
        emit(m, sizeof(m));
    }

    const std::vector<uint8_t>& bytes() const { return out_; }

private:
    static void writeTicker(uint8_t* p, const char* ticker) {
        std::memset(p, ' ', 8);
        size_t n = std::strlen(ticker);
        if (n > 8) n = 8;
        std::memcpy(p, ticker, n);
    }
    void emit(const uint8_t* m, size_t len) {
        uint8_t lp[2];
        writeU16BE(lp, static_cast<uint16_t>(len));
        out_.insert(out_.end(), lp, lp + 2);
        out_.insert(out_.end(), m, m + len);
    }

    std::vector<uint8_t> out_;
    uint64_t ts_ = 1;
};

// ─── Synthetic self-test ─────────────────────────────────────────────────────

inline int runSyntheticSelfTest() {
    std::printf("── NasdaqItchReplay SYNTHETIC self-test ──\n");
    std::printf("(no real NASDAQ file supplied; exercising the replay+compare "
                "path on a generated ITCH 5.0 stream)\n\n");

    // Base price 100.0000 = 1,000,000 in price(4). Keep everything inside the
    // engine's auto-centered FlatPriceMap window (±100000 ticks = ±$10).
    const char* SYM = "AAPL";
    SyntheticStream s;

    // Build a two-sided book with multiple levels and orders per level.
    s.addOrder(1, Side::Buy,  100, SYM,  999000);  // bid 99.90 x100
    s.addOrder(2, Side::Buy,  200, SYM,  999000);  // bid 99.90 x200 (same level)
    s.addOrder(3, Side::Buy,  150, SYM,  998000);  // bid 99.80 x150
    s.addOrder(4, Side::Sell, 120, SYM, 1001000);  // ask 100.10 x120
    s.addOrder(5, Side::Sell, 300, SYM, 1002000);  // ask 100.20 x300
    // Checkpoint 1: full two-sided book.
    s.trade(SYM, Side::Buy, 1, 1000000, 1001);

    // Partial execution of order 2 (200 -> 130 at its level).
    s.executed(2, 70, 2001);
    // Checkpoint 2.
    s.trade(SYM, Side::Sell, 1, 1000000, 1002);

    // Partial cancel of order 3 (150 -> 100).
    s.cancel(3, 50);
    // Full delete of order 4 (removes the 100.10 ask level entirely).
    s.del(4);
    // Checkpoint 3.
    s.trade(SYM, Side::Buy, 1, 1000000, 1003);

    // Replace order 5: move ask from 100.20 x300 to 100.15 x250 (new ref 6).
    s.replace(5, 6, 250, 1001500);
    // Checkpoint 4.
    s.trade(SYM, Side::Sell, 1, 1000000, 1004);

    // Fully execute order 1 (bid 99.90 loses 100; level still has order 2's 130).
    s.executed(1, 100, 2005);
    // Execute remaining of order 2 exactly (130 -> 0; level 99.90 disappears).
    s.executed(2, 130, 2006);
    // Checkpoint 5: best bid should now be 99.80.
    s.trade(SYM, Side::Buy, 1, 1000000, 1005);

    // Add a second symbol to prove multi-symbol isolation.
    const char* SYM2 = "MSFT";
    s.addOrder(100, Side::Buy,  50, SYM2, 2000000);  // 200.00
    s.addOrder(101, Side::Sell, 60, SYM2, 2001000);  // 200.10
    s.trade(SYM2, Side::Buy, 1, 2000000, 1006);

    // Replay the generated stream through the harness (same code real data hits).
    ReplayHarness harness(/*depth=*/10);
    const std::vector<uint8_t>& buf = s.bytes();
    uint64_t idx = harness.replay(buf.data(), buf.size());

    const CompareStats& st = harness.stats();
    std::printf("messages=%llu adds=%llu execs=%llu rejects=%llu\n",
                (unsigned long long)idx,
                (unsigned long long)harness.addCount(),
                (unsigned long long)harness.execCount(),
                (unsigned long long)harness.rejectCount());
    std::printf("checkpoints=%llu mismatches=%llu\n",
                (unsigned long long)st.checkpoints,
                (unsigned long long)st.mismatches);

    // 7 'A' messages (refs 1-5 on AAPL, 100/101 on MSFT); ref 6 arrives via a
    // Replace, which is not counted as an add.
    bool ok = (st.checkpoints == 6) && (st.mismatches == 0) &&
              (harness.rejectCount() == 0) && (harness.addCount() == 7);
    if (!ok) {
        std::printf("\nSYNTHETIC SELF-TEST: FAIL\n");
        return 1;
    }
    std::printf("\nSYNTHETIC SELF-TEST: PASS\n");
    std::printf("NOTE: this validates the replay+compare mechanics only. "
                "Authoritative\n      correctness requires a real "
                "ftp.nasdaqtrader.com ITCH 5.0 sample:\n"
                "      NasdaqItchReplay <decompressed-itch50-file>\n");
    return 0;
}

}  // namespace itchreplay
}  // namespace OrderMatcher

int main(int argc, char** argv) {
    using namespace OrderMatcher::itchreplay;
    if (argc >= 2) {
        size_t depth = 10;
        if (argc >= 3) depth = static_cast<size_t>(std::strtoul(argv[2], nullptr, 10));
        return replayFile(argv[1], depth);
    }
    return runSyntheticSelfTest();
}
