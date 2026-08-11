#pragma once

#include "aster/core/types.h"

namespace aster {

// Distance kernels with runtime SIMD dispatch. Preference order:
//   AVX-512 > AVX2 > NEON > scalar
// Scalar remains the portable fallback and the correctness reference.

enum class DistanceBackend {
  kScalar,
  kNeon,
  kAvx2,
  kAvx512,
};

// CPU feature probes used by dispatch and tests.
bool CpuSupportsAvx512();
bool CpuSupportsAvx2();
bool CpuSupportsNeon();
DistanceBackend ActiveDistanceBackend();

// Dispatched public API.
float L2Squared(VectorView a, VectorView b);
float Dot(VectorView a, VectorView b);
float CosineSimilarity(VectorView a, VectorView b);
// Fast path when both L2 norms are already known (exact index build caches
// database-side norms; callers pass the query norm once per Search).
float CosineSimilarityPreNorm(VectorView a, float norm_a, VectorView b,
                              float norm_b);

// Explicit scalar reference implementations (always available; used by tests
// and as the dispatch fallback).
float L2SquaredScalar(VectorView a, VectorView b);
float DotScalar(VectorView a, VectorView b);

// Uniform "higher is better" score for a metric, so that top-k merging
// across segments and replicas never has to branch on metric direction.
//   kL2     -> -L2Squared
//   kDot    -> Dot
//   kCosine -> CosineSimilarity
float Score(Metric metric, VectorView a, VectorView b);

}  // namespace aster
