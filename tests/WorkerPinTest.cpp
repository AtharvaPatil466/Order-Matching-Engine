// WorkerPinTest — async workers are pinned inside the CPUs the process may use.
//
// Each worker pinned itself to CPU <its index>, whatever the process was
// allowed to run on. Under `taskset -c 2-5 OrderEngine --threads 4`
// (docs/CapacityPlanning.md) the workers ran on CPUs 0-3 — worker 0 on the
// housekeeping core docs/OSTuning.md keeps clear — and two engines, or an
// engine and any other process, shared worker cores whatever their masks said.
//
// The workers are the threads startAsync() creates (/proc/self/task before and
// after). A worker pins itself first thing, so the test polls until two of the
// new threads hold a one-CPU mask.
//   * unrestricted: worker i is on the i-th allowed CPU — CPU i on a box that
//     allows every CPU, as before;
//   * restricted to the last two allowed CPUs, as `taskset -c` does: the
//     workers are on those two, and no thread the engine started may run
//     anywhere else;
//   * setWorkerCores (the worker_cores key) puts them where it says, and a
//     CPU outside the mask makes startAsync throw, starting nothing.
// Linux only: macOS has no CPU affinity mask (exit 77, skipped).

#include "MatchingEngine.h"

#include <cstdio>

#if defined(__linux__)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <sched.h>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

using namespace OrderMatcher;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::vector<int> affinityOf(pid_t tid) {
    cpu_set_t set;
    CPU_ZERO(&set);
    std::vector<int> cpus;
    if (sched_getaffinity(tid, sizeof set, &set) != 0) return cpus;
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (CPU_ISSET(c, &set)) cpus.push_back(c);
    }
    return cpus;
}

void restrictTo(const std::vector<int>& cpus) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus) CPU_SET(c, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) {
        std::perror("sched_setaffinity");
        std::exit(1);
    }
}

std::set<pid_t> threadIds() {
    std::set<pid_t> ids;
    if (DIR* dir = opendir("/proc/self/task")) {
        while (const dirent* e = readdir(dir)) {
            if (e->d_name[0] != '.') ids.insert(static_cast<pid_t>(std::atoi(e->d_name)));
        }
        closedir(dir);
    }
    return ids;
}

std::string str(const std::vector<int>& cpus) {
    std::string s = "{";
    for (size_t i = 0; i < cpus.size(); ++i) s += (i ? "," : "") + std::to_string(cpus[i]);
    return s + "}";
}

// The mask of every thread a 2-worker engine starts on the calling thread's
// current mask, once both workers have pinned (or after 10 s, as they are).
std::vector<std::vector<int>> engineThreadMasks(const std::vector<int>& workerCores) {
    const std::set<pid_t> before = threadIds();
    MatchingEngine engine;
    engine.addSymbol(1);
    engine.setWorkerCores(workerCores);
    engine.startAsync(2);
    std::vector<std::vector<int>> masks;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        masks.clear();
        size_t pinned = 0;
        for (pid_t tid : threadIds()) {
            if (before.count(tid)) continue;
            masks.push_back(affinityOf(tid));
            pinned += masks.back().size() == 1;
        }
        if (pinned >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    engine.stopAsync();
    return masks;
}

// Exactly the two workers sit on one CPU each, on `first` and `second`, and no
// engine thread may run outside `allowed` (sorted).
void expectWorkersOn(const char* label, const std::vector<int>& allowed, int first, int second,
                     const std::vector<int>& workerCores = {}) {
    const std::vector<std::vector<int>> masks = engineThreadMasks(workerCores);
    std::vector<int> pinnedTo;
    bool inside = true;
    std::string seen;
    for (const auto& m : masks) {
        if (m.size() == 1) pinnedTo.push_back(m[0]);
        for (int c : m) inside = inside && std::binary_search(allowed.begin(), allowed.end(), c);
        seen += " " + str(m);
    }
    std::sort(pinnedTo.begin(), pinnedTo.end());
    std::printf("%s: allowed %s, engine threads:%s\n", label, str(allowed).c_str(), seen.c_str());
    check(pinnedTo == std::vector<int>{first, second},
          std::string(label) + ": workers pinned to " + str(pinnedTo) + ", want " +
              str({first, second}));
    check(inside, std::string(label) + ": every engine thread stays inside " + str(allowed));
}

}  // namespace

int main() {
    const std::vector<int> all = affinityOf(0);
    if (all.size() < 3) {
        std::printf("SKIP: needs 3 allowed CPUs, this process has %s\n", str(all).c_str());
        return 77;
    }

    expectWorkersOn("unrestricted", all, all[0], all[1]);

    const std::vector<int> lastTwo = {all[all.size() - 2], all.back()};
    restrictTo(lastTwo);
    expectWorkersOn("taskset to the last two CPUs", lastTwo, lastTwo[0], lastTwo[1]);
    restrictTo(all);

    expectWorkersOn("worker_cores", all, all[1], all.back(), {all.back(), all[1]});

    MatchingEngine engine;
    engine.addSymbol(1);
    engine.setWorkerCores({all.back() + 1, all[0]});
    const std::set<pid_t> before = threadIds();
    bool threw = false;
    try {
        engine.startAsync(2);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::printf("worker_cores outside the mask: %s\n", e.what());
    }
    check(threw && threadIds() == before,
          "worker_cores outside the mask: startAsync throws and starts no thread");

    if (failures) {
        std::printf("FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS: workers are pinned inside the CPUs the process may use\n");
    return 0;
}

#else

int main() {
    std::printf("SKIP: no CPU affinity mask on this platform\n");
    return 77;
}

#endif
