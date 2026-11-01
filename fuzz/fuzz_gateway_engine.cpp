// fuzz_gateway_engine — roadmap 1.6 "Done": one wire message, legal or not,
// cannot corrupt the book.
//
// The input is the byte stream one client sends the native gateway. It goes
// through TcpGateway's own framer (feedBytes -> decodeFrame -> processMessage)
// into MatchingEngine::submitOrder on a synchronous engine shaped like
// GatewayServer's (symbols 0 and 1, no credential store, no session end).
// After the stream, and again after an expiry sweep, every book must pass
// OrderBook::validateIntegrity; the engine's own asserts are armed (-UNDEBUG)
// and UBSan halts. Any of the three is a finding.
//
// A fresh engine per input keeps every crash reproducible from its file. No
// socket: the harness hands feedBytes fd -1, so responses find no client and
// are dropped, exactly as for a client that vanished.

#include "MatchingEngine.h"
#include "TcpGateway.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace OrderMatcher {

// Befriended by TcpGateway for this harness only.
struct TcpGatewayFuzzAccess {
    static void feed(TcpGateway& gw, const uint8_t* data, size_t size) {
        TcpGateway::ClientState st{};
        st.readBuf.assign(data, data + size);
        if (st.readBuf.size() < sizeof(uint32_t)) st.readBuf.resize(sizeof(uint32_t));
        st.readPos = size;
        st.lastActivity = std::chrono::steady_clock::now();
        (void)gw.feedBytes(/*fd=*/-1, st);
    }
};

}  // namespace OrderMatcher

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSymbols[] = {0, 1};

void checkBooks(MatchingEngine& engine, const char* when) {
    for (SymbolId s : kSymbols) {
        std::string err;
        if (!engine.getOrderBook(s)->validateIntegrity(&err)) {
            std::fprintf(stderr, "book %u fails validateIntegrity %s: %s\n", s, when,
                         err.c_str());
            std::abort();
        }
    }
}

// Small books: one is built per input, so its size is the exec rate. A static
// initializer rather than LLVMFuzzerInitialize so the replay driver gets it too;
// the sizes are read at the first addSymbol.
[[maybe_unused]] const bool kSmallBooks = [] {
    setenv("OB_ORDER_POOL_CAPACITY", "2048", 0);
    setenv("OB_TRADE_HISTORY_CAPACITY", "1024", 0);
    return true;
}();

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    MatchingEngine engine;
    for (SymbolId s : kSymbols) engine.addSymbol(s);
    engine.setSessionEndScheduled(false);  // as GatewayServer
    engine.start();
    {
        TcpGateway gateway(engine);
        TcpGatewayFuzzAccess::feed(gateway, data, size);
    }
    checkBooks(engine, "after the stream");
    engine.expireOrders(std::numeric_limits<uint64_t>::max());
    checkBooks(engine, "after an expiry sweep");
    engine.stop();
    return 0;
}
