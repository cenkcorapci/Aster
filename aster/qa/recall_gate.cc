// M2-T10: CLI for the HNSW recall@10 gate.
//
//   bazel run //aster/qa:recall_gate -- [--scale=pr|nightly]
//   ASTER_RECALL_SCALE=nightly bazel run //aster/qa:recall_gate

#include <cstdio>
#include <cstring>
#include <string_view>

#include "aster/core/features.h"
#include "aster/qa/recall_gate_lib.h"

int main(int argc, char** argv) {
  const char* scale_arg = "pr";
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a.rfind("--scale=", 0) == 0) {
      scale_arg = argv[i] + std::strlen("--scale=");
    } else if (a == "--scale" && i + 1 < argc) {
      scale_arg = argv[++i];
    } else if (a == "--help" || a == "-h") {
      std::printf(
          "Usage: recall_gate [--scale=pr|nightly]\n"
          "Env: ASTER_RECALL_SCALE=pr|nightly\n"
          "Gate: mean recall@10 >= 0.95 at ef_search=128 vs exact.\n"
          "Corpora: deterministic SIFT1M-like (L2, 128-d) and GloVe-like "
          "(cosine, 100-d) subsets.\n");
      return 0;
    }
  }

#if !ASTER_ENABLE_HNSW
  std::fprintf(stderr, "recall_gate: HNSW disabled (Tiny profile)\n");
  return 1;
#else
  const auto scale_name = aster::recall_gate::ScaleFromEnvOrDefault(scale_arg);
  const bool ok = aster::recall_gate::Run(scale_name, /*print=*/true);
  return ok ? 0 : 2;
#endif
}
