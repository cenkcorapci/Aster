#pragma once

#include "aster/core/types.h"

namespace aster {

// Distance kernels with runtime SIMD dispatch (AVX2 when available; scalar
// elsewhere). NEON / AVX-512 land in M2-T09. Scalar remains the portable
// fallback and the correctness reference.

enum class DistanceBackend {
  kScalar,
  kAvx2,
};

// True when this process can safely run AVX2 distance kernels (x86 + CPUID).
bool CpuSupportsAvx2();
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
