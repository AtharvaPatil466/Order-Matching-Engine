// JournalHaltTest — what the ENGINE does once its journal cannot write
// (roadmap 1.4-D4; JRN-4, JRN-7).
//
// A real write error — RLIMIT_FSIZE in a child, with SIGXFSZ ignored so the
// kernel answers EFBIG — used to abort the process (3c4322a): safe, but it took
// every session down with it and left nothing to tell a client why. The journal
// now HALTS instead, and the engine has to act on that:
//   - the request whose commit failed is not reported accepted, and nothing it
//     produced reaches the client (the durable-ack gate keeps it held);
//   - every later order, cancel or modify is rejected with JournalHalted, on
//     both the sync path and at async admission;
//   - no checkpoint runs: the in-memory book holds state the disk refused, and
//     a checkpoint would make it durable behind the client's back;
//   - a checkpoint whose OWN temp file fails (h3: RLIMIT_FSIZE=2000 against a
//     4 KB book) leaves the live journal as it was, and the process running.

#include "MatchingEngine.h"
#include "OrderBook.h"
#include "TempPath.h"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr SymbolId kSym = 1;
constexpr ParticipantId kOwner = 7;
constexpr int kWholeBeforeLimit = 3;

// Child exit codes. 0 is success; anything else names the broken expectation.
enum Code : int {
    kOk = 0,
    kSetrlimitFailed = 90,
    kAcceptedUndurable,      // the order whose commit failed was reported accepted
    kWrongReason,            // rejected, but not with JournalHalted
    kEventLeaked,            // the client heard about the order the disk refused
    kAdmittedAfterHalt,      // a later order was accepted by a halted engine
    kCancelAfterHalt,        // a later cancel was accepted by a halted engine
    kCheckpointRan,          // a checkpoint rewrote the journal while halted
    kNotHalted,              // the async worker's failed commit did not halt the journal
};

const char* describe(int code) {
    switch (code) {
    case kOk:                return "ok";
    case kSetrlimitFailed:   return "setrlimit failed";
    case kAcceptedUndurable: return "the order whose commit failed was reported ACCEPTED";
    case kWrongReason:       return "rejected with a reason other than JournalHalted";
    case kEventLeaked:       return "the client was told about an order the disk refused";
    case kAdmittedAfterHalt: return "a halted engine accepted a new order";
    case kCancelAfterHalt:   return "a halted engine accepted a cancel";
    case kCheckpointRan:     return "a checkpoint ran on a halted journal";
    case kNotHalted:         return "the journal did not halt";
    default:                 return "?";
    }
}

struct ClientView : EventListener {
    std::vector<OrderUpdate> updates;
    void onTrade(const Trade&) override {}
    void onOrderUpdate(const OrderUpdate& u) override { updates.push_back(u); }
    void onMarketData(const MarketDataUpdate&) override {}
    void onBookVisible(const BookVisibleUpdate&) override {}
    bool heardAbout(OrderId id) const {
        for (const auto& u : updates)
            if (u.orderId == id) return true;
        return false;
    }
};

bool limitFileSize(rlim_t bytes) {
    std::signal(SIGXFSZ, SIG_IGN);   // EFBIG instead of death
    struct rlimit rl{bytes, bytes};
    return setrlimit(RLIMIT_FSIZE, &rl) == 0;
}

// The limit lands halfway through the fourth order's record, behind the
// sequence-id lease the engine journals before its first id (D1).
rlim_t midFourthRecord() {
    constexpr int kLeaseRecords = 1;
    return sizeof(JournalFileHeader) +
           (kLeaseRecords + kWholeBeforeLimit) * sizeof(JournalEntry) +
           sizeof(JournalEntry) / 2;
}

// Orders 1..maxId a restart restored, through the replay every binary runs.
size_t restoredFrom(const std::string& path, OrderId maxId) {
    MatchingEngine restarted;
    restarted.addSymbol(kSym);
    restarted.enableJournal(path);
    restarted.replayJournal();
    size_t n = 0;
    for (OrderId id = 1; id <= maxId; ++id)
        if (restarted.getOrderBook(kSym)->getOrder(id)) ++n;
    return n;
}

SubmitResult restingBuy(MatchingEngine& e, OrderId id) {
    return e.submitOrder(kSym, id, kOwner, Side::Buy, 10000 - static_cast<Price>(id), 10,
                         OrderType::Limit);
}

// Run `body` in a forked child and return how it ended, printed.
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

// Sync engine, as GatewayServer runs it: Immediate, durable acks on.
void testSyncPathRejectsAndHolds() {
    const std::string path = uniqueTempPath("journal_halt_sync.wal");
    std::filesystem::remove(path);
    const int rc = inChild("sync", [&]() -> int {
        if (!limitFileSize(midFourthRecord())) return kSetrlimitFailed;
        MatchingEngine engine;
        ClientView client;
        engine.addSymbol(kSym);
        engine.getOrderBook(kSym)->setEventListener(&client);
        engine.enableJournal(path, Journal::SyncPolicy::Immediate);
        engine.start();
        if (!engine.enableDurableClientAcks(true)) return kWrongReason;

        for (OrderId id = 1; id <= kWholeBeforeLimit; ++id)
            if (!restingBuy(engine, id).isAccepted()) return kWrongReason;

        // The fourth commit hits the limit mid-record.
        const SubmitResult failed = restingBuy(engine, 4);
        if (failed.isAccepted()) return kAcceptedUndurable;
        if (failed.rejectReason != RejectReason::JournalHalted) return kWrongReason;
        if (client.heardAbout(4)) return kEventLeaked;

        const SubmitResult later = restingBuy(engine, 5);
        if (later.isAccepted()) return kAdmittedAfterHalt;
        if (later.rejectReason != RejectReason::JournalHalted) return kWrongReason;

        const SubmitResult cancel = engine.submitCancel(kSym, 1);
        if (cancel.isAccepted()) return kCancelAfterHalt;
        if (cancel.rejectReason != RejectReason::JournalHalted) return kWrongReason;

        const auto before = std::filesystem::file_size(path);
        engine.checkpoint();
        if (std::filesystem::file_size(path) != before) return kCheckpointRan;
        return kOk;
    });
    // A restart replays exactly the three orders the disk accepted.
    const size_t resting = restoredFrom(path, 10);
    std::filesystem::remove(path);
    std::printf("  sync: %zu resting after restart\n", resting);
    assert(rc == kOk && "sync engine on a failed journal write");
    assert(resting == static_cast<size_t>(kWholeBeforeLimit));
}

// Async engine: the failure happens on a worker; admission must notice.
void testAsyncAdmissionRejects() {
    const std::string path = uniqueTempPath("journal_halt_async.wal");
    std::filesystem::remove(path);
    const int rc = inChild("async", [&]() -> int {
        if (!limitFileSize(midFourthRecord())) return kSetrlimitFailed;
        MatchingEngine engine;
        engine.addSymbol(kSym);
        engine.enableJournal(path, Journal::SyncPolicy::Immediate);
        engine.startAsync(1);
        for (OrderId id = 1; id <= kWholeBeforeLimit + 1; ++id) restingBuy(engine, id);
        engine.waitForDrain();
        if (!engine.getJournal()->halted()) return kNotHalted;

        const SubmitResult later = restingBuy(engine, 9);
        if (later.isAccepted()) return kAdmittedAfterHalt;
        if (later.rejectReason != RejectReason::JournalHalted) return kWrongReason;
        return kOk;
    });
    std::filesystem::remove(path);
    assert(rc == kOk && "async engine on a failed journal write");
}

// h3: a checkpoint under RLIMIT_FSIZE=2000 against a 30-order book. The temp
// file fails; the live journal must be left exactly as it was.
void testCheckpointFailureKeepsTheJournal() {
    constexpr int kOrders = 30;
    const std::string path = uniqueTempPath("journal_halt_ckpt.wal");
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".tmp");
    const int rc = inChild("checkpoint", [&]() -> int {
        MatchingEngine engine;
        engine.addSymbol(kSym);
        engine.enableJournal(path);
        engine.start();
        for (OrderId id = 1; id <= kOrders; ++id) restingBuy(engine, id);
        engine.getJournal()->flush();
        if (!limitFileSize(2000)) return kSetrlimitFailed;
        engine.checkpoint();
        return kOk;
    });
    const size_t resting = restoredFrom(path, kOrders);
    const bool tmpLeft = std::filesystem::exists(path + ".tmp");
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".tmp");
    std::printf("  checkpoint: %zu of %d resting after restart\n", resting, kOrders);
    assert(rc == kOk && "a failed checkpoint must not stop the process");
    assert(resting == static_cast<size_t>(kOrders) &&
           "a failed checkpoint replaced the live journal");
    assert(!tmpLeft && "a failed checkpoint left its temp file behind");
}

// D5 (JRN-9): a checkpoint's rename is a change to the DIRECTORY, durable only
// once the directory is synced; until then a power cut can bring the old file
// back and drop everything appended to the new one. A directory this process
// may write but not open (0300) makes that sync fail for real. The checkpoint
// must halt the journal rather than carry on behind a rename it cannot keep.
void testCheckpointDirectorySyncFailureHalts() {
    if (::geteuid() == 0) {
        std::puts("  directory sync: SKIP (root opens a 0300 directory)");
        return;
    }
    constexpr int kOrders = 5;
    const std::string dir = uniqueTempPath("journal_halt_dirsync");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);
    const std::string path = dir + "/live.wal";
    bool halted = false;
    RejectReason next = RejectReason::None;
    {
        MatchingEngine engine;
        engine.addSymbol(kSym);
        engine.enableJournal(path);
        engine.start();
        for (OrderId id = 1; id <= kOrders; ++id) restingBuy(engine, id);
        engine.getJournal()->flush();
        assert(::chmod(dir.c_str(), 0300) == 0);
        engine.checkpoint();
        halted = engine.getJournal()->halted();
        next = restingBuy(engine, 99).rejectReason;
        assert(::chmod(dir.c_str(), 0700) == 0);
        engine.stop();
    }
    const size_t resting = restoredFrom(path, kOrders);
    std::filesystem::remove_all(dir);
    std::printf("  directory sync: halted=%d, next order rejected %s, %zu of %d resting "
                "after restart\n", halted ? 1 : 0,
                next == RejectReason::JournalHalted ? "JournalHalted" : "otherwise",
                resting, kOrders);
    assert(halted && "the checkpoint renamed over the journal without syncing the directory");
    assert(next == RejectReason::JournalHalted);
    assert(resting == static_cast<size_t>(kOrders));
}

}  // namespace

int main() {
    testSyncPathRejectsAndHolds();
    testAsyncAdmissionRejects();
    testCheckpointFailureKeepsTheJournal();
    testCheckpointDirectorySyncFailureHalts();
    std::puts("JournalHaltTest passed");
    return 0;
}
