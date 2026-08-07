#include <gtest/gtest.h>

#include <string>

#include "aster/metrics/metrics.h"

namespace aster {
namespace {

TEST(MetricsRegistry, RegistersKeyMetricsWithTypes) {
  MetricsRegistry registry;
  const std::string text = registry.Render();

  EXPECT_NE(text.find("# TYPE read_latency_ms histogram"), std::string::npos);
  EXPECT_NE(text.find("# TYPE write_latency_ms histogram"), std::string::npos);
  EXPECT_NE(text.find("# TYPE hnsw_search_latency histogram"),
            std::string::npos);
  EXPECT_NE(text.find("# TYPE segment_count gauge"), std::string::npos);
  EXPECT_NE(text.find("# TYPE compaction_backlog gauge"), std::string::npos);
  EXPECT_NE(text.find("# TYPE gossip_messages counter"), std::string::npos);
  EXPECT_NE(text.find("# TYPE replication_lag gauge"), std::string::npos);

  EXPECT_NE(text.find("# HELP read_latency_ms read latency"),
            std::string::npos);
  EXPECT_NE(text.find("# HELP gossip_messages cluster traffic"),
            std::string::npos);
}

TEST(MetricsRegistry, CounterAndGaugeSamples) {
  MetricsRegistry registry;
  registry.Counter("gossip_messages").Inc(3);
  registry.Gauge("segment_count").Set(7);
  registry.Gauge("compaction_backlog").Inc(2);
  registry.Gauge("replication_lag").Set(42);

  const std::string text = registry.Render();
  EXPECT_NE(text.find("gossip_messages 3\n"), std::string::npos);
  EXPECT_NE(text.find("segment_count 7\n"), std::string::npos);
  EXPECT_NE(text.find("compaction_backlog 2\n"), std::string::npos);
  EXPECT_NE(text.find("replication_lag 42\n"), std::string::npos);
}

TEST(MetricsRegistry, HistogramObserveAndBuckets) {
  MetricsRegistry registry;
  auto& hist = registry.Histogram("read_latency_ms");
  hist.Observe(3.0);
  hist.Observe(12.0);

  const std::string text = registry.Render();
  EXPECT_NE(text.find("read_latency_ms_bucket{le=\"1\"} 0\n"),
            std::string::npos);
  EXPECT_NE(text.find("read_latency_ms_bucket{le=\"5\"} 1\n"),
            std::string::npos);
  EXPECT_NE(text.find("read_latency_ms_bucket{le=\"10\"} 1\n"),
            std::string::npos);
  EXPECT_NE(text.find("read_latency_ms_bucket{le=\"25\"} 2\n"),
            std::string::npos);
  EXPECT_NE(text.find("read_latency_ms_bucket{le=\"+Inf\"} 2\n"),
            std::string::npos);
  EXPECT_NE(text.find("read_latency_ms_sum 15\n"), std::string::npos);
  EXPECT_NE(text.find("read_latency_ms_count 2\n"), std::string::npos);
}

TEST(MetricsRegistry, WriteAndHnswHistogramsPresent) {
  MetricsRegistry registry;
  registry.Histogram("write_latency_ms").Observe(1.5);
  registry.Histogram("hnsw_search_latency").Observe(8.0);

  const std::string text = registry.Render();
  EXPECT_NE(text.find("write_latency_ms_count 1\n"), std::string::npos);
  EXPECT_NE(text.find("hnsw_search_latency_count 1\n"), std::string::npos);
  EXPECT_NE(text.find("write_latency_ms_sum 1.5\n"), std::string::npos);
}

}  // namespace
}  // namespace aster
