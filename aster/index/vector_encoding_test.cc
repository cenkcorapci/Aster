#include "aster/index/vector_encoding.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "gtest/gtest.h"

namespace aster {
namespace {

std::vector<float> MakeVector(uint32_t dim, uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> dist(0.0f, 1.0f);
  std::vector<float> v(dim);
  for (uint32_t i = 0; i < dim; ++i) v[i] = dist(rng);
  return v;
}

float MaxAbsErr(const std::vector<float>& a, const std::vector<float>& b) {
  float m = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    m = std::max(m, std::fabs(a[i] - b[i]));
  }
  return m;
}

float Rmse(const std::vector<float>& a, const std::vector<float>& b) {
  double s = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double d = static_cast<double>(a[i] - b[i]);
    s += d * d;
  }
  return static_cast<float>(std::sqrt(s / static_cast<double>(a.size())));
}

TEST(VectorEncoding, NamesAndKnown) {
  EXPECT_STREQ(VectorEncodingName(VectorEncoding::kFloat32), "float32");
  EXPECT_STREQ(VectorEncodingName(VectorEncoding::kFloat16), "float16");
  EXPECT_STREQ(VectorEncodingName(VectorEncoding::kInt8Sq), "int8_sq");
  EXPECT_STREQ(VectorEncodingName(VectorEncoding::kBinaryRabitq),
               "binary_rabitq");
  EXPECT_TRUE(IsKnownVectorEncoding(0));
  EXPECT_TRUE(IsKnownVectorEncoding(3));
  EXPECT_FALSE(IsKnownVectorEncoding(4));
  EXPECT_FALSE(IsKnownVectorEncoding(255));
}

TEST(VectorEncoding, SizeBudget200kDim512) {
  constexpr uint32_t kDim = 512;
  constexpr uint64_t kN = 200000;

  EXPECT_EQ(PackedPayloadBytes(VectorEncoding::kFloat32, kDim, kN),
            kN * kDim * 4);
  EXPECT_EQ(PackedPayloadBytes(VectorEncoding::kFloat16, kDim, kN),
            kN * kDim * 2);
  EXPECT_EQ(PackedPayloadBytes(VectorEncoding::kInt8Sq, kDim, kN),
            kN * (kDim + 4));
  EXPECT_EQ(PackedPayloadBytes(VectorEncoding::kBinaryRabitq, kDim, kN),
            kN * (((kDim + 7) / 8) + 4));

  // Sanity: int8 ≈ 103.2e6 B (~98.4 MiB), binary ≈ 13.6e6 B for 200k×512.
  const uint64_t int8_bytes =
      PackedPayloadBytes(VectorEncoding::kInt8Sq, kDim, kN);
  const uint64_t bin_bytes =
      PackedPayloadBytes(VectorEncoding::kBinaryRabitq, kDim, kN);
  EXPECT_EQ(int8_bytes, 103200000u);
  EXPECT_EQ(bin_bytes, 13600000u);
  EXPECT_LT(int8_bytes, 110ull << 20);
  EXPECT_LT(bin_bytes, 15ull << 20);

  // vs float32: ≥4× and ≥28× respectively.
  const uint64_t f32 =
      PackedPayloadBytes(VectorEncoding::kFloat32, kDim, kN);
  EXPECT_GE(f32 / int8_bytes, 3u);
  EXPECT_GE(f32 / bin_bytes, 28u);
}

TEST(Float16, RoundTripCommon) {
  const float samples[] = {0.f, 1.f, -1.f, 0.5f, -0.5f, 65504.f, -65504.f,
                           1e-4f, 3.14159f, -42.5f};
  for (float x : samples) {
    const uint16_t bits = Float32ToFloat16Bits(x);
    const float y = Float16BitsToFloat32(bits);
    if (x == 0.f) {
      EXPECT_FLOAT_EQ(y, 0.f);
    } else {
      EXPECT_NEAR(y, x, std::fabs(x) * 0.001f + 1e-3f) << "x=" << x;
    }
  }
}

TEST(CompactVectorStore, Float32Identity) {
  constexpr uint32_t kDim = 8;
  auto a = MakeVector(kDim, 1);
  auto b = MakeVector(kDim, 2);
  std::vector<float> data = a;
  data.insert(data.end(), b.begin(), b.end());

  auto store = CompactVectorStore::Encode(VectorEncoding::kFloat32, kDim,
                                          data.data(), 2);
  ASSERT_TRUE(store.ok()) << store.status().message();
  EXPECT_EQ(store.value().BytesUsed(), 2u * kDim * 4u);

  auto got = store.value().Decode(1);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), b);
}

TEST(CompactVectorStore, Float16RoundTrip) {
  constexpr uint32_t kDim = 64;
  auto src = MakeVector(kDim, 42);
  // Keep values in float16-friendly range.
  for (float& x : src) x = std::clamp(x, -100.f, 100.f);

  auto store =
      CompactVectorStore::Encode(VectorEncoding::kFloat16, kDim, src.data(), 1);
  ASSERT_TRUE(store.ok());
  EXPECT_EQ(store.value().BytesUsed(), kDim * 2u);

  auto got = store.value().Decode(0);
  ASSERT_TRUE(got.ok());
  EXPECT_LT(MaxAbsErr(src, got.value()), 0.1f);
  EXPECT_LT(Rmse(src, got.value()), 0.05f);
}

TEST(CompactVectorStore, Int8SqRoundTripAndSize) {
  constexpr uint32_t kDim = 512;
  constexpr size_t kN = 32;
  std::vector<float> data;
  data.reserve(kN * kDim);
  for (size_t i = 0; i < kN; ++i) {
    auto v = MakeVector(kDim, static_cast<uint32_t>(1000 + i));
    data.insert(data.end(), v.begin(), v.end());
  }

  auto store = CompactVectorStore::Encode(VectorEncoding::kInt8Sq, kDim,
                                          data.data(), kN);
  ASSERT_TRUE(store.ok());
  EXPECT_EQ(store.value().BytesUsed(), kN * (kDim + 4));

  double total_rmse = 0.0;
  for (size_t i = 0; i < kN; ++i) {
    auto got = store.value().Decode(i);
    ASSERT_TRUE(got.ok());
    std::vector<float> src(data.begin() + static_cast<std::ptrdiff_t>(i * kDim),
                           data.begin() +
                               static_cast<std::ptrdiff_t>((i + 1) * kDim));
    total_rmse += Rmse(src, got.value());
  }
  EXPECT_LT(total_rmse / static_cast<double>(kN), 0.05);
}

TEST(CompactVectorStore, BinaryRabitqSizeAndEnergy) {
  constexpr uint32_t kDim = 512;
  auto src = MakeVector(kDim, 7);
  float norm = 0.f;
  for (float x : src) norm += x * x;
  norm = std::sqrt(norm);

  auto store = CompactVectorStore::Encode(VectorEncoding::kBinaryRabitq, kDim,
                                          src.data(), 1);
  ASSERT_TRUE(store.ok());
  EXPECT_EQ(store.value().BytesUsed(), ((kDim + 7) / 8) + 4);

  auto got = store.value().Decode(0);
  ASSERT_TRUE(got.ok());
  float got_norm = 0.f;
  for (float x : got.value()) got_norm += x * x;
  got_norm = std::sqrt(got_norm);
  EXPECT_NEAR(got_norm, norm, norm * 0.05f + 1e-3f);

  // Cosine between original and reconstruction should be strongly positive.
  float dot = 0.f;
  for (uint32_t i = 0; i < kDim; ++i) dot += src[i] * got.value()[i];
  const float cos = dot / (norm * got_norm + 1e-12f);
  EXPECT_GT(cos, 0.5f);
}

TEST(CompactVectorStore, EncodeFromRowsSkipsTombstones) {
  std::vector<Row> rows(3);
  rows[0].id = "a";
  rows[0].vector = {1.f, 0.f};
  rows[1].id = "b";
  rows[1].tombstone = true;
  rows[1].vector = {9.f, 9.f};
  rows[2].id = "c";
  rows[2].vector = {0.f, 1.f};

  auto store =
      CompactVectorStore::Encode(VectorEncoding::kFloat16, 2, rows);
  ASSERT_TRUE(store.ok());
  EXPECT_EQ(store.value().size(), 2u);

  auto a = store.value().Decode(0);
  auto c = store.value().Decode(1);
  ASSERT_TRUE(a.ok() && c.ok());
  EXPECT_NEAR(a.value()[0], 1.f, 0.01f);
  EXPECT_NEAR(c.value()[1], 1.f, 0.01f);
}

TEST(CompactVectorStore, FromPayloadRoundTrip) {
  constexpr uint32_t kDim = 16;
  auto src = MakeVector(kDim, 99);
  auto store = CompactVectorStore::Encode(VectorEncoding::kInt8Sq, kDim,
                                          src.data(), 1);
  ASSERT_TRUE(store.ok());

  auto again = CompactVectorStore::FromPayload(
      VectorEncoding::kInt8Sq, kDim, 1, store.value().payload());
  ASSERT_TRUE(again.ok());
  auto d0 = store.value().Decode(0);
  auto d1 = again.value().Decode(0);
  ASSERT_TRUE(d0.ok() && d1.ok());
  EXPECT_EQ(d0.value(), d1.value());
}

}  // namespace
}  // namespace aster
