#pragma once

#include "aster/core/types.h"

namespace aster {

// Scalar distance kernels. SIMD backends (AVX2/AVX-512/NEON) plug in behind
// the same signatures per docs/design.md; scalar is the portable fallback.

float L2Squared(VectorView a, VectorView b);
float Dot(VectorView a, VectorView b);
float CosineSimilarity(VectorView a, VectorView b);

// Uniform "higher is better" score for a metric, so that top-k merging
// across segments and replicas never has to branch on metric direction.
//   kL2     -> -L2Squared
//   kDot    -> Dot
//   kCosine -> CosineSimilarity
float Score(Metric metric, VectorView a, VectorView b);

}  // namespace aster
