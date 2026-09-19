#pragma once

// CapacityMonitor — resource exhaustion alerting.
//
// Roadmap Phase 4, Week 15: Capacity Exhaustion Alerts
//
// Monitors:
//   - Queue depth > 90% of capacity
//   - Memory usage > 95% of limit
//   - Journal disk usage > 90%
//   - Replication lag > 100ms
//
// Every breach is emitted as a `capacity_alert` event on the structured log
// sink (StructuredLog.h), which needs no configuration and is where the rest
// of the engine already reports. An AlertDispatcher (webhooks) and an
// IncidentLogger (NDJSON file) are additionally notified when one has been
// attached; both need a deployment decision first, so neither is required for
// a breach to be visible.
//
// MatchingEngine::startAsync() installs the queue-depth and journal-disk
// callbacks and starts this monitor; stopAsync() stops it.

#include "AlertDispatcher.h"
#include "IncidentLogger.h"
#include "StructuredLog.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace OrderMatcher {

struct CapacityThresholds {
    double queueDepthPct{0.90};       // Alert when queue > 90% full
    double memoryUsagePct{0.95};      // Alert when memory > 95% used
    double journalDiskPct{0.90};      // Alert when disk > 90% used
    uint64_t replicationLagMs{100};   // Alert when repl lag > 100ms
    uint64_t checkIntervalMs{1000};   // Check every second
};

class CapacityMonitor {
public:
    // Callback types for querying current resource usage
    using QueueDepthFn = std::function<double()>;       // Returns 0.0–1.0
    using MemoryUsageFn = std::function<double()>;      // Returns 0.0–1.0
    using DiskUsageFn = std::function<double()>;        // Returns 0.0–1.0
    using ReplicationLagFn = std::function<uint64_t()>; // Returns ms

    CapacityMonitor() = default;

    ~CapacityMonitor() { stop(); }

    CapacityMonitor(const CapacityMonitor&) = delete;
    CapacityMonitor& operator=(const CapacityMonitor&) = delete;

    void setThresholds(const CapacityThresholds& t) { thresholds_ = t; }
    void setAlertDispatcher(AlertDispatcher* d) { alertDispatcher_ = d; }
    void setIncidentLogger(IncidentLogger* l) { incidentLogger_ = l; }

    // Register resource query callbacks
    void setQueueDepthCallback(QueueDepthFn fn) { queryQueueDepth_ = std::move(fn); }
    void setMemoryUsageCallback(MemoryUsageFn fn) { queryMemoryUsage_ = std::move(fn); }
    void setDiskUsageCallback(DiskUsageFn fn) { queryDiskUsage_ = std::move(fn); }
    void setReplicationLagCallback(ReplicationLagFn fn) { queryReplLag_ = std::move(fn); }

    void start() {
        if (running_.exchange(true)) return;
        worker_ = std::thread([this] { monitorLoop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
    }

    // Manual check (for testing without background thread)
    void checkNow() {
        checkAllResources();
    }

    // Stats
    uint64_t alertsRaised() const {
        return alertsRaised_.load(std::memory_order_relaxed);
    }

private:
    // stop() joins this thread on the engine's shutdown path, so the sleep
    // between checks is also the worst case for how long shutdown blocks.
    // Sleeping the full checkIntervalMs in one call made that a whole second
    // per engine teardown. Sleeping in slices bounds the join at
    // kSleepSliceMs instead; if the check interval ever needs sub-10ms
    // shutdown latency, swap the slices for a condition_variable with a
    // deadline.
    static constexpr uint64_t kSleepSliceMs = 10;

    void monitorLoop() {
        while (running_.load(std::memory_order_acquire)) {
            for (uint64_t slept = 0;
                 slept < thresholds_.checkIntervalMs &&
                     running_.load(std::memory_order_acquire);
                 slept += kSleepSliceMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::min(kSleepSliceMs, thresholds_.checkIntervalMs - slept)));
            }
            if (!running_.load(std::memory_order_acquire)) break;
            checkAllResources();
        }
    }

    void checkAllResources() {
        // Queue depth
        if (queryQueueDepth_) {
            double depth = queryQueueDepth_();
            if (depth > thresholds_.queueDepthPct) {
                raiseAlert("queue_depth",
                    "Queue depth at " + pctStr(depth) +
                    " (threshold: " + pctStr(thresholds_.queueDepthPct) + ")",
                    AlertLevel::Warning, depth);
            }
        }

        // Memory usage
        if (queryMemoryUsage_) {
            double usage = queryMemoryUsage_();
            if (usage > thresholds_.memoryUsagePct) {
                raiseAlert("memory_usage",
                    "Memory usage at " + pctStr(usage) +
                    " (threshold: " + pctStr(thresholds_.memoryUsagePct) + ")",
                    AlertLevel::Critical, usage);
            }
        }

        // Journal disk usage
        if (queryDiskUsage_) {
            double disk = queryDiskUsage_();
            if (disk > thresholds_.journalDiskPct) {
                raiseAlert("journal_disk",
                    "Journal disk usage at " + pctStr(disk) +
                    " (threshold: " + pctStr(thresholds_.journalDiskPct) + ")",
                    AlertLevel::Critical, disk);
            }
        }

        // Replication lag
        if (queryReplLag_) {
            uint64_t lag = queryReplLag_();
            if (lag > thresholds_.replicationLagMs) {
                raiseAlert("replication_lag",
                    "Replication lag at " + std::to_string(lag) +
                    "ms (threshold: " + std::to_string(thresholds_.replicationLagMs) + "ms)",
                    AlertLevel::Critical, static_cast<double>(lag) / 1000.0);
            }
        }
    }

    void raiseAlert(const std::string& resource, const std::string& message,
                    AlertLevel level, double usagePct) {
        // The structured sink is the only output that is always there. A
        // webhook needs an endpoint and the incident log needs a path, and
        // both are deployment decisions — so for most of this monitor's life
        // both pointers were null and a breach produced nothing at all except
        // a counter increment. obSink() is the seam the rest of the engine
        // already reports through (engine_start, checkpoint_abandoned,
        // shutdown_worker_still_running), so an operator who has turned
        // logging on sees capacity breaches without configuring anything new.
        obSink().log(obEvent("capacity_alert", severityOf(level))
                         .kv("resource",  resource)
                         .kv("level",     alertLevelStr(level))
                         .kv("usage_pct", usagePct * 100.0)
                         .kv("detail",    message));
        if (alertDispatcher_) {
            alertDispatcher_->fire(level, "capacity_monitor", message);
        }
        if (incidentLogger_) {
            incidentLogger_->logCapacityAlert(resource, usagePct * 100.0);
        }
        alertsRaised_.fetch_add(1, std::memory_order_relaxed);
    }

    static LogSeverity severityOf(AlertLevel level) {
        switch (level) {
            case AlertLevel::Info:     return LogSeverity::Info;
            case AlertLevel::Warning:  return LogSeverity::Warn;
            case AlertLevel::Critical:
            case AlertLevel::Fatal:    return LogSeverity::Error;
        }
        return LogSeverity::Warn;
    }

    static std::string pctStr(double fraction) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f%%", fraction * 100.0);
        return buf;
    }

    CapacityThresholds thresholds_;
    AlertDispatcher* alertDispatcher_{nullptr};
    IncidentLogger* incidentLogger_{nullptr};

    QueueDepthFn queryQueueDepth_;
    MemoryUsageFn queryMemoryUsage_;
    DiskUsageFn queryDiskUsage_;
    ReplicationLagFn queryReplLag_;

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::atomic<uint64_t> alertsRaised_{0};
};

}  // namespace OrderMatcher
