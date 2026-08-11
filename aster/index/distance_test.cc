#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "aster/index/distance.h"

namespace aster {
namespace {

const char* BackendName(DistanceBackend b) {
  switch (b) {
    case DistanceBackend::kAvx512:
      return "avx512";
    case DistanceBackend::kAvx2:
      return "avx2";
    case DistanceBackend::kNeon:
      return "neon";
    case DistanceBackend::kScalar:
      return "scalar";
  }
  return "unknown";
}

std::vector<float> RandomVector(size_t dim, std::mt19937& rng) {
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<float> v(dim);
  for (float& x : v) x = dist(rng);
  return v;
}

TEST(DistanceDispatch, ReportsBackendConsistentWithCpu) {
  const DistanceBackend active = ActiveDistanceBackend();
  // Preference: AVX-512 > AVX2 > NEON > scalar.
  if (CpuSupportsAvx512()) {
    EXPECT_EQ(active, DistanceBackend::kAvx512);
  } else if (CpuSupportsAvx2()) {
    EXPECT_EQ(active, DistanceBackend::kAvx2);
  } else if (CpuSupportsNeon()) {
    EXPECT_EQ(active, DistanceBackend::kNeon);
  } else {
    EXPECT_EQ(active, DistanceBackend::kScalar);
  }

  std::printf(
      "M2-T09: ActiveDistanceBackend=%s (avx512=%d avx2=%d neon=%d)\n",
      BackendName(active), CpuSupportsAvx512() ? 1 : 0,
      CpuSupportsAvx2() ? 1 : 0, CpuSupportsNeon() ? 1 : 0);

#if defined(__aarch64__) || defined(__arm__)
  EXPECT_TRUE(CpuSupportsNeon());
  EXPECT_EQ(active, DistanceBackend::kNeon);
  std::printf(
      "M2-T09: Apple Silicon / ARM host — dispatch selects NEON "
      "(ActiveDistanceBackend=kNeon).\n");
#endif
}

class DistanceSimdMatch : public ::testing::TestWithParam<size_t> {};

TEST_P(DistanceSimdMatch, L2AndDotMatchScalar) {
  const size_t dim = GetParam();
  std::mt19937 rng(static_cast<uint32_t>(9000 + dim));
  constexpr int kTrials = 64;
  // Tight relative/absolute tolerance for float32 reductions.
  constexpr float kAbsTol = 1e-5f;
  constexpr float kRelTol = 1e-5f;

  for (int t = 0; t < kTrials; ++t) {
    const auto a = RandomVector(dim, rng);
    const auto b = RandomVector(dim, rng);

    const float l2_ref = L2SquaredScalar(a, b);
    const float l2 = L2Squared(a, b);
    const float l2_tol = kAbsTol + kRelTol * std::fabs(l2_ref);
    EXPECT_NEAR(l2, l2_ref, l2_tol) << "dim=" << dim << " trial=" << t;

    const float dot_ref = DotScalar(a, b);
    const float dot = Dot(a, b);
    const float dot_tol = kAbsTol + kRelTol * std::fabs(dot_ref);
    EXPECT_NEAR(dot, dot_ref, dot_tol) << "dim=" << dim << " trial=" << t;

    const float cos = CosineSimilarity(a, b);
    const float denom =
        std::sqrt(DotScalar(a, a)) * std::sqrt(DotScalar(b, b));
    const float cos_ref = (denom == 0.0f) ? 0.0f : DotScalar(a, b) / denom;
    const float cos_tol = kAbsTol + kRelTol * std::fabs(cos_ref);
    EXPECT_NEAR(cos, cos_ref, cos_tol) << "dim=" << dim << " trial=" << t;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Dims, DistanceSimdMatch,
    ::testing::Values(
        1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 128, 384, 1536),
    [](const ::testing::TestParamInfo<size_t>& info) {
      return "dim_" + std::to_string(info.param);
    });

TEST(DistanceMicrobench, SimdFasterThanScalarWhenAvailable) {
  constexpr size_t kDim = 384;
  constexpr int kWarmup = 200;
  constexpr int kIters = 20000;

  std::mt19937 rng(42);
  const auto a = RandomVector(kDim, rng);
  const auto b = RandomVector(kDim, rng);

  // Touch both paths so the first timed call isn't cold-I$.
  volatile float sink = 0.0f;
  for (int i = 0; i < kWarmup; ++i) {
    sink += L2SquaredScalar(a, b);
    sink += L2Squared(a, b);
    sink += DotScalar(a, b);
    sink += Dot(a, b);
  }

  const auto time_fn = [&](auto&& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) sink += fn(a, b);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() /
           static_cast<double>(kIters);
  };

  const double scalar_l2_ns = time_fn([](const auto& x, const auto& y) {
    return L2SquaredScalar(x, y);
  });
  const double dispatch_l2_ns =
      time_fn([](const auto& x, const auto& y) { return L2Squared(x, y); });
  const double scalar_dot_ns =
      time_fn([](const auto& x, const auto& y) { return DotScalar(x, y); });
  const double dispatch_dot_ns =
      time_fn([](const auto& x, const auto& y) { return Dot(x, y); });

  const DistanceBackend backend = ActiveDistanceBackend();
  std::printf(
      "M2-T09 microbench dim=%zu iters=%d: L2 scalar=%.1fns dispatch=%.1fns "
      "(%.2fx); Dot scalar=%.1fns dispatch=%.1fns (%.2fx); backend=%s; "
      "sink=%g\n",
      kDim, kIters, scalar_l2_ns, dispatch_l2_ns,
      scalar_l2_ns / dispatch_l2_ns, scalar_dot_ns, dispatch_dot_ns,
      scalar_dot_ns / dispatch_dot_ns, BackendName(backend),
      static_cast<double>(sink));

  if (backend == DistanceBackend::kScalar) {
    GTEST_SKIP()
        << "SIMD unavailable — speedup check skipped (scalar==dispatch)";
  }

  // On a supported CPU, SIMD should beat scalar for 384-d by a clear margin.
  // Use a conservative 1.3x floor so CI noise doesn't flake.
  EXPECT_GT(scalar_l2_ns / dispatch_l2_ns, 1.3);
  EXPECT_GT(scalar_dot_ns / dispatch_dot_ns, 1.3);
}

}  // namespace
}  // namespace aster
