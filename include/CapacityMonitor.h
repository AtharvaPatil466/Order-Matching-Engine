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
// Each alert triggers a webhook with configurable severity.
// Integrates with the existing AlertDispatcher.

#include "AlertDispatcher.h"
#include "IncidentLogger.h"
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
    void monitorLoop() {
        while (running_.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(thresholds_.checkIntervalMs));
            if (!running_.load()) break;
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
        if (alertDispatcher_) {
            alertDispatcher_->fire(level, "capacity_monitor", message);
        }
        if (incidentLogger_) {
            incidentLogger_->logCapacityAlert(resource, usagePct * 100.0);
        }
        alertsRaised_.fetch_add(1, std::memory_order_relaxed);
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
