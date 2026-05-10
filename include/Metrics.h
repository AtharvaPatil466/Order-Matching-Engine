#pragma once

#include <atomic>
#include <string>
#include <unordered_map>
#include <sstream>
#include <mutex>
#include <vector>
#include <memory>

namespace OrderMatcher {

// ─── Prometheus-style Counter (monotonically increasing) ─────────────────────

class Counter {
public:
    explicit Counter(const std::string& name, const std::string& help = "")
        : name_(name), help_(help), value_(0) {}

    void increment(uint64_t delta = 1) {
        value_.fetch_add(delta, std::memory_order_relaxed);
    }

    uint64_t value() const { return value_.load(std::memory_order_relaxed); }
    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }

private:
    std::string name_;
    std::string help_;
    std::atomic<uint64_t> value_;
};

// ─── Prometheus-style Gauge (can go up and down) ─────────────────────────────

class Gauge {
public:
    explicit Gauge(const std::string& name, const std::string& help = "")
        : name_(name), help_(help), value_(0) {}

    void set(int64_t val) { value_.store(val, std::memory_order_relaxed); }
    void increment(int64_t delta = 1) { value_.fetch_add(delta, std::memory_order_relaxed); }
    void decrement(int64_t delta = 1) { value_.fetch_sub(delta, std::memory_order_relaxed); }

    int64_t value() const { return value_.load(std::memory_order_relaxed); }
    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }

private:
    std::string name_;
    std::string help_;
    std::atomic<int64_t> value_;
};

// ─── Histogram (fixed-bucket latency distribution) ───────────────────────────

class Histogram {
public:
    explicit Histogram(const std::string& name, const std::string& help = "",
                       std::vector<double> buckets = {50, 100, 250, 500, 1000, 2500, 5000, 10000})
        : name_(name), help_(help), bucketBounds_(std::move(buckets)),
          // resize(N, 0) would copy-construct atomics, which is deleted.
          // Construct a fresh vector with N default-initialized atomics
          // (value-init = zero) and move-assign.
          bucketCounts_(bucketBounds_.size() + 1),
          count_(0), sum_(0) {}

    void observe(double value) {
        count_.fetch_add(1, std::memory_order_relaxed);
        // Atomic add for double using CAS loop
        double old = sum_.load(std::memory_order_relaxed);
        while (!sum_.compare_exchange_weak(old, old + value, std::memory_order_relaxed)) {}

        for (size_t i = 0; i < bucketBounds_.size(); ++i) {
            if (value <= bucketBounds_[i]) {
                bucketCounts_[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
        bucketCounts_.back().fetch_add(1, std::memory_order_relaxed); // +Inf always
    }

    uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    double sum() const { return sum_.load(std::memory_order_relaxed); }
    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }
    const std::vector<double>& bounds() const { return bucketBounds_; }
    uint64_t bucketCount(size_t i) const { return bucketCounts_[i].load(std::memory_order_relaxed); }

private:
    std::string name_;
    std::string help_;
    std::vector<double> bucketBounds_;
    std::vector<std::atomic<uint64_t>> bucketCounts_;
    std::atomic<uint64_t> count_;
    std::atomic<double> sum_;
};

// ─── Metrics Registry ────────────────────────────────────────────────────────

class MetricsRegistry {
public:
    static MetricsRegistry& instance() {
        static MetricsRegistry reg;
        return reg;
    }

    Counter& counter(const std::string& name, const std::string& help = "") {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = counters_.find(name);
        if (it != counters_.end()) return *it->second;
        auto c = std::make_unique<Counter>(name, help);
        auto& ref = *c;
        counters_[name] = std::move(c);
        return ref;
    }

    Gauge& gauge(const std::string& name, const std::string& help = "") {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = gauges_.find(name);
        if (it != gauges_.end()) return *it->second;
        auto g = std::make_unique<Gauge>(name, help);
        auto& ref = *g;
        gauges_[name] = std::move(g);
        return ref;
    }

    Histogram& histogram(const std::string& name, const std::string& help = "",
                         std::vector<double> buckets = {}) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = histograms_.find(name);
        if (it != histograms_.end()) return *it->second;
        auto h = buckets.empty()
            ? std::make_unique<Histogram>(name, help)
            : std::make_unique<Histogram>(name, help, std::move(buckets));
        auto& ref = *h;
        histograms_[name] = std::move(h);
        return ref;
    }

    // Export all metrics in Prometheus text exposition format
    std::string exportPrometheus() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::ostringstream out;

        for (auto& [name, c] : counters_) {
            if (!c->help().empty())
                out << "# HELP " << name << " " << c->help() << "\n";
            out << "# TYPE " << name << " counter\n";
            out << name << " " << c->value() << "\n";
        }

        for (auto& [name, g] : gauges_) {
            if (!g->help().empty())
                out << "# HELP " << name << " " << g->help() << "\n";
            out << "# TYPE " << name << " gauge\n";
            out << name << " " << g->value() << "\n";
        }

        for (auto& [name, h] : histograms_) {
            if (!h->help().empty())
                out << "# HELP " << name << " " << h->help() << "\n";
            out << "# TYPE " << name << " histogram\n";
            for (size_t i = 0; i < h->bounds().size(); ++i) {
                out << name << "_bucket{le=\"" << h->bounds()[i] << "\"} "
                    << h->bucketCount(i) << "\n";
            }
            out << name << "_bucket{le=\"+Inf\"} " << h->bucketCount(h->bounds().size()) << "\n";
            out << name << "_sum " << h->sum() << "\n";
            out << name << "_count " << h->count() << "\n";
        }

        return out.str();
    }

private:
    MetricsRegistry() = default;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<Counter>> counters_;
    std::unordered_map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::unordered_map<std::string, std::unique_ptr<Histogram>> histograms_;
};

// ─── Convenience macros ──────────────────────────────────────────────────────

#define METRICS_COUNTER(name, help) \
    OrderMatcher::MetricsRegistry::instance().counter(name, help)

#define METRICS_GAUGE(name, help) \
    OrderMatcher::MetricsRegistry::instance().gauge(name, help)

#define METRICS_HISTOGRAM(name, help) \
    OrderMatcher::MetricsRegistry::instance().histogram(name, help)

} // namespace OrderMatcher
