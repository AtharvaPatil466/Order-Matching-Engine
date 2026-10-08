// JournalSubmitFailureTest — a refused io_uring_submit must halt the journal,
// not wedge its writer (JRN-11, roadmap 1.4-D4).
//
// commitBatchAsync set inflightCount_ = 1 and then called io_uring_submit with
// its return value ignored. When the kernel refuses the submission, no CQE ever
// comes, the reaper never clears inflightCount_, and the next commit — or the
// Immediate path's own wait — blocks on chainDone_ forever. The prepared SQEs
// also stay in the ring, where any later submit (the shutdown NOP) would put
// them on the disk behind whatever had happened since.
//
// The refusal is produced with the "journal.uring.submit_fail" fault point, so
// this needs the fault-injection build on Linux with liburing, and io_uring
// allowed at runtime (Docker's default seccomp forbids it). Elsewhere it skips.

#include "Journal.h"
#include "TempPath.h"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <string>
#include <unistd.h>

#if defined(__linux__) && defined(OB_HAVE_LIBURING) && defined(OB_ENABLE_FAULT_INJECTION)
#include <liburing.h>
#define OB_SUBMIT_TEST 1
#endif

using namespace OrderMatcher;

#ifdef OB_SUBMIT_TEST
namespace {

void wedged(int) {
    static const char msg[] =
        "FAIL: the writer is blocked on a chain the kernel never accepted "
        "(io_uring_submit's refusal was ignored)\n";
    (void)!write(2, msg, sizeof msg - 1);
    _exit(1);
}

bool ioUringUsable() {
    struct io_uring ring;
    if (io_uring_queue_init(8, &ring, 0) != 0) return false;
    io_uring_queue_exit(&ring);
    return true;
}

void add(Journal& j, OrderId id) {
    j.logAddOrder(id, 7, 1, Side::Buy, 10000, 10, OrderType::Limit);
}

}  // namespace
#endif

int main() {
#ifndef OB_SUBMIT_TEST
    std::puts("JournalSubmitFailureTest: SKIP (needs ENABLE_FAULT_INJECTION on Linux with liburing)");
    return 0;
#else
    if (!ioUringUsable()) {
        std::puts("JournalSubmitFailureTest: SKIP (io_uring is not allowed at runtime here)");
        return 0;
    }
    std::signal(SIGALRM, wedged);
    alarm(20);

    const std::string path = uniqueTempPath("journal_submit_failure.wal");
    std::remove(path.c_str());
    auto& fi = FaultInjector::instance();
    fi.reset();
    {
        Journal j(path, Journal::SyncPolicy::Immediate, 1);
        size_t durable = 0;
        j.setOnDurable([&](size_t n) { durable += n; });

        add(j, 1);
        assert(durable == 1 && !j.halted() && "a plain commit before the fault");

        fi.arm("journal.uring.submit_fail", 1.0);
        add(j, 2);           // pre-fix: blocks here, on its own chain
        fi.disarm();
        std::printf("after the refused submit: halted=%d durable=%zu\n", j.halted(), durable);
        assert(j.halted() && "a refused submit must halt the journal");
        assert(durable == 1 && "an entry the kernel never took was reported durable");

        add(j, 3);           // halted: held in memory, never written
        j.flush();
        assert(j.pendingEntries() == 2 && "the halted journal must keep both entries");
    }   // shutdown submits its NOP; the neutered SQEs must not write anything

    const size_t onDisk = Journal(path).readAll(true, true).size();
    std::remove(path.c_str());
    std::printf("on disk after shutdown: %zu\n", onDisk);
    assert(onDisk == 1 && "an entry whose submit was refused reached the disk after the halt");
    std::puts("JournalSubmitFailureTest passed");
    return 0;
#endif
}
