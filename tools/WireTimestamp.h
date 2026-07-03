#pragma once
//
// WireTimestamp.h — shared timestamping helpers for the OUCH wire-latency
// harness (WireLatencySender / WireLatencyReceiver).
//
// Linux: hardware NIC timestamps via SO_TIMESTAMPING.
//   * RX timestamps ride the ancillary cmsg on a normal recvmsg().
//   * TX timestamps are delivered on the socket ERROR QUEUE — you must
//     recvmsg(fd, ..., MSG_ERRQUEUE) to collect them (asynchronous: the ack
//     may arrive before the TX timestamp is queued, so collect TX *after*
//     the round-trip barrier).
//   * scm_timestamping.ts[2] = hardware (raw NIC clock), ts[0] = software.
//
// Non-Linux (macOS/BSD): SO_TIMESTAMPING does not exist. Everything below the
// #ifdef falls back to clock_gettime(CLOCK_MONOTONIC) userspace timestamps so
// the tools still build and run for a software smoke test. CLOCK_MONOTONIC
// (not REALTIME) because we only ever take *differences* on one host and it is
// immune to NTP steps.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#endif

namespace wirelat {

// Monotonic userspace clock in nanoseconds. Software-fallback timestamp source.
inline uint64_t softwareNowNs() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Enable SO_TIMESTAMPING (HW TX/RX + software fallback) on `fd`. Returns true
// if hardware timestamping was successfully requested; false on non-Linux or
// setsockopt failure (caller then uses the software path).
inline bool enableTimestamping(int fd) {
#if defined(__linux__)
    int flags = SOF_TIMESTAMPING_TX_HARDWARE |
                SOF_TIMESTAMPING_RX_HARDWARE |
                SOF_TIMESTAMPING_SOFTWARE |     // fallback if HW not available
                SOF_TIMESTAMPING_OPT_CMSG |
                SOF_TIMESTAMPING_OPT_TSONLY;    // errqueue carries ts only, no payload echo
    return ::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) == 0;
#else
    (void)fd;
    return false;
#endif
}

#if defined(__linux__)
// Pull a nanosecond timestamp out of a SO_TIMESTAMPING cmsg on `msg`. Prefers
// the hardware clock (ts[2]); falls back to the software clock (ts[0]). Sets
// `outHw` accordingly. Returns false if no SO_TIMESTAMPING cmsg is present.
inline bool extractTimestamp(struct msghdr& msg, uint64_t& outNs, bool& outHw) {
    for (struct cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm != nullptr;
         cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SO_TIMESTAMPING) {
            const struct scm_timestamping* t =
                reinterpret_cast<const struct scm_timestamping*>(CMSG_DATA(cm));
            const struct timespec& hw = t->ts[2];
            const struct timespec& sw = t->ts[0];
            const bool haveHw = (hw.tv_sec != 0 || hw.tv_nsec != 0);
            const struct timespec& s = haveHw ? hw : sw;
            outHw = haveHw;
            outNs = static_cast<uint64_t>(s.tv_sec) * 1'000'000'000ull +
                    static_cast<uint64_t>(s.tv_nsec);
            return true;
        }
    }
    return false;
}
#endif

// Receive exactly `frameSize` bytes into `buf`, capturing the RX timestamp of
// the first segment that arrives. Returns false on EOF/error. On the software
// path (or if no HW cmsg is present), the timestamp is softwareNowNs() taken
// when the first bytes land.
inline bool recvFrameWithTs(int fd, uint8_t* buf, size_t frameSize,
                            uint64_t& tsNs, bool& hw) {
    size_t got = 0;
    bool haveTs = false;
    while (got < frameSize) {
#if defined(__linux__)
        char cbuf[CMSG_SPACE(sizeof(struct scm_timestamping))];
        struct iovec iov;
        iov.iov_base = buf + got;
        iov.iov_len = frameSize - got;
        struct msghdr msg;
        std::memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        ssize_t n = ::recvmsg(fd, &msg, 0);
        if (n <= 0) return false;
        if (!haveTs) {
            uint64_t t = 0;
            bool h = false;
            if (extractTimestamp(msg, t, h)) {
                tsNs = t;
                hw = h;
                haveTs = true;
            }
        }
        got += static_cast<size_t>(n);
#else
        ssize_t n = ::recv(fd, buf + got, frameSize - got, 0);
        if (n <= 0) return false;
        if (!haveTs) {
            tsNs = softwareNowNs();
            hw = false;
            haveTs = true;
        }
        got += static_cast<size_t>(n);
#endif
    }
    if (!haveTs) {  // HW cmsg missing on every segment — fall back to software
        tsNs = softwareNowNs();
        hw = false;
    }
    return true;
}

// After a send(), collect the TX timestamp from the socket error queue. Polls
// up to `timeoutMs` for the errqueue message. Returns true if a TX timestamp
// was obtained (sets tsNs/hw). On non-Linux, or if none arrives in time,
// returns false — the caller then uses the software send-time it captured.
inline bool collectTxTimestamp(int fd, uint64_t& tsNs, bool& hw, int timeoutMs) {
#if defined(__linux__)
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLERR;  // errqueue readiness surfaces as POLLERR in revents
    pfd.revents = 0;
    int pr = ::poll(&pfd, 1, timeoutMs);
    if (pr <= 0) return false;

    char cbuf[CMSG_SPACE(sizeof(struct scm_timestamping))];
    struct msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);  // OPT_TSONLY → no iovec needed
    ssize_t n = ::recvmsg(fd, &msg, MSG_ERRQUEUE);
    if (n < 0) return false;
    return extractTimestamp(msg, tsNs, hw);
#else
    (void)fd;
    (void)tsNs;
    (void)hw;
    (void)timeoutMs;
    return false;
#endif
}

// One-line platform banner printed at tool startup so the log is unambiguous
// about whether the numbers below are hardware or software timestamps.
inline void printTimestampBanner(bool hwEnabled) {
#if defined(__linux__)
    if (hwEnabled) {
        std::printf("[wire-latency] SO_TIMESTAMPING enabled "
                    "(hardware TX/RX, software fallback per-message)\n");
    } else {
        std::printf("[wire-latency] SO_TIMESTAMPING setsockopt failed — "
                    "falling back to clock_gettime(CLOCK_MONOTONIC) "
                    "software timestamps\n");
    }
#else
    (void)hwEnabled;
    std::printf("[wire-latency] hardware timestamping not available on this "
                "platform (Linux SO_TIMESTAMPING required) — falling back to "
                "clock_gettime(CLOCK_MONOTONIC) software timestamps\n");
#endif
}

}  // namespace wirelat
