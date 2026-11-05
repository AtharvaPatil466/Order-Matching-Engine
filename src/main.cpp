#include "MatchingEngine.h"
#include "AlertDispatcher.h"
#include "AdminServer.h"
#include "CliFlags.h"
#include "Config.h"
#include "ConfigValidator.h"
#include "Journal.h"
#include "JournalBoot.h"
#include "Metrics.h"
#include "SessionScheduler.h"

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

int main(int argc, char* argv[]) {
    std::cout << "╔═══════════════════════════════════════════════════╗\n"
              << "║   Low-Latency Order Matching Engine v3.0          ║\n"
              << "║   Thread-Per-Symbol | Admin Server | Production   ║\n"
              << "╚═══════════════════════════════════════════════════╝\n\n";

    // ── CLI / env parsing ─────────────────────────────────────────────
    // Every flag this binary reads, in one table: the unknown-argument check
    // and --help both come from it. A flag read below but missing here is
    // refused at startup, so the two cannot drift apart silently.
    static constexpr CliFlag kFlags[] = {
        {"--threads", "N", "worker threads (default 4)"},
        {"--symbols", "S", "symbols 0..S-1 (default 4)"},
        {"--port", "P", "admin HTTP port (default 8080)"},
        {"--admin-bind", "ADDR", "admin listen address (default 127.0.0.1) [OB_ADMIN_BIND]"},
        {"--admin-token", "TOKEN", "admin bearer token [OB_ADMIN_TOKEN]"},
        {"--admin-no-auth", nullptr, "run the admin port without a token [OB_ADMIN_NO_AUTH=1]"},
        {"--config", "PATH", "config file, re-read on SIGHUP [OB_CONFIG_PATH]"},
        {"--journal", "PATH", "journal file, replayed at boot [OB_JOURNAL_PATH]"},
        {"--journal-max-mb", "MB", "checkpoint past this size [OB_JOURNAL_MAX_SIZE_MB]"},
        {"--replay-legacy-journal", nullptr,
         "replay a journal from a build that seeded orders [OB_REPLAY_LEGACY_JOURNAL=1]"},
        {"--session-schedule", "PRE,OPEN,CLOSE_AUCTION,CLOSE",
         "session timetable, HH:MM UTC; the close cancels DAY orders [OB_SESSION_SCHEDULE]"},
        {"--role", "ROLE", "removed with replication; any value refuses to boot [OB_NODE_ROLE]"},
        {"--help", nullptr, "this message"},
    };
    if (const std::string err = cliArgsError(argc, argv, kFlags); !err.empty()) {
        std::cerr << "[Config] FATAL: " << err << " — refusing to start. See --help.\n";
        return 1;
    }

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
            std::cout << "Usage: OrderEngine [options]\n";
            printCliFlags(std::cout, kFlags);
            std::cout << "\nA flag wins over its [ENV] variable. Any other argument is refused.\n";
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

    // ── Structured logging sink ──────────────────────────────────────
    // config/engine.conf.example has documented `log_sink = stderr | null`
    // since it was written, and nothing read the key: no production entry
    // point ever called setObSink(), so obSink() resolved to NullSink and
    // EVERY structured log in the engine went nowhere. That covers the events
    // an operator most needs — risk rejects, circuit-breaker trips, kill
    // switch sweeps, gateway disconnects, control-plane contention, an
    // undeliverable alert — all built, all discarded.
    //
    // Installed before validation and before the engine exists, so startup
    // problems are the first thing the sink sees.
    //
    // Default stays `null`, matching what the example file calls the
    // production default: obSinkActive() gates event construction on the hot
    // path, and a LogEvent heap-allocates per .kv(), which measures 5-8x on
    // matching latency. Turning logs on is an operator's decision; what was
    // wrong was that they could not make it.
    static JsonStderrSink jsonSink;
    const std::string logSink = cfg.getString("log_sink", "null");
    if (logSink == "stderr") {
        setObSink(&jsonSink);
        std::cout << "[Log] Structured sink: stderr (JSON)\n";
    } else if (logSink != "null" && !logSink.empty()) {
        std::cerr << "[Log] WARNING: unknown log_sink '" << logSink
                  << "' — expected 'stderr' or 'null'; logging stays off\n";
    }

    // ── Startup config validation (P3-7, fail-fast) ───────────────────
    // Validate every declared instrument / participant and the journal path
    // BEFORE constructing the engine. On ANY problem, print each one and
    // exit(1) — the engine never starts in a silently-degraded state.
    std::vector<int> workerCores;   // worker_cores, applied at startAsync
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

        // Replication was removed (roadmap 1.1 R1): it had no fencing, a
        // promoted backup kept applying the old primary's stream, its send
        // blocked the commit path, and resync only ever added orders
        // (REPL-1..9). A role still refuses to boot, so a deployment that sets
        // one finds out here rather than running as an unannounced single node.
        const std::string requestedRole = flagOrEnv(argc, argv, "--role", "OB_NODE_ROLE");
        if (!requestedRole.empty()) {
            vr.fail("--role / OB_NODE_ROLE = '" + requestedRole + "': replication was "
                    "removed. This engine runs as a single node and recovers from its "
                    "journal; unset the role");
        }

        workerCores = validateWorkerCores(cfg, numThreads > 0 ? numThreads : 1,
                                          Utils::allowedCpus(), vr);

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
    // No session end until a SessionScheduler (below) brings one, so a DAY
    // order without an expiry is refused instead of resting as GTC (PRIOR-7).
    engine.setSessionEndScheduled(false);
    std::cout << "[Engine] Registered " << numSymbols << " symbols\n";

    // STP-8: a symbol trades in the band its engine.conf entry names, set
    // before replay so no first order can pick one (unset, a book centres its
    // band on its first rest, and one participant's 1-tick bid made every real
    // price unrestable). A config that leaves a symbol out refuses to boot.
    // Without --config the books keep the anchor, for local runs and tests.
    if (!configPath.empty()) {
        for (size_t s = 0; s < numSymbols; ++s) {
            const std::string key = "instrument." + std::to_string(s) + ".";
            if (!cfg.has(key + "min_price") || !cfg.has(key + "max_price")) {
                std::cerr << "[Config] FATAL: symbol " << s << " has no price band; set "
                          << key << "min_price and " << key << "max_price\n";
                return 1;
            }
            try {
                engine.getOrderBook(static_cast<SymbolId>(s))
                    ->setPriceRange(cfg.getInt64(key + "min_price"),
                                    cfg.getInt64(key + "max_price"));
            } catch (const std::exception& e) {
                std::cerr << "[Config] FATAL: symbol " << s << " price band: " << e.what() << "\n";
                return 1;
            }
        }
        std::cout << "[Config] Price bands set for " << numSymbols << " symbols\n";
    }

    // Enable, refuse and replay the journal — the same sequence GatewayServer
    // runs, shared through JournalBoot.h so the two cannot drift apart.
    if (const int rc = bootJournal(engine, argc, argv); rc != 0) return rc;

    const std::string maxSizeStr = flagOrEnv(argc, argv, "--journal-max-mb", "OB_JOURNAL_MAX_SIZE_MB");
    const size_t journalMaxMb = maxSizeStr.empty() ? 0 : std::stoul(maxSizeStr);

    // ── Alerting (config-driven) ──────────────────────────────────────
    //
    // alert_webhook_url / alert_webhook_format / alert_min_level existed only
    // as commented-out lines in config/engine.conf.example. NO CODE READ THEM,
    // so AlertDispatcher — implemented and unit-tested — was reachable from no
    // production path at all. That is wired here.
    //
    // THE https LIMIT IS REPORTED AT STARTUP, NOT AT DELIVERY. AlertDispatcher
    // has no TLS client: httpPost speaks plain HTTP, so addWebhook() returns
    // false for any https:// URL. Finding that out when the first incident
    // fires is the worst possible time, and the shipped example config pointed
    // at https://hooks.slack.com/... — a URL the code provably refuses.
    //
    // The engine deliberately does NOT gain a TLS stack for this. It has no
    // external dependencies beyond liburing on Linux, and adding OpenSSL to
    // deliver one webhook would trade that for a supply-chain surface on the
    // alerting path of all places. The standard answer is a local terminator:
    // point the engine at http://127.0.0.1:<port> and let a sidecar do TLS to
    // Slack or PagerDuty. That is documented in engine.conf.example and
    // repeated in the failure message below.
    static AlertDispatcher alerts;
    const std::string alertUrl = cfg.getString("alert_webhook_url", "");
    if (!alertUrl.empty()) {
        const std::string fmtStr = cfg.getString("alert_webhook_format", "generic");
        AlertDispatcher::Format fmt = AlertDispatcher::Format::Generic;
        if (fmtStr == "slack")          fmt = AlertDispatcher::Format::Slack;
        else if (fmtStr == "pagerduty") fmt = AlertDispatcher::Format::PagerDuty;
        else if (fmtStr != "generic") {
            std::cerr << "[Alert] FATAL: unknown alert_webhook_format '" << fmtStr
                      << "' — expected generic, slack or pagerduty.\n";
            return 1;
        }

        const std::string lvlStr = cfg.getString("alert_min_level", "warning");
        AlertLevel minLevel = AlertLevel::Warning;
        if (lvlStr == "info")          minLevel = AlertLevel::Info;
        else if (lvlStr == "critical") minLevel = AlertLevel::Critical;
        else if (lvlStr == "fatal")    minLevel = AlertLevel::Fatal;
        else if (lvlStr != "warning") {
            std::cerr << "[Alert] FATAL: unknown alert_min_level '" << lvlStr
                      << "' — expected info, warning, critical or fatal.\n";
            return 1;
        }

        if (!alerts.addWebhook(alertUrl, fmt, cfg.getString("alert_routing_key", ""),
                               minLevel)) {
            std::cerr
                << "[Alert] FATAL: cannot deliver to " << alertUrl << "\n"
                << "        AlertDispatcher has no TLS client, so an https://\n"
                << "        endpoint is undeliverable. Refusing to start rather\n"
                << "        than run with alerting that silently drops every\n"
                << "        alert — you would find out during the incident it\n"
                << "        was supposed to announce.\n"
                << "        Run a local TLS terminator and point this at it:\n"
                << "          alert_webhook_url = http://127.0.0.1:8081/alerts\n"
                << "        (the sidecar forwards to Slack/PagerDuty over TLS)\n";
            return 1;
        }
        alerts.start();
        engine.getCapacityMonitor().setAlertDispatcher(&alerts);
        engine.getKillSwitch().setAlertDispatcher(&alerts);
        std::cout << "[Alert] Webhook enabled: " << alertUrl
                  << " (format=" << fmtStr << ", min_level=" << lvlStr << ")\n";
    }

    // Session timetable (roadmap 1.8-H8). Off unless asked for: the hours,
    // the timezone, the calendar and what a late start does are venue
    // decisions this binary does not make on its own. Parsed before the
    // engine starts so a bad value refuses to boot rather than running
    // without a close.
    // The flag, then OB_SESSION_SCHEDULE, then `session_schedule` in the
    // config file (Config::getString reads that same env name first).
    std::string sessionSpec =
        flagOrEnv(argc, argv, "--session-schedule", "OB_SESSION_SCHEDULE");
    if (sessionSpec.empty()) sessionSpec = cfg.getString("session_schedule");
    std::optional<SessionSchedule> sessionSchedule;
    if (!sessionSpec.empty()) {
        sessionSchedule = parseSessionSchedule(sessionSpec);
        if (!sessionSchedule) {
            std::cerr << "FATAL: --session-schedule / session_schedule '" << sessionSpec
                      << "' is not HH:MM,HH:MM,HH:MM,HH:MM (pre-open, open, closing"
                         " auction, close; UTC, strictly increasing)\n";
            return 1;
        }
    }

    std::cout << "[Engine] Starting async mode with " << numThreads << " worker threads...\n";
    engine.setWorkerCores(workerCores);
    engine.startAsync(numThreads, 8192);
    // GTD/DAY expiry. Nothing started this, so GTD orders rested and traded
    // forever. Safe in async mode: each sweep is posted to every worker as an
    // in-band control message, never run against a book from this thread.
    engine.startExpiryTimer();

    std::unique_ptr<SessionScheduler> scheduler;
    if (sessionSchedule) {
        std::vector<SymbolId> universe;
        for (size_t s = 0; s < numSymbols; ++s) universe.push_back(static_cast<SymbolId>(s));
        scheduler = std::make_unique<SessionScheduler>(engine, std::move(universe),
                                                       *sessionSchedule);
        scheduler->start();
        std::cout << "[Session] Scheduler on: " << sessionSpec
                  << " UTC, Monday to Friday (pre-open, open, closing auction, close)\n";
    }

    // ── Admin HTTP server ────────────────────────────────────────────
    AdminServer admin(engine, adminPort);
    // Loopback unless widened. The Docker image sets OB_ADMIN_BIND=0.0.0.0: a
    // published port or a k8s probe arrives on the container's interface, not
    // its loopback. A bad address makes admin.start() below refuse.
    if (const std::string adminBind = flagOrEnv(argc, argv, "--admin-bind", "OB_ADMIN_BIND");
        !adminBind.empty()) {
        admin.setBindAddress(adminBind);
    }
    const std::string adminToken = flagOrEnv(argc, argv, "--admin-token", "OB_ADMIN_TOKEN");
    // Presence is enough. Read as a value-taking flag, a bare --admin-no-auth
    // in the last argv slot was invisible, so the opt-out the FATAL message
    // below tells operators to pass did nothing.
    const bool adminAuthDisabled =
        flagOrEnvBool(argc, argv, "--admin-no-auth", "OB_ADMIN_NO_AUTH");

    if (!adminToken.empty()) {
        admin.setAdminToken(adminToken);
        std::cout << "[Admin] Token auth enabled on all endpoints (except /health)\n";
    } else if (adminAuthDisabled) {
        admin.setAuthDisabled(true);
        std::cout << "[Admin] Auth explicitly disabled via --admin-no-auth\n";
    } else {
        // Refuse to boot rather than serve the book and risk state openly
        // because a token was omitted. Opting out is available, but has to
        // be deliberate.
        std::cerr << "[Admin] FATAL: no admin token configured.\n"
                  << "        Set --admin-token / OB_ADMIN_TOKEN, or pass\n"
                  << "        --admin-no-auth (OB_ADMIN_NO_AUTH=1) to run the admin\n"
                  << "        port unauthenticated on purpose.\n";
        return 1;
    }
    // A failed bind used to be a line on stderr and nothing more: the engine
    // went on to announce "Ready for traffic" with no admin port, so the k8s
    // liveness probe hit nothing and the pod looked dead for an unrelated
    // reason. Refuse to come up instead.
    if (!admin.start()) {
        std::cerr << "[Admin] FATAL: admin server failed to start on port "
                  << adminPort << ".\n";
        return 1;
    }

    // ── Ready ────────────────────────────────────────────────────────
    // A "warmup" used to run here: 200 Limit orders per symbol — 800 at the
    // default --symbols 4 — attributed to participants 1 and 2 and submitted
    // through processOrder before the engine announced itself. They were not a
    // warmup. They rested in the live book and were served on /book, so a
    // client's first fill was against a counterparty that had never submitted
    // anything; they consumed client order ids 1..200*symbols, so a client
    // numbering from 1 collided with DuplicateOrderId; and they were journaled,
    // so the WAL opened with fabricated state. The log said "Warming up books",
    // which reads as page-faulting, and then "System ready".
    //
    // Nothing replaces it. If pre-faulting the pools is ever worth doing,
    // MemoryPool::warmup (include/MemoryPool.h:89) touches the memory without
    // inventing orders — the book is the wrong place for a page-fault workaround.
    admin.setReady(true);
    std::cout << "[Engine] Ready for traffic.\n";

    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "  Admin server:  http://localhost:" << adminPort << "\n";
    std::cout << "  Endpoints:\n";
    std::cout << "    GET /metrics              — throughput & queue depth\n";
    std::cout << "    GET /otr?participantId=X  — order-to-trade ratio\n";
    std::cout << "    GET /book?symbolId=Y      — L2 order book snapshot\n";
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
    // requests, and checkpoint every resting order so a restart restores it.
    // DAY orders included: a SIGTERM is a deploy, a reload or a crash-restart,
    // not the end of a session, and this used to cancel every client's DAY
    // orders on each one. Nothing here knows when a session ends, so nothing
    // here cancels them.
    std::cout << "\n[Engine] Shutting down — draining queues...\n";
    admin.stop();
    if (scheduler) scheduler->stop();  // no session transition during the drain
    const auto shutdownReport = engine.gracefulShutdown();
    std::cout << "[Engine] Queues drained. DAY orders cancelled: "
              << shutdownReport.dayOrdersCancelled
              << ", persisted for restore: " << shutdownReport.ordersPersisted
              << " (GTD=" << shutdownReport.gtdOrdersPersisted
              << ", other=" << shutdownReport.otherOrdersPersisted << ")"
              << (shutdownReport.journalEnabled ? "" : " [no journal — not durable]")
              << "\n";
    engine.stopAsync();
    std::cout << "[Engine] Shutdown complete.\n";

    return 0;
}
