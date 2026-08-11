#include <gtest/gtest.h>

#include "aster/qa/latency_bench_lib.h"

namespace aster {
namespace {

TEST(LatencyBenchSmoke, SmokeRunsAndReportsStats) {
#if !ASTER_ENABLE_HNSW
  GTEST_SKIP() << "HNSW disabled under Tiny profile";
#else
  aster::latency_bench::Result r;
  const bool ok =
      aster::latency_bench::Run(/*scale=*/"smoke", /*print=*/false,
                                 /*p50_threshold_ms=*/1000.0,
                                 /*out_json_path=*/"", &r);
  EXPECT_TRUE(ok);
  EXPECT_GT(r.measured, 0u);
  EXPECT_GT(r.p50_ms, 0.0);
#endif
}

}  // namespace
}  // namespace aster

