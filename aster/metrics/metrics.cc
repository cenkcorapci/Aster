#include "aster/metrics/metrics.h"

#include <limits>
#include <sstream>

namespace aster {
namespace {

std::string FormatDouble(double v) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(6);
  oss << v;
  std::string s = oss.str();
  // Trim trailing zeros after the decimal point for cleaner scrapes.
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') {
      s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
      s.pop_back();
    }
  }
  return s.empty() ? "0" : s;
}

std::string FormatLe(double le) {
  if (le == std::numeric_limits<double>::infinity()) {
    return "+Inf";
  }
  return FormatDouble(le);
}

}  // namespace

void Counter::Inc(uint64_t delta) {
  value_.fetch_add(delta, std::memory_order_relaxed);
}

uint64_t Counter::Value() const {
  return value_.load(std::memory_order_relaxed);
}

void Gauge::Set(int64_t value) {
  value_.store(value, std::memory_order_relaxed);
}

void Gauge::Inc(int64_t delta) {
  value_.fetch_add(delta, std::memory_order_relaxed);
}

void Gauge::Dec(int64_t delta) {
  value_.fetch_sub(delta, std::memory_order_relaxed);
}

int64_t Gauge::Value() const {
  return value_.load(std::memory_order_relaxed);
}

Histogram::Histogram(std::vector<double> buckets)
    : buckets_(std::move(buckets)), counts_(buckets_.size()) {
  for (auto& c : counts_) {
    c.store(0, std::memory_order_relaxed);
  }
}

void Histogram::Observe(double value) {
  count_.fetch_add(1, std::memory_order_relaxed);
  const auto scaled =
      static_cast<uint64_t>(value * 1000.0 < 0 ? 0 : value * 1000.0);
  sum_millis_x1000_.fetch_add(scaled, std::memory_order_relaxed);

  for (size_t i = 0; i < buckets_.size(); ++i) {
    if (value <= buckets_[i]) {
      counts_[i].fetch_add(1, std::memory_order_relaxed);
    }
  }
  inf_count_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t Histogram::BucketCount(size_t i) const {
  return counts_[i].load(std::memory_order_relaxed);
}

uint64_t Histogram::InfCount() const {
  return inf_count_.load(std::memory_order_relaxed);
}

uint64_t Histogram::Count() const {
  return count_.load(std::memory_order_relaxed);
}

double Histogram::Sum() const {
  return static_cast<double>(sum_millis_x1000_.load(std::memory_order_relaxed)) /
         1000.0;
}

std::vector<double> DefaultLatencyBucketsMs() {
  return {1, 2.5, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
}

MetricsRegistry::MetricsRegistry() { RegisterDefaults(); }

MetricsRegistry& MetricsRegistry::Instance() {
  static MetricsRegistry instance;
  return instance;
}

void MetricsRegistry::RegisterDefaults() {
  const auto latency_buckets = DefaultLatencyBucketsMs();

  auto add_histogram = [this, &latency_buckets](const std::string& name,
                                                const std::string& help) {
    histograms_[name] =
        std::make_unique<::aster::Histogram>(latency_buckets);
    help_[name] = help;
  };
  auto add_gauge = [this](const std::string& name, const std::string& help) {
    gauges_[name] = std::make_unique<::aster::Gauge>();
    help_[name] = help;
  };
  auto add_counter = [this](const std::string& name, const std::string& help) {
    counters_[name] = std::make_unique<::aster::Counter>();
    help_[name] = help;
  };

  // Key metrics from docs/design.md "Monitoring".
  add_histogram("read_latency_ms", "read latency");
  add_histogram("write_latency_ms", "write latency");
  add_histogram("hnsw_search_latency", "ANN latency");
  add_gauge("segment_count", "LSM segments");
  add_gauge("compaction_backlog", "pending compaction");
  add_counter("gossip_messages", "cluster traffic");
  add_gauge("replication_lag", "replica delay");
}

::aster::Counter& MetricsRegistry::Counter(const std::string& name) {
  auto it = counters_.find(name);
  if (it == counters_.end()) {
    counters_[name] = std::make_unique<::aster::Counter>();
    help_.emplace(name, name);
    it = counters_.find(name);
  }
  return *it->second;
}

::aster::Gauge& MetricsRegistry::Gauge(const std::string& name) {
  auto it = gauges_.find(name);
  if (it == gauges_.end()) {
    gauges_[name] = std::make_unique<::aster::Gauge>();
    help_.emplace(name, name);
    it = gauges_.find(name);
  }
  return *it->second;
}

::aster::Histogram& MetricsRegistry::Histogram(const std::string& name) {
  auto it = histograms_.find(name);
  if (it == histograms_.end()) {
    histograms_[name] =
        std::make_unique<::aster::Histogram>(DefaultLatencyBucketsMs());
    help_.emplace(name, name);
    it = histograms_.find(name);
  }
  return *it->second;
}

std::string MetricsRegistry::Render() const {
  std::string out;

  auto help_for = [this](const std::string& name) -> const std::string& {
    static const std::string kEmpty;
    auto it = help_.find(name);
    return it == help_.end() ? kEmpty : it->second;
  };

  for (const auto& [name, counter] : counters_) {
    const auto& help = help_for(name);
    if (!help.empty()) {
      out += "# HELP " + name + " " + help + "\n";
    }
    out += "# TYPE " + name + " counter\n";
    out += name + " " + std::to_string(counter->Value()) + "\n";
  }

  for (const auto& [name, gauge] : gauges_) {
    const auto& help = help_for(name);
    if (!help.empty()) {
      out += "# HELP " + name + " " + help + "\n";
    }
    out += "# TYPE " + name + " gauge\n";
    out += name + " " + std::to_string(gauge->Value()) + "\n";
  }

  for (const auto& [name, hist] : histograms_) {
    const auto& help = help_for(name);
    if (!help.empty()) {
      out += "# HELP " + name + " " + help + "\n";
    }
    out += "# TYPE " + name + " histogram\n";
    const auto& buckets = hist->Buckets();
    for (size_t i = 0; i < buckets.size(); ++i) {
      out += name + "_bucket{le=\"" + FormatLe(buckets[i]) + "\"} " +
             std::to_string(hist->BucketCount(i)) + "\n";
    }
    out += name + "_bucket{le=\"+Inf\"} " +
           std::to_string(hist->InfCount()) + "\n";
    out += name + "_sum " + FormatDouble(hist->Sum()) + "\n";
    out += name + "_count " + std::to_string(hist->Count()) + "\n";
  }

  return out;
}

}  // namespace aster
