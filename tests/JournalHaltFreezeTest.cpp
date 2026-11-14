// JournalHaltFreezeTest — once the journal has halted, no path changes a book
// (JOURNAL-6, DA-1, DA-2).
//
// JournalHaltTest pins that new orders, cancels and modifies are refused at
// admission. Everything else that moves a book kept running on a halted
// journal: the kill switch (sync and async, through TcpGateway too), the DAY
// and expiry sweeps, session transitions and the volatility reopen, and any
// request already queued for a worker. Each cancelled or moved the book in
// memory with nothing written, so a restart put it all back: the kill switch
// was answered as if it had worked and every "killed" order came back live.
//
// Each case runs in a child under RLIMIT_FSIZE (SIGXFSZ ignored, so write(2)
// answers EFBIG). Orders 1-3 rest durably; order 4's record crosses the limit
// and halts the journal. Then every other mutating path is driven, and the book
// must still hold exactly what it held at the halt.

#include "AdminServer.h"
#include "GatewayProcess.h"
#include "GatewayTestClient.h"
#include "MatchingEngine.h"
#include "OrderBook.h"
#include "TcpGateway.h"
#include "TempPath.h"

#include <atomic>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr ParticipantId kOwner = 7;
constexpr int kDurable = 3;           // orders 1..3 reach the disk
constexpr uint64_t kGtdExpiry = 1000; // order 1's expiry, on the test clock

enum Code : int {
    kOk = 0,
    kSetup = 90,
    kNotHalted,
    kKillMoved,          // the kill switch cancelled in memory
    kEngineKillMoved,    // setKillSwitch(true)'s sweep cancelled in memory
    kExpiryMoved,        // the expiry sweep cancelled in memory
    kSessionMoved,       // a session transition changed the trading state
    kSessionEndMoved,    // endTradingSession closed the book
    kGatewayKillAcked,   // TcpGateway answered the kill switch Ack
    kBookServed,         // the admin port served a book view while halted
    kSnapshotChanged,    // getSnapshot did not show the book in memory
};

const char* describe(int code) {
    switch (code) {
    case kOk:               return "ok";
    case kSetup:            return "setup failed";
    case kNotHalted:        return "the journal did not halt";
    case kKillMoved:        return "killSwitch cancelled orders on a halted journal";
    case kEngineKillMoved:  return "setKillSwitch(true) cancelled orders on a halted journal";
    case kExpiryMoved:      return "the expiry sweep cancelled an order on a halted journal";
    case kSessionMoved:     return "a session transition moved a book on a halted journal";
    case kSessionEndMoved:  return "endTradingSession closed a book on a halted journal";
    case kGatewayKillAcked: return "TcpGateway acked a kill switch the journal could not record";
    case kBookServed:       return "the admin port served a book view on a halted journal";
    case kSnapshotChanged:  return "getSnapshot did not serve the book as it stands in memory";
    default:                return "?";
    }
}

std::atomic<uint64_t> gClock{500};

bool limitFileSize(rlim_t bytes) {
    std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit rl{bytes, bytes};
    return setrlimit(RLIMIT_FSIZE, &rl) == 0;
}

// Header, the sequence-id lease, three whole records, then half of the fourth.
rlim_t midFourthRecord() {
    return sizeof(JournalFileHeader) + (1 + kDurable) * sizeof(JournalEntry) +
           sizeof(JournalEntry) / 2;
}

// Order 1 is GTD, the rest GTC; all rest, none cross.
SubmitResult restingBuy(MatchingEngine& e, OrderId id) {
    const bool gtd = id == 1;
    return e.submitOrder(kSym, id, kOwner, Side::Buy, 10000 - static_cast<Price>(id), 10,
                         OrderType::Limit, 0, 0, gtd ? TimeInForce::GTD : TimeInForce::GTC,
                         gtd ? kGtdExpiry : 0);
}

size_t resting(MatchingEngine& e) {
    size_t n = 0;
    e.getOrderBook(kSym)->forEachOrderLocked([&](const Order&) { ++n; });
    return n;
}

size_t restoredFrom(const std::string& path) {
    MatchingEngine restarted;
    restarted.addSymbol(kSym);
    restarted.enableJournal(path);
    restarted.replayJournal();
    return resting(restarted);
}

int inChild(const char* name, const std::function<int()>& body) {
    std::fflush(stdout);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) _exit(body());
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    if (WIFSIGNALED(status)) {
        std::printf("  %s: child killed by signal %d\n", name, WTERMSIG(status));
        return -WTERMSIG(status);
    }
    std::printf("  %s: %s\n", name, describe(WEXITSTATUS(status)));
    return WEXITSTATUS(status);
}

// Rest 1..3, halt on 4. Returns kOk with the journal halted.
int restThenHalt(MatchingEngine& e, bool async) {
    e.addSymbol(kSym);
    e.setExpiryClock([] { return gClock.load(); });
    if (async) e.startAsync(1); else e.start();
    for (OrderId id = 1; id <= kDurable + 1; ++id) (void)restingBuy(e, id);
    e.waitForDrain();
    return e.getJournal()->halted() ? kOk : kNotHalted;
}

// The paths both engines share, driven after the halt.
int driveEveryOtherPath(MatchingEngine& e) {
    const size_t atHalt = resting(e);
    (void)e.killSwitch(kOwner);
    e.waitForDrain();
    if (resting(e) != atHalt) return kKillMoved;

    e.setKillSwitch(true);
    e.setKillSwitch(false);
    if (resting(e) != atHalt) return kEngineKillMoved;

    gClock.store(kGtdExpiry + 1);
    e.expireOrdersFromClock();
    e.waitForDrain();
    if (resting(e) != atHalt) return kExpiryMoved;

    (void)e.setTradingStateBatch({kSym}, TradingState::Halted);
    if (e.getOrderBook(kSym)->getTradingState() != TradingState::Continuous) return kSessionMoved;

    (void)e.endTradingSession({kSym});
    if (e.getOrderBook(kSym)->getTradingState() != TradingState::Continuous)
        return kSessionEndMoved;
    if (resting(e) != atHalt) return kSessionEndMoved;
    return kOk;
}

void runCase(const char* name, bool async) {
    const std::string path = uniqueTempPath(async ? "halt_freeze_async.wal" : "halt_freeze_sync.wal");
    std::filesystem::remove(path);
    const int rc = inChild(name, [&]() -> int {
        if (!limitFileSize(midFourthRecord())) return kSetup;
        MatchingEngine e;
        e.enableJournal(path, Journal::SyncPolicy::Immediate);
        if (const int r = restThenHalt(e, async); r != kOk) return r;
        return driveEveryOtherPath(e);
    });
    const size_t restored = restoredFrom(path);
    std::filesystem::remove(path);
    std::printf("  %s: %zu resting after restart\n", name, restored);
    assert(rc == kOk && "a path changed the book on a halted journal");
    assert(restored == static_cast<size_t>(kDurable));
}

// DA-1 over TCP on a sync engine: the kill switch must not be acknowledged.
void runGatewayKill() {
    const std::string path = uniqueTempPath("halt_freeze_gateway.wal");
    std::filesystem::remove(path);
    const int rc = inChild("sync gateway kill", [&]() -> int {
        if (!limitFileSize(midFourthRecord())) return kSetup;
        MatchingEngine e;
        e.enableJournal(path, Journal::SyncPolicy::Immediate);
        if (const int r = restThenHalt(e, false); r != kOk) return r;
        const size_t atHalt = resting(e);
        TcpGateway gateway(e);
        if (!gateway.start(0)) return kSetup;
        TestClient client;
        if (!client.connect(gateway.port())) return kSetup;
        client.setRecvTimeout(30);
        OrderRequest kill{};
        kill.type = OrderRequest::Type::KillSwitch;
        kill.participantId = kOwner;
        GatewayResponse resp{};
        if (!client.sendOrder(kill) || !client.recvResponse(resp)) return kSetup;
        if (resting(e) != atHalt) return kKillMoved;
        if (resp.type == GatewayResponse::Type::Ack) return kGatewayKillAcked;
        if (resp.rejectReason != RejectReason::JournalHalted) return kGatewayKillAcked;
        return kOk;
    });
    std::filesystem::remove(path);
    assert(rc == kOk && "the gateway acked a kill switch on a halted journal");
}

// Reads while halted (Rule 2.7). The sync engine applied order 4 before its
// record failed, so memory holds 4 orders and the disk 3. The admin port's
// views of the book refuse; getSnapshot, an in-process call whose caller can
// ask getJournal()->halted(), serves what is in memory.
void runReadsWhileHalted() {
    const std::string path = uniqueTempPath("halt_freeze_reads.wal");
    std::filesystem::remove(path);
    const int rc = inChild("reads while halted", [&]() -> int {
        if (!limitFileSize(midFourthRecord())) return kSetup;
        MatchingEngine e;
        e.enableJournal(path, Journal::SyncPolicy::Immediate);
        if (const int r = restThenHalt(e, false); r != kOk) return r;
        if (e.getSnapshot(kSym, 10).bidCount != kDurable + 1) return kSnapshotChanged;
        AdminServer admin(e, 0);
        admin.setAuthDisabled(true);
        if (!admin.start()) return kSetup;
        for (const char* view : {"/book", "/audit", "/auction"}) {
            const std::string body =
                gwtest::httpGet(admin.port(), std::string(view) + "?symbolId=" + std::to_string(kSym));
            if (body.find("journal_halted") == std::string::npos) return kBookServed;
        }
        if (gwtest::httpGet(admin.port(), "/health").find("ok") == std::string::npos) return kSetup;
        admin.stop();
        return kOk;
    });
    std::filesystem::remove(path);
    assert(rc == kOk && "a book view was served on a halted journal");
}

}  // namespace

int main() {
    runReadsWhileHalted();
    runCase("sync", false);
    runCase("async", true);
    runGatewayKill();
    std::puts("JournalHaltFreezeTest passed");
    return 0;
}
