// JournalWriteFailureTest — the journal must never report an entry durable that
// the disk did not accept.
//
// WHAT WAS WRONG (JRN-4). writeBatch did
//
//     size_t w = std::fwrite(batch_.data(), sizeof(JournalEntry), toWrite, file_);
//     std::fflush(file_);
//     return w;
//
// fwrite reports what it copied into the stdio BUFFER, not what reached the
// file, and the fflush result was discarded. On ENOSPC or EFBIG the flush fails,
// yet the commit counted the whole batch as written; fdatasync then succeeds —
// the lost bytes never reached the kernel, so there is nothing for it to fail on
// — and onDurable fires for entries that are not in the file. A durable client
// ack built on that confirms orders a full disk just dropped.
//
// Continuing after the failure is not safe either, which is why the fix is to
// stop. Both commit paths APPEND. A failed flush can leave a torn record at EOF,
// and anything appended after it is lost on replay, because recovery stops at
// the first record whose CRC does not check. And DurabilityGate releases events
// against a single FIFO count of committed entries, so a later successful
// commit would advance that count across the failed batch and release acks for
// exactly the orders that were dropped.
//
// HOW THE FAILURE IS PRODUCED. For real, without mocks: a child process sets
// RLIMIT_FSIZE so the limit lands halfway through the fourth record, and ignores
// SIGXFSZ so the kernel returns EFBIG instead of killing it. The fault-injection
// points cannot do this — they model a benign short write and a skipped fsync,
// which are retried and withheld correctly, and four chaos suites pin that
// behaviour. This test is about the real error the injectors never produce.
//
// WHAT IS PINNED.
//   1. The invariant: the number of entries onDurable has reported never exceeds
//      the number intact on disk. Every durability callback is sent to the
//      parent through a pipe, which RLIMIT_FSIZE does not apply to, so the count
//      survives however the child ends.
//   2. The outcome: the child is stopped by SIGABRT. A journal that can no
//      longer tell what is on disk must not keep accepting work.

#include "Journal.h"
#include "TempPath.h"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr int kEntries           = 10;
constexpr int kWholeBeforeLimit  = 3;    // the limit admits exactly this many records
constexpr int kSetrlimitFailed   = 90;   // child exit code: the test could not arm itself

[[noreturn]] void runChild(const std::string& path, int reportFd) {
    std::signal(SIGXFSZ, SIG_IGN);   // EFBIG instead of death
    const rlim_t limit = sizeof(JournalFileHeader)
                       + kWholeBeforeLimit * sizeof(JournalEntry)
                       + sizeof(JournalEntry) / 2;          // mid-record
    struct rlimit rl{limit, limit};
    if (setrlimit(RLIMIT_FSIZE, &rl) != 0) _exit(kSetrlimitFailed);

    // Immediate: one commit per entry, so the first failing commit is the
    // fourth one and the expected counts are exact.
    Journal journal(path, Journal::SyncPolicy::Immediate, 1);
    uint64_t reported = 0;
    journal.setOnDurable([&](size_t n) {
        reported += n;
        (void)!write(reportFd, &reported, sizeof reported);
    });
    for (int i = 1; i <= kEntries; ++i)
        journal.logAddOrder(static_cast<OrderId>(i), 7, 1, Side::Buy, 10000, 10,
                            OrderType::Limit);
    journal.flush();
    _exit(0);
}

std::string tempJournalPath() {
    return uniqueTempPath("ob_journal_write_failure.wal");
}

}  // namespace

int main() {
    const std::string path = tempJournalPath();
    std::remove(path.c_str());

    int fds[2];
    assert(pipe(fds) == 0);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        runChild(path, fds[1]);
    }
    close(fds[1]);

    uint64_t value = 0, maxReported = 0;
    while (read(fds[0], &value, sizeof value) == static_cast<ssize_t>(sizeof value))
        maxReported = value;
    close(fds[0]);

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);

    const size_t onDisk = Journal(path).readAll(true, true).size();
    std::remove(path.c_str());

    std::printf("reported durable: %llu   intact on disk: %zu   child: %s %d\n",
                static_cast<unsigned long long>(maxReported), onDisk,
                WIFSIGNALED(status) ? "signal" : "exit",
                WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));

    assert(!(WIFEXITED(status) && WEXITSTATUS(status) == kSetrlimitFailed) &&
           "setrlimit failed, so the write failure was never armed");
    assert(onDisk == static_cast<size_t>(kWholeBeforeLimit) &&
           "the file-size limit did not land where this test expects");

    assert(maxReported <= onDisk &&
           "the journal reported entries DURABLE that are not intact on disk — a "
           "durable ack built on this confirms orders the disk dropped");
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
           "a journal write failure must stop the process: continuing appends after "
           "a possibly-torn record and advances the gate's FIFO count past the "
           "failed batch");

    std::puts("JournalWriteFailureTest passed");
    return 0;
}
