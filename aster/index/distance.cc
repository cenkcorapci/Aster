#include "aster/index/distance.h"

#include <cassert>
#include <cmath>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#define ASTER_HAVE_X86 1
#include <immintrin.h>
#endif

namespace aster {
namespace {

#if defined(ASTER_HAVE_X86)

// Horizontal sum of 8 floats in a YMM register. Must share the AVX2 target
// with callers so __m256 does not cross an ABI boundary.
__attribute__((target("avx2,fma"))) inline float HSum256(__m256 v) {
  const __m128 lo = _mm256_castps256_ps128(v);
  const __m128 hi = _mm256_extractf128_ps(v, 1);
  __m128 sum = _mm_add_ps(lo, hi);
  const __m128 shuf = _mm_movehdup_ps(sum);
  sum = _mm_add_ps(sum, shuf);
  const __m128 dup = _mm_movehl_ps(shuf, sum);
  sum = _mm_add_ss(sum, dup);
  return _mm_cvtss_f32(sum);
}

__attribute__((target("avx2,fma"))) float L2SquaredAvx2(VectorView a,
                                                        VectorView b) {
  assert(a.size() == b.size());
  const size_t n = a.size();
  const float* pa = a.data();
  const float* pb = b.data();

  __m256 acc = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256 va = _mm256_loadu_ps(pa + i);
    const __m256 vb = _mm256_loadu_ps(pb + i);
    const __m256 diff = _mm256_sub_ps(va, vb);
    acc = _mm256_fmadd_ps(diff, diff, acc);
  }
  float sum = HSum256(acc);
  for (; i < n; ++i) {
    const float d = pa[i] - pb[i];
    sum += d * d;
  }
  // Avoid leaking YMM dirty upper state into SSE-only code on older ABIs.
  _mm256_zeroupper();
  return sum;
}

__attribute__((target("avx2,fma"))) float DotAvx2(VectorView a, VectorView b) {
  assert(a.size() == b.size());
  const size_t n = a.size();
  const float* pa = a.data();
  const float* pb = b.data();

  __m256 acc = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256 va = _mm256_loadu_ps(pa + i);
    const __m256 vb = _mm256_loadu_ps(pb + i);
    acc = _mm256_fmadd_ps(va, vb, acc);
  }
  float sum = HSum256(acc);
  for (; i < n; ++i) sum += pa[i] * pb[i];
  _mm256_zeroupper();
  return sum;
}

#endif  // ASTER_HAVE_X86

using BinaryFn = float (*)(VectorView, VectorView);

BinaryFn SelectL2() {
#if defined(ASTER_HAVE_X86)
  if (CpuSupportsAvx2()) return &L2SquaredAvx2;
#endif
  return &L2SquaredScalar;
}

BinaryFn SelectDot() {
#if defined(ASTER_HAVE_X86)
  if (CpuSupportsAvx2()) return &DotAvx2;
#endif
  return &DotScalar;
}

}  // namespace

bool CpuSupportsAvx2() {
#if defined(ASTER_HAVE_X86) && (defined(__GNUC__) || defined(__clang__))
  // GCC may require an explicit init; Clang treats this as a cheap no-op when
  // CPUID state is already populated.
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  // ARM (incl. Apple Silicon) and other ISAs: scalar until M2-T09 (NEON).
  return false;
#endif
}

DistanceBackend ActiveDistanceBackend() {
  return CpuSupportsAvx2() ? DistanceBackend::kAvx2 : DistanceBackend::kScalar;
}

float L2SquaredScalar(VectorView a, VectorView b) {
  assert(a.size() == b.size());
  float sum = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    const float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

float DotScalar(VectorView a, VectorView b) {
  assert(a.size() == b.size());
  float sum = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) sum += a[i] * b[i];
  return sum;
}

float L2Squared(VectorView a, VectorView b) {
  static const BinaryFn kn = SelectL2();
  return kn(a, b);
}

float Dot(VectorView a, VectorView b) {
  static const BinaryFn kn = SelectDot();
  return kn(a, b);
}

float CosineSimilarity(VectorView a, VectorView b) {
  // Cosine = Dot / (|a| |b|); reuse dispatched Dot (+ norms via Dot(x,x)).
  assert(a.size() == b.size());
  const float denom = std::sqrt(Dot(a, a)) * std::sqrt(Dot(b, b));
  if (denom == 0.0f) return 0.0f;
  return Dot(a, b) / denom;
}

float CosineSimilarityPreNorm(VectorView a, float norm_a, VectorView b,
                              float norm_b) {
  assert(a.size() == b.size());
  if (norm_a == 0.0f || norm_b == 0.0f) return 0.0f;
  return Dot(a, b) / (norm_a * norm_b);
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
