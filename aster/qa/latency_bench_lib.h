#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "aster/core/types.h"

namespace aster {
namespace latency_bench {

struct Result {
  // Latency statistics for the timed search loop (milliseconds).
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double avg_ms = 0.0;

  // Bench metadata (so CI artifacts are self-describing).
  std::string scale;
  uint32_t dim = 0;
  uint64_t n_base = 0;
  uint32_t n_query = 0;
  uint32_t top_k = 0;
  uint32_t ef_search = 0;
  uint32_t warmup = 0;
  uint32_t measured = 0;
  Metric metric = Metric::kL2;

  double p50_threshold_ms = 5.0;
  bool pass = false;
};

// Deterministic latency bench for ANN search.
//
// `scale` selects a fixed dataset + workload configuration.
// When `out_json_path` is non-empty, the bench writes one JSON object.
// Returns true when `p50_ms < p50_threshold_ms`.
bool Run(std::string_view scale, bool print, double p50_threshold_ms,
         std::string_view out_json_path, Result* out);

}  // namespace latency_bench
}  // namespace aster

