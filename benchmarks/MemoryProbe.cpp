// MemoryProbe — the sizes and resident memory docs/CapacityPlanning.md sizes
// deployments from (roadmap 1.11-K5, PERF-4).
//
// Prints the struct sizes the per-order and journal tables quote, then the
// resident set after constructing a MatchingEngine and after adding `symbols`
// empty books at the default configuration. A book is allocated in full when
// its symbol is added, so the per-symbol figure is what a deployment pays per
// instrument before any order arrives.
//
// Ported from the audit's work/perf/mem.cpp (macOS-only, never committed).
// RSS counts touched pages only, so it is a floor on what the allocations cost.
//
// Usage: MemoryProbe [--symbols N]   (default 50)

#include "Journal.h"
#include "MatchingEngine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

using namespace OrderMatcher;

static double rssMiB() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        std::fprintf(stderr, "task_info failed\n");
        std::exit(1);
    }
    return static_cast<double>(info.resident_size) / 1048576.0;
#else
    unsigned long sizePages = 0, residentPages = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f || std::fscanf(f, "%lu %lu", &sizePages, &residentPages) != 2) {
        std::fprintf(stderr, "cannot read /proc/self/statm\n");
        std::exit(1);
    }
    std::fclose(f);
    return static_cast<double>(residentPages) * static_cast<double>(sysconf(_SC_PAGESIZE)) /
           1048576.0;
#endif
}

int main(int argc, char* argv[]) {
    int symbols = 50;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) {
            symbols = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: MemoryProbe [--symbols N]\n");
            return 2;
        }
    }
    if (symbols < 1) {
        std::fprintf(stderr, "--symbols must be at least 1\n");
        return 2;
    }

    std::printf("sizeof(Order)=%zu sizeof(JournalEntry)=%zu sizeof(OrderRequest)=%zu\n",
                sizeof(Order), sizeof(JournalEntry), sizeof(OrderRequest));
    const double r0 = rssMiB();
    auto engine = std::make_unique<MatchingEngine>();
    const double r1 = rssMiB();
    for (int s = 0; s < symbols; ++s) engine->addSymbol(static_cast<SymbolId>(s));
    const double r2 = rssMiB();
    std::printf("RSS: engine ctor %.1f MiB; %d empty symbols %.1f MiB => %.2f MiB/symbol\n",
                r1 - r0, symbols, r2 - r1, (r2 - r1) / symbols);
    return 0;
}
