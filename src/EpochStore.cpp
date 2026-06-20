// EpochStore — see include/EpochStore.h for the rationale and on-disk
// format. This translation unit holds the POSIX-specific durability path so
// the header stays free of platform plumbing.

#include "EpochStore.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace OrderMatcher {

namespace {

// Force the bytes already written to `fd` all the way to stable storage.
// On Apple, plain fsync() only pushes data to the drive's volatile cache;
// F_FULLFSYNC additionally tells the drive to flush that cache, which is
// what actually guarantees crash survival. On Linux, fdatasync() is the
// right barrier (we don't care about metadata-only timestamp updates). This
// mirrors Journal::syncFile() so both durable writers behave identically
// per platform.
void syncFd(int fd) {
#ifdef __APPLE__
    ::fcntl(fd, F_FULLFSYNC);
#else
    ::fdatasync(fd);
#endif
}

}  // namespace

EpochStore::EpochStore(std::string path) : path_(std::move(path)) {}

void EpochStore::store(uint64_t epoch) {
    // Write to a temp sibling first, fsync it, then atomically rename over
    // the real path. The rename guarantees a reader never observes a
    // partially written file; the pre-rename fsync guarantees the new bytes
    // are durable before they become visible.
    const std::string tmpPath = path_ + ".tmp";

    // 0644: owner read/write, group/other read — same posture as an
    // ordinary data file the operator may inspect.
    int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return;  // best-effort: nothing durable to do if we can't open
    }

    // Serialize as the 8 raw host-order bytes. A loop guards against short
    // writes (a single ::write may persist fewer bytes than requested).
    uint8_t buf[sizeof(uint64_t)];
    std::memcpy(buf, &epoch, sizeof(buf));

    size_t total = 0;
    bool writeOk = true;
    while (total < sizeof(buf)) {
        ssize_t n = ::write(fd, buf + total, sizeof(buf) - total);
        if (n <= 0) {
            writeOk = false;
            break;
        }
        total += static_cast<size_t>(n);
    }

    if (writeOk) {
        // Durability barrier MUST precede the rename: the new value has to
        // be on stable storage before it can replace the old one.
        syncFd(fd);
    }
    ::close(fd);

    if (!writeOk) {
        ::unlink(tmpPath.c_str());  // don't leave a torn temp behind
        return;
    }

    // Atomic publish. If the rename fails, the previous file (if any) is
    // left intact and the temp is removed, so we never regress to a
    // half-written state.
    if (::rename(tmpPath.c_str(), path_.c_str()) != 0) {
        ::unlink(tmpPath.c_str());
        return;
    }

    // Best-effort: also flush the directory entry so the rename itself
    // survives a crash. Failure here is non-fatal (and unsupported on some
    // filesystems), so we ignore errors.
    std::string dir = path_;
    const size_t slash = dir.find_last_of('/');
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
    int dirFd = ::open(dir.c_str(), O_RDONLY);
    if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }
}

uint64_t EpochStore::load() const {
    int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
        return 0;  // missing file → fresh node at epoch 0
    }

    uint8_t buf[sizeof(uint64_t)];
    size_t total = 0;
    bool readErr = false;
    while (total < sizeof(buf)) {
        ssize_t n = ::read(fd, buf + total, sizeof(buf) - total);
        if (n < 0) {
            readErr = true;  // genuine I/O error
            break;
        }
        if (n == 0) {
            break;  // EOF — file shorter than 8 bytes (empty or torn)
        }
        total += static_cast<size_t>(n);
    }

    // Reject anything that isn't exactly one complete 8-byte record:
    // short/truncated/garbage files, and any trailing bytes beyond the
    // record, all decode to epoch 0 rather than a bogus value. We probe one
    // extra byte to detect an over-long file.
    bool exactSize = !readErr && total == sizeof(buf);
    if (exactSize) {
        uint8_t extra;
        if (::read(fd, &extra, 1) > 0) {
            exactSize = false;  // longer than 8 bytes → treat as corrupt
        }
    }
    ::close(fd);

    if (!exactSize) {
        return 0;
    }

    uint64_t epoch = 0;
    std::memcpy(&epoch, buf, sizeof(epoch));
    return epoch;
}

const std::string& EpochStore::path() const { return path_; }

}  // namespace OrderMatcher
