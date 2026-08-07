#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aster {

// Prometheus counter (monotonically increasing).
class Counter {
 public:
  void Inc(uint64_t delta = 1);
  uint64_t Value() const;

 private:
  std::atomic<uint64_t> value_{0};
};

// Prometheus gauge (arbitrary up/down).
class Gauge {
 public:
  void Set(int64_t value);
  void Inc(int64_t delta = 1);
  void Dec(int64_t delta = 1);
  int64_t Value() const;

 private:
  std::atomic<int64_t> value_{0};
};

// Prometheus histogram with cumulative buckets + _sum / _count.
class Histogram {
 public:
  explicit Histogram(std::vector<double> buckets);

  void Observe(double value);

  const std::vector<double>& Buckets() const { return buckets_; }
  uint64_t BucketCount(size_t i) const;
  uint64_t InfCount() const;
  uint64_t Count() const;
  double Sum() const;

 private:
  std::vector<double> buckets_;
  std::vector<std::atomic<uint64_t>> counts_;
  std::atomic<uint64_t> inf_count_{0};
  std::atomic<uint64_t> count_{0};
  std::atomic<uint64_t> sum_millis_x1000_{0};  // fixed-point: value * 1000
};

// Default millisecond latency bucket upper bounds (excluding +Inf).
std::vector<double> DefaultLatencyBucketsMs();

// Metrics registry with Prometheus text-format Render()
// (docs/design.md, "Monitoring"). Key metrics are registered at construction.
class MetricsRegistry {
 public:
  MetricsRegistry();

  static MetricsRegistry& Instance();

  ::aster::Counter& Counter(const std::string& name);
  ::aster::Gauge& Gauge(const std::string& name);
  ::aster::Histogram& Histogram(const std::string& name);

  // Renders Prometheus text exposition format (HELP/TYPE + samples).
  std::string Render() const;

 private:
  void RegisterDefaults();

  std::map<std::string, std::unique_ptr<::aster::Counter>> counters_;
  std::map<std::string, std::unique_ptr<::aster::Gauge>> gauges_;
  std::map<std::string, std::unique_ptr<::aster::Histogram>> histograms_;
  std::map<std::string, std::string> help_;
};

}  // namespace aster
