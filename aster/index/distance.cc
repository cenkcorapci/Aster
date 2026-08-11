#include "aster/index/distance.h"

#include <cassert>
#include <cmath>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#define ASTER_HAVE_X86 1
#include <immintrin.h>
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
#define ASTER_HAVE_NEON 1
#include <arm_neon.h>
#endif

namespace aster {
namespace {

#if defined(ASTER_HAVE_X86)

// --- AVX-512 (16-wide) -----------------------------------------------------

__attribute__((target("avx512f,fma"))) inline float HSum512(__m512 v) {
  return _mm512_reduce_add_ps(v);
}

__attribute__((target("avx512f,fma"))) float L2SquaredAvx512(VectorView a,
                                                             VectorView b) {
  assert(a.size() == b.size());
  const size_t n = a.size();
  const float* pa = a.data();
  const float* pb = b.data();

  __m512 acc = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    const __m512 va = _mm512_loadu_ps(pa + i);
    const __m512 vb = _mm512_loadu_ps(pb + i);
    const __m512 diff = _mm512_sub_ps(va, vb);
    acc = _mm512_fmadd_ps(diff, diff, acc);
  }
  float sum = HSum512(acc);
  for (; i < n; ++i) {
    const float d = pa[i] - pb[i];
    sum += d * d;
  }
  _mm256_zeroupper();
  return sum;
}

__attribute__((target("avx512f,fma"))) float DotAvx512(VectorView a,
                                                       VectorView b) {
  assert(a.size() == b.size());
  const size_t n = a.size();
  const float* pa = a.data();
  const float* pb = b.data();

  __m512 acc = _mm512_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    const __m512 va = _mm512_loadu_ps(pa + i);
    const __m512 vb = _mm512_loadu_ps(pb + i);
    acc = _mm512_fmadd_ps(va, vb, acc);
  }
  float sum = HSum512(acc);
  for (; i < n; ++i) sum += pa[i] * pb[i];
  _mm256_zeroupper();
  return sum;
}

// --- AVX2 (8-wide) ---------------------------------------------------------

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

#if defined(ASTER_HAVE_NEON)

// --- ARM NEON (4-wide) -----------------------------------------------------

inline float HSum128(float32x4_t v) {
  const float32x2_t sum2 = vadd_f32(vget_low_f32(v), vget_high_f32(v));
  return vget_lane_f32(vpadd_f32(sum2, sum2), 0);
}

float L2SquaredNeon(VectorView a, VectorView b) {
  assert(a.size() == b.size());
  const size_t n = a.size();
  const float* pa = a.data();
  const float* pb = b.data();

  float32x4_t acc = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const float32x4_t va = vld1q_f32(pa + i);
    const float32x4_t vb = vld1q_f32(pb + i);
    const float32x4_t diff = vsubq_f32(va, vb);
    acc = vfmaq_f32(acc, diff, diff);
  }
  float sum = HSum128(acc);
  for (; i < n; ++i) {
    const float d = pa[i] - pb[i];
    sum += d * d;
  }
  return sum;
}

float DotNeon(VectorView a, VectorView b) {
  assert(a.size() == b.size());
  const size_t n = a.size();
  const float* pa = a.data();
  const float* pb = b.data();

  float32x4_t acc = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const float32x4_t va = vld1q_f32(pa + i);
    const float32x4_t vb = vld1q_f32(pb + i);
    acc = vfmaq_f32(acc, va, vb);
  }
  float sum = HSum128(acc);
  for (; i < n; ++i) sum += pa[i] * pb[i];
  return sum;
}

#endif  // ASTER_HAVE_NEON

using BinaryFn = float (*)(VectorView, VectorView);

BinaryFn SelectL2() {
#if defined(ASTER_HAVE_X86)
  if (CpuSupportsAvx512()) return &L2SquaredAvx512;
  if (CpuSupportsAvx2()) return &L2SquaredAvx2;
#endif
#if defined(ASTER_HAVE_NEON)
  if (CpuSupportsNeon()) return &L2SquaredNeon;
#endif
  return &L2SquaredScalar;
}

BinaryFn SelectDot() {
#if defined(ASTER_HAVE_X86)
  if (CpuSupportsAvx512()) return &DotAvx512;
  if (CpuSupportsAvx2()) return &DotAvx2;
#endif
#if defined(ASTER_HAVE_NEON)
  if (CpuSupportsNeon()) return &DotNeon;
#endif
  return &DotScalar;
}

}  // namespace

bool CpuSupportsAvx512() {
#if defined(ASTER_HAVE_X86) && (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx512f");
#else
  return false;
#endif
}

bool CpuSupportsAvx2() {
#if defined(ASTER_HAVE_X86) && (defined(__GNUC__) || defined(__clang__))
  // GCC may require an explicit init; Clang treats this as a cheap no-op when
  // CPUID state is already populated.
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  return false;
#endif
}

bool CpuSupportsNeon() {
#if defined(ASTER_HAVE_NEON)
  // aarch64 mandates NEON; 32-bit ARM only reaches here when __ARM_NEON is set.
  return true;
#else
  return false;
#endif
}

DistanceBackend ActiveDistanceBackend() {
  if (CpuSupportsAvx512()) return DistanceBackend::kAvx512;
  if (CpuSupportsAvx2()) return DistanceBackend::kAvx2;
  if (CpuSupportsNeon()) return DistanceBackend::kNeon;
  return DistanceBackend::kScalar;
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
