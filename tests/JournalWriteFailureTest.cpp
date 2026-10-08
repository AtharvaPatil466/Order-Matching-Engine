// JournalWriteFailureTest — the journal must never report an entry durable that
// the disk did not accept, and must stop writing once the disk has refused one.
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
// Continuing to write after the failure is not safe either. Both commit paths
// APPEND: a failed write can leave a torn record at EOF, and anything appended
// after it is lost on replay, because recovery stops at the first record whose
// CRC does not check. And DurabilityGate releases events against a single FIFO
// count of committed entries, so a later successful commit would advance that
// count across the failed batch and release acks for exactly the orders that
// were dropped.
//
// That used to be answered by aborting the process (3c4322a). Roadmap 1.4-D4
// replaces the abort with a HALTED journal: it cuts what the failed commit put
// down, keeps the batch in memory, writes nothing more, and never reports it
// durable; the engine rejects new orders and a restart replays the file.
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
//   2. The outcome: the process keeps running with the journal halted, the
//      unwritten entries still held, and nothing after the failure on disk —
//      not even the half record the failed write left, and not on the way out.

#include "Journal.h"
#include "TempPath.h"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

constexpr int kEntries           = 10;
constexpr int kWholeBeforeLimit  = 3;    // the limit admits exactly this many records
constexpr int kSetrlimitFailed   = 90;   // child exit codes: the test could not arm itself,
constexpr int kNotHalted         = 91;   //   the write failed and the journal carried on,
constexpr int kBatchNotKept      = 92;   //   or it dropped the entries it could not write

[[noreturn]] void runChild(const std::string& path, int reportFd) {
    std::signal(SIGXFSZ, SIG_IGN);   // EFBIG instead of death
    const rlim_t limit = sizeof(JournalFileHeader)
                       + kWholeBeforeLimit * sizeof(JournalEntry)
                       + sizeof(JournalEntry) / 2;          // mid-record
    struct rlimit rl{limit, limit};
    if (setrlimit(RLIMIT_FSIZE, &rl) != 0) _exit(kSetrlimitFailed);

    // Immediate: one commit per entry, so the first failing commit is the
    // fourth one and the expected counts are exact.
    int code = 0;
    {
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
        if (!journal.halted())
            code = kNotHalted;
        else if (journal.pendingEntries() != static_cast<size_t>(kEntries - kWholeBeforeLimit))
            code = kBatchNotKept;
    }   // ~Journal flushes and closes: a halted one must write nothing on the way out
    _exit(code);
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
    const auto fileBytes = std::filesystem::file_size(path);
    std::remove(path.c_str());

    std::printf("reported durable: %llu   intact on disk: %zu   file bytes: %llu   child: %s %d\n",
                static_cast<unsigned long long>(maxReported), onDisk,
                static_cast<unsigned long long>(fileBytes),
                WIFSIGNALED(status) ? "signal" : "exit",
                WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));

    assert(!(WIFEXITED(status) && WEXITSTATUS(status) == kSetrlimitFailed) &&
           "setrlimit failed, so the write failure was never armed");
    assert(onDisk == static_cast<size_t>(kWholeBeforeLimit) &&
           "the file-size limit did not land where this test expects");

    assert(maxReported <= onDisk &&
           "the journal reported entries DURABLE that are not intact on disk — a "
           "durable ack built on this confirms orders the disk dropped");
    assert(WIFEXITED(status) &&
           "a journal write failure must halt the journal, not kill the process");
    assert(WEXITSTATUS(status) != kNotHalted &&
           "the write failed and the journal did not halt: the next commit would "
           "append behind a possibly-torn record");
    assert(WEXITSTATUS(status) != kBatchNotKept &&
           "the halted journal dropped the entries it could not write");
    assert(WEXITSTATUS(status) == 0);
    assert(fileBytes == sizeof(JournalFileHeader) + kWholeBeforeLimit * sizeof(JournalEntry) &&
           "the failed commit's partial record is still in the file — it must be cut, "
           "so the file holds exactly what was reported durable");

    std::puts("JournalWriteFailureTest passed");
    return 0;
}
