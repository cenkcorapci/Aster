#include <cstdlib>
#include <string_view>

#include <gtest/gtest.h>

#include "aster/qa/latency_bench_lib.h"

namespace aster {
namespace {

double EnvOrDefault(std::string_view name, double def) {
  const char* v = std::getenv(std::string(name).c_str());
  if (!v || v[0] == '\0') return def;
  return std::strtod(v, nullptr);
}

TEST(LatencyBenchGateCi, P50Below5ms) {
#if !ASTER_ENABLE_HNSW
  GTEST_SKIP() << "HNSW disabled under Tiny profile";
#else
  constexpr double kDefaultThresholdMs = 5.0;
  const double thr = EnvOrDefault("ASTER_LATENCY_P50_THRESHOLD_MS",
                                  kDefaultThresholdMs);

  aster::latency_bench::Result r;
  const bool ok = aster::latency_bench::Run(/*scale=*/"ci", /*print=*/true,
                                            /*p50_threshold_ms=*/thr,
                                            /*out_json_path=*/"",
                                            &r);
  EXPECT_TRUE(ok) << "Expected p50_ms < " << thr << " but got "
                    << r.p50_ms;
#endif
}

}  // namespace
}  // namespace aster

