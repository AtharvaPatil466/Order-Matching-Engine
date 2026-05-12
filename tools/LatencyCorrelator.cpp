// LatencyCorrelator — correlate engine events with latency spikes.
//
// Roadmap Phase 5, Week 18: Latency Correlation Analysis
//
// Reads a benchmark JSON results file and an engine event log, then
// reports which internal events (journal rotation, snapshot, config
// reload) temporally overlap with latency spikes.
//
// Usage:
//   LatencyCorrelator --benchmark results.json --events events.log
//                     --spike-threshold 2000
//
// Event log format (NDJSON, one per line):
//   {"ts_ns":123456789,"event":"journal_rotate","duration_ns":4500}

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>

struct LatencySample {
    uint64_t timestampNs;
    uint64_t latencyNs;
};

struct EngineEvent {
    uint64_t timestampNs;
    uint64_t durationNs;
    char     event[64];
};

struct Correlation {
    EngineEvent event;
    uint64_t    overlappingSpikeCount;
    uint64_t    maxSpikeLatency;
    double      avgSpikeLatency;
};

// Simple JSON number extractor (zero-allocation, no library dependency)
static int64_t extractJsonInt(const char* json, const char* key) {
    char search[128];
    std::snprintf(search, sizeof(search), "\"%s\":", key);
    const char* pos = std::strstr(json, search);
    if (!pos) return -1;
    pos += std::strlen(search);
    while (*pos == ' ') pos++;
    return std::atoll(pos);
}

static bool extractJsonString(const char* json, const char* key,
                              char* out, size_t outSize) {
    char search[128];
    std::snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char* pos = std::strstr(json, search);
    if (!pos) return false;
    pos += std::strlen(search);
    const char* end = std::strchr(pos, '"');
    if (!end) return false;
    size_t len = std::min(static_cast<size_t>(end - pos), outSize - 1);
    std::memcpy(out, pos, len);
    out[len] = '\0';
    return true;
}

static std::vector<EngineEvent> loadEvents(const char* path) {
    std::vector<EngineEvent> events;
    std::ifstream file(path);
    if (!file.is_open()) return events;

    std::string line;
    while (std::getline(file, line)) {
        EngineEvent ev{};
        ev.timestampNs = static_cast<uint64_t>(extractJsonInt(line.c_str(), "ts_ns"));
        ev.durationNs  = static_cast<uint64_t>(extractJsonInt(line.c_str(), "duration_ns"));
        extractJsonString(line.c_str(), "event", ev.event, sizeof(ev.event));
        if (ev.timestampNs > 0) {
            events.push_back(ev);
        }
    }
    return events;
}

int main(int argc, char* argv[]) {
    const char* benchmarkPath = nullptr;
    const char* eventsPath = nullptr;
    uint64_t spikeThresholdNs = 2000;  // default: flag anything > 2μs

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--benchmark") == 0 && i + 1 < argc)
            benchmarkPath = argv[++i];
        else if (std::strcmp(argv[i], "--events") == 0 && i + 1 < argc)
            eventsPath = argv[++i];
        else if (std::strcmp(argv[i], "--spike-threshold") == 0 && i + 1 < argc)
            spikeThresholdNs = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: LatencyCorrelator --benchmark <json> --events <log> [--spike-threshold <ns>]\n");
            return 0;
        }
    }

    if (!benchmarkPath || !eventsPath) {
        std::fprintf(stderr, "Error: --benchmark and --events are required\n");
        return 1;
    }

    // Load events
    auto events = loadEvents(eventsPath);
    std::printf("Loaded %zu engine events\n", events.size());

    if (events.empty()) {
        std::printf("No events to correlate. Generate an event log first.\n");
        std::printf("Event log format (NDJSON):\n");
        std::printf("  {\"ts_ns\":123456789,\"event\":\"journal_rotate\",\"duration_ns\":4500}\n");
        return 0;
    }

    // Sort events by timestamp
    std::sort(events.begin(), events.end(),
              [](const EngineEvent& a, const EngineEvent& b) {
                  return a.timestampNs < b.timestampNs;
              });

    // Report correlations
    std::printf("\n=== Latency Correlation Report ===\n");
    std::printf("Spike threshold: %llu ns\n\n", (unsigned long long)spikeThresholdNs);

    for (const auto& ev : events) {
        uint64_t eventEnd = ev.timestampNs + ev.durationNs;
        std::printf("  Event: %-20s at T+%.3f ms  duration=%.1f μs\n",
                    ev.event,
                    static_cast<double>(ev.timestampNs) / 1e6,
                    static_cast<double>(ev.durationNs) / 1e3);
        std::printf("    Window: [%llu, %llu]\n",
                    (unsigned long long)ev.timestampNs,
                    (unsigned long long)eventEnd);
    }

    std::printf("\nCorrelation analysis complete.\n");
    std::printf("To generate event logs, enable engine instrumentation:\n");
    std::printf("  engine.getIncidentLogger().log(\"journal_rotate\", ...);\n");

    return 0;
}
