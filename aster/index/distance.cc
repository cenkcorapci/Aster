#include "aster/index/distance.h"

#include <cassert>
#include <cmath>

namespace aster {

float L2Squared(VectorView a, VectorView b) {
  assert(a.size() == b.size());
  float sum = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    const float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

float Dot(VectorView a, VectorView b) {
  assert(a.size() == b.size());
  float sum = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) sum += a[i] * b[i];
  return sum;
}

float CosineSimilarity(VectorView a, VectorView b) {
  assert(a.size() == b.size());
  float dot = 0.0f, na = 0.0f, nb = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
  const float denom = std::sqrt(na) * std::sqrt(nb);
  if (denom == 0.0f) return 0.0f;
  return dot / denom;
}

float Score(Metric metric, VectorView a, VectorView b) {
  switch (metric) {
    case Metric::kL2:
      return -L2Squared(a, b);
    case Metric::kDot:
      return Dot(a, b);
    case Metric::kCosine:
      return CosineSimilarity(a, b);
  }
  return 0.0f;
}

}  // namespace aster
