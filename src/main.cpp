#include "MatchingEngine.h"
#include "AdminServer.h"
#include "ReplicationProtocol.h"
#include "Config.h"
#include "ConfigValidator.h"
#include "Journal.h"
#include "Metrics.h"
#ifdef OB_HAVE_DPDK
#include "DpdkGateway.h"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

using namespace OrderMatcher;

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_reload_config{false};

void signalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

void sighupHandler(int) {
    g_reload_config.store(true, std::memory_order_release);
}

namespace {

// Resolve a CLI flag, falling back to an OB_* environment variable.
// Empty return means neither was supplied.
std::string flagOrEnv(int argc, char** argv, const char* flag, const char* env) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return argv[i + 1];
        }
    }
    if (const char* v = std::getenv(env)) return v;
    return {};
}

}  // namespace

int main(int argc, char* argv[]) {
    std::cout << "╔═══════════════════════════════════════════════════╗\n"
              << "║   Low-Latency Order Matching Engine v3.0          ║\n"
              << "║   Thread-Per-Symbol | Admin Server | Production   ║\n"
              << "╚═══════════════════════════════════════════════════╝\n\n";

    // ── CLI / env parsing ─────────────────────────────────────────────
    size_t numThreads = 4;
    uint16_t adminPort = 8080;
    size_t numSymbols = 4;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc)
            numThreads = std::stoul(argv[++i]);
        else if (arg == "--port" && i + 1 < argc)
            adminPort = static_cast<uint16_t>(std::stoul(argv[++i]));
        else if (arg == "--symbols" && i + 1 < argc)
            numSymbols = std::stoul(argv[++i]);
        else if (arg == "--help") {
            std::cout
                << "Usage: OrderEngine [options]\n"
                << "  --threads N    Worker threads (default: 4)\n"
                << "  --port P       Admin HTTP port (default: 8080)\n"
                << "  --symbols S    Number of symbols (default: 4)\n"
                << "\n"
                << "Replication (env-driven, optional):\n"
                << "  OB_NODE_ROLE              primary | backup   (unset = standalone)\n"
                << "  OB_NODE_ID                numeric node id    (default: 1)\n"
                << "  OB_REPLICATION_PORT       primary listen port (default: 9002)\n"
                << "  OB_PRIMARY_HOST           backup-only: host of primary\n"
                << "  OB_PRIMARY_REPLICATION_PORT  backup-only: port of primary\n"
                << "  OB_JOURNAL_PATH           enables on-disk journaling\n"
                << "  --config PATH  Config file path (reloaded on SIGHUP)\n";
            return 0;
        }
    }

    // Signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGHUP, sighupHandler);

    // ── Config ───────────────────────────────────────────────────────
    Config cfg;
    const std::string configPath = flagOrEnv(argc, argv, "--config", "OB_CONFIG_PATH");
    if (!configPath.empty()) {
        if (cfg.loadFile(configPath)) {
            std::cout << "[Config] Loaded from " << configPath << "\n";
        } else {
            std::cerr << "[Config] WARNING: could not load " << configPath << "\n";
        }
    }

    // ── Startup config validation (P3-7, fail-fast) ───────────────────
    // Validate every declared instrument / participant and the journal path
    // BEFORE constructing the engine. On ANY problem, print each one and
    // exit(1) — the engine never starts in a silently-degraded state.
    {
        ConfigValidationResult vr = validateStartupConfig(cfg);

        // Also validate the journal path we will actually open (from
        // --journal / OB_JOURNAL_PATH, which is separate from the config key).
        const std::string resolvedJournal =
            flagOrEnv(argc, argv, "--journal", "OB_JOURNAL_PATH");
        if (!resolvedJournal.empty() && !isJournalPathWritable(resolvedJournal)) {
            vr.fail("journal path '" + resolvedJournal +
                    "': parent directory does not exist or is not writable");
        }

        if (!vr.ok) {
            std::cerr << "[Config] FATAL: startup configuration is invalid — "
                         "refusing to start:\n";
            for (const auto& e : vr.errors) std::cerr << "  - " << e << "\n";
            return 1;
        }
        std::cout << "[Config] Startup validation passed\n";
    }

    // ── Engine ────────────────────────────────────────────────────────
    MatchingEngine engine;
    for (size_t s = 0; s < numSymbols; ++s) {
        engine.addSymbol(static_cast<SymbolId>(s));
    }
    std::cout << "[Engine] Registered " << numSymbols << " symbols\n";

    // Optional journal — enables replication on the primary path because
    // the coordinator hooks the journal's onCommit callback. Without a
    // journal, the engine still runs but replication has no entries to
    // ship (heartbeats only).
    const std::string journalPath = flagOrEnv(argc, argv, "--journal", "OB_JOURNAL_PATH");
    if (!journalPath.empty()) {
        engine.enableJournal(journalPath);
        std::cout << "[Engine] Journal enabled at " << journalPath << "\n";
    }

    const std::string maxSizeStr = flagOrEnv(argc, argv, "--journal-max-mb", "OB_JOURNAL_MAX_SIZE_MB");
    const size_t journalMaxMb = maxSizeStr.empty() ? 0 : std::stoul(maxSizeStr);

    std::cout << "[Engine] Starting async mode with " << numThreads << " worker threads...\n";
    engine.startAsync(numThreads, 8192);

    // ── Replication wiring (env-driven) ──────────────────────────────
    // The OrderEngine binary used to read these env vars only via its
    // docker-compose.yml manifest, but no code path consumed them.
    // Wiring them here is what closes the gap between the
    // "TLA+-verified replication protocol" claim and the running binary.
    std::unique_ptr<ReplicationCoordinator> coord;

    const std::string role = flagOrEnv(argc, argv, "--role", "OB_NODE_ROLE");
    if (!role.empty()) {
        const uint32_t nodeId = static_cast<uint32_t>(
            std::stoul(flagOrEnv(argc, argv, "--node-id", "OB_NODE_ID").empty()
                          ? "1"
                          : flagOrEnv(argc, argv, "--node-id", "OB_NODE_ID")));
        const int replPort = std::stoi(
            flagOrEnv(argc, argv, "--repl-port", "OB_REPLICATION_PORT").empty()
                ? "9002"
                : flagOrEnv(argc, argv, "--repl-port", "OB_REPLICATION_PORT"));

        coord = std::make_unique<ReplicationCoordinator>(nodeId);

        if (role == "primary") {
            // Both hooks below MUST be registered before startAsPrimary():
            // its accept loop is live once it returns, and a backup that
            // connects before setOnPeerJoined is armed receives no
            // SnapshotStart/SnapshotEnd bracket — the backup engages
            // snapshot buffering on connect and would buffer live entries
            // forever, never applying them. Registering the commit hook
            // first closes the sibling window where entries committed
            // pre-registration are never shipped to that early joiner.
            // replicateEntry() no-ops until the role flips to Primary, so
            // early registration is safe.

            // Hook the journal's onCommit so every fsync-durable batch
            // is shipped to the backup. The callback runs synchronously
            // on the engine's writer thread — the coordinator's send
            // path locks internally and writes to a non-blocking TCP
            // socket, so the overhead is bounded.
            if (auto* j = engine.getJournal()) {
                ReplicationCoordinator* c = coord.get();
                j->setOnCommit([c](const JournalEntry* entries, size_t n) {
                    for (size_t i = 0; i < n; ++i) {
                        c->replicateEntry(reinterpret_cast<const uint8_t*>(&entries[i]),
                                          sizeof(JournalEntry));
                    }
                });
                std::cout << "[Replication] Journal commit hook installed\n";
            } else {
                std::cout << "[Replication] WARNING: no journal — only heartbeats will flow.\n"
                          << "             Set OB_JOURNAL_PATH to enable entry replication.\n";
            }

            // Snapshot-on-join: stream every currently-resting order to
            // a backup that just connected. Closes the rolling-restart
            // gap where pre-reconnect entries would never reach a
            // late-joining backup. The Snapshot handler on the receive
            // side is idempotent, so running concurrently with live
            // replication is safe (duplicates collapse).
            {
                ReplicationCoordinator* c = coord.get();
                MatchingEngine* e = &engine;
                coord->setOnPeerJoined([c, e]() {
                    static auto& kStreams = MetricsRegistry::instance().counter(
                        "replication_snapshot_streams_total",
                        "Number of times a snapshot stream was initiated to a joining peer");
                    static auto& kSnapEntries = MetricsRegistry::instance().counter(
                        "replication_snapshot_entries_total",
                        "Total resting orders shipped as Snapshot entries across all streams");
                    size_t shipped = 0;
                    e->streamSnapshot([&](const JournalEntry& entry) {
                        c->replicateEntry(reinterpret_cast<const uint8_t*>(&entry),
                                          sizeof(JournalEntry));
                        ++shipped;
                    });
                    kStreams.increment(1);
                    kSnapEntries.increment(shipped);
                    std::cout << "[Replication] Snapshot streamed " << shipped
                              << " resting orders to joining backup\n";
                });
            }

            if (!coord->startAsPrimary(replPort)) {
                std::cerr << "[Replication] FATAL: failed to start as primary on port "
                          << replPort << "\n";
                return 1;
            }
            std::cout << "[Replication] Started as PRIMARY (nodeId=" << nodeId
                      << ", port=" << replPort << ")\n";
        } else if (role == "backup") {
            const std::string primaryHost = flagOrEnv(argc, argv, "--primary-host",
                                                       "OB_PRIMARY_HOST");
            const std::string primaryPortStr = flagOrEnv(argc, argv, "--primary-port",
                                                          "OB_PRIMARY_REPLICATION_PORT");
            if (primaryHost.empty()) {
                std::cerr << "[Replication] FATAL: backup role requires OB_PRIMARY_HOST\n";
                return 1;
            }
            const int primaryPort = primaryPortStr.empty() ? 9002 : std::stoi(primaryPortStr);

            // Backup books stay in replay mode for the duration of the
            // backup role so applying primary-shipped entries does not
            // emit duplicate market data or order acks to local clients.
            // On promotion (heartbeat timeout → lease acquisition), the
            // promotion callback below flips them back to live mode.
            engine.setReplayModeAllBooks(true);

            coord->setJournalApplyCallback([&engine](const uint8_t* data, size_t len) {
                if (len != sizeof(JournalEntry)) return;
                JournalEntry entry{};
                std::memcpy(&entry, data, sizeof(entry));
                engine.applyReplicatedEntry(entry);
            });

            coord->setPromotionCallback([&engine]() {
                std::cout << "[Replication] PROMOTED to primary — exiting replay mode\n";
                engine.setReplayModeAllBooks(false);
            });

            if (!coord->startAsBackup(primaryHost, primaryPort)) {
                std::cerr << "[Replication] FATAL: failed to connect to primary at "
                          << primaryHost << ":" << primaryPort << "\n";
                return 1;
            }
            std::cout << "[Replication] Started as BACKUP (nodeId=" << nodeId
                      << ", primary=" << primaryHost << ":" << primaryPort << ")\n";
        } else {
            std::cerr << "[Replication] FATAL: OB_NODE_ROLE must be 'primary' or 'backup', got '"
                      << role << "'\n";
            return 1;
        }
    } else {
        std::cout << "[Replication] Disabled (standalone mode)\n";
    }

    // ── Admin HTTP server ────────────────────────────────────────────
    AdminServer admin(engine, adminPort);
    admin.setReplicationCoordinator(coord.get());  // null is fine — standalone
    const std::string adminToken = flagOrEnv(argc, argv, "--admin-token", "OB_ADMIN_TOKEN");
    if (!adminToken.empty()) {
        admin.setAdminToken(adminToken);
        std::cout << "[Admin] Token auth enabled on all endpoints (except /health)\n";
    } else {
        std::cout << "[Admin] WARNING: No OB_ADMIN_TOKEN set — admin port unauthenticated\n";
    }
    admin.start();

    // ── Warmup ───────────────────────────────────────────────────────
    // Skip warmup on backup: it would inject orders that the primary
    // hasn't sent, breaking the byte-identical-state invariant.
    if (role != "backup") {
        std::cout << "[Engine] Warming up books...\n";
        uint64_t warmupId = 1;
        for (size_t s = 0; s < numSymbols; ++s) {
            auto sym = static_cast<SymbolId>(s);
            for (int i = 0; i < 100; ++i) {
                engine.processOrder(sym, warmupId++, 1, Side::Buy,
                                    100000 + (i % 100), 10, OrderType::Limit);
                engine.processOrder(sym, warmupId++, 2, Side::Sell,
                                    100100 + (i % 100), 10, OrderType::Limit);
            }
        }
        engine.waitForDrain();
        std::cout << "[Engine] Warmup complete. System ready.\n\n";
        admin.setReady(true);
        std::cout << "[Engine] Ready for traffic.\n";
    } else {
        std::cout << "[Engine] Backup mode — skipping warmup, awaiting primary entries.\n\n";
        admin.setReady(true);
        std::cout << "[Engine] Ready for traffic.\n";
    }

#ifdef OB_HAVE_DPDK
    // Kernel-bypass OUCH ingestion on the secondary ENI (F-Stack/DPDK). Behind
    // OB_HAVE_DPDK so the default kernel build is unchanged; further gated on
    // OB_DPDK=1 so a DPDK-capable binary can still run the kernel path. F-Stack
    // reads its NIC/lcore/IP config from the process args (--conf) via ff_init.
    std::unique_ptr<DpdkGateway> dpdk;
    if (const char* d = std::getenv("OB_DPDK"); d && std::string(d) == "1") {
        DpdkConfig dcfg;
        if (const char* p = std::getenv("OB_DPDK_PORT")) {
            dcfg.port = static_cast<uint16_t>(std::stoul(p));
        }
        dpdk = std::make_unique<DpdkGateway>(engine, dcfg);
        if (dpdk->start(argc, argv)) {
            std::cout << "[DPDK] F-Stack kernel-bypass ingestion started on port "
                      << dcfg.port << "\n";
        } else {
            std::cerr << "[DPDK] FATAL: F-Stack ingestion failed to start\n";
            return 1;
        }
    }
#endif

    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "  Admin server:  http://localhost:" << adminPort << "\n";
    std::cout << "  Endpoints:\n";
    std::cout << "    GET /metrics              — throughput & queue depth\n";
    std::cout << "    GET /otr?participantId=X  — order-to-trade ratio\n";
    std::cout << "    GET /book?symbolId=Y      — L2 order book snapshot\n";
    std::cout << "    GET /role                 — primary | backup | standalone\n";
    std::cout << "    GET /replication          — peer liveness, epoch, bytes\n";
    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "Press Ctrl-C to shut down.\n\n";

    // Block until SIGINT/SIGTERM
    while (g_running.load(std::memory_order_acquire)) {
        if (g_reload_config.exchange(false, std::memory_order_acq_rel) && !configPath.empty()) {
            std::cout << "[Config] SIGHUP received — reloading " << configPath << "\n";
            cfg.loadFile(configPath);
            const uint64_t newRate = static_cast<uint64_t>(cfg.getInt64("rate_limit.default_rate", 0));
            const uint64_t newBurst = static_cast<uint64_t>(cfg.getInt64("rate_limit.default_burst", 0));
            if (newRate > 0) {
                engine.getRateLimiter().reconfigure(newRate, newBurst);
                std::cout << "[Config] Rate limiter reconfigured: " << newRate << " msg/s\n";
            }
        }
        if (journalMaxMb > 0) {
            if (auto* j = engine.getJournal()) {
                if (j->needsCheckpoint(std::numeric_limits<size_t>::max(), journalMaxMb * 1024 * 1024)) {
                    std::cout << "[Journal] Size limit reached — checkpointing...\n";
                    engine.checkpoint();
                    std::cout << "[Journal] Checkpoint complete.\n";
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Graceful shutdown (P3-6): stop admitting new orders, drain in-flight
    // requests, cancel DAY orders at session end, and checkpoint GTD/GTC
    // resting orders so a restart restores them.
    std::cout << "\n[Engine] Shutting down — draining queues...\n";
    admin.stop();
#ifdef OB_HAVE_DPDK
    if (dpdk) dpdk->stop();
#endif
    const auto shutdownReport = engine.gracefulShutdown();
    std::cout << "[Engine] Queues drained. DAY orders cancelled: "
              << shutdownReport.dayOrdersCancelled
              << ", persisted for restore: " << shutdownReport.ordersPersisted
              << " (GTD=" << shutdownReport.gtdOrdersPersisted
              << ", other=" << shutdownReport.otherOrdersPersisted << ")"
              << (shutdownReport.journalEnabled ? "" : " [no journal — not durable]")
              << "\n";
    if (coord) coord->stop();
    engine.stopAsync();
    std::cout << "[Engine] Shutdown complete.\n";

    return 0;
}
