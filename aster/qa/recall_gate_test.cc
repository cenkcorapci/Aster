// M2-T10: PR / nightly recall@10 CI gate (ef_search=128 vs exact).

#include <gtest/gtest.h>

#include "aster/core/features.h"
#include "aster/qa/recall_gate_lib.h"

namespace aster {
namespace {

TEST(RecallGate, SiftAndGloveSubsetsMeetFloor) {
#if !ASTER_ENABLE_HNSW
  GTEST_SKIP() << "HNSW disabled under Tiny profile";
#else
  const auto scale_name = recall_gate::ScaleFromEnvOrDefault("pr");
  EXPECT_TRUE(recall_gate::Run(scale_name, /*print=*/true))
      << "mean recall@10 @ ef=128 must be >= 0.95 on SIFT- and GloVe-like "
         "subsets (scale="
      << scale_name << ")";
#endif
}

}  // namespace
}  // namespace aster
