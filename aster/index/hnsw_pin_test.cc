#include "aster/index/hnsw_pin.h"

#include <chrono>
#include <string>
#include <vector>

#include "aster/index/hnsw_build.h"
#include "aster/index/hnsw_search.h"
#include "aster/platform/s3_fake.h"
#include "aster/platform/s3_storage.h"
#include "gtest/gtest.h"

#if ASTER_ENABLE_HNSW

namespace aster {
namespace {

HnswGraph MakeMultiLayerFixture() {
  HnswParams params;
  params.m = 4;
  params.ef_construction = 32;
  params.ef_search_default = 16;
  params.max_layers = 8;

  HnswGraph g(params);
  g.set_segment_id(7);
  EXPECT_TRUE(g.AddNode(10, 2).ok());
  EXPECT_TRUE(g.AddNode(11, 0).ok());
  EXPECT_TRUE(g.AddNode(12, 1).ok());
  EXPECT_TRUE(g.AddNode(13, 0).ok());
  EXPECT_TRUE(g.SetNeighbors(0, 0, {1, 2}).ok());
  EXPECT_TRUE(g.SetNeighbors(1, 0, {0, 2}).ok());
  EXPECT_TRUE(g.SetNeighbors(2, 0, {0, 1, 3}).ok());
  EXPECT_TRUE(g.SetNeighbors(3, 0, {2}).ok());
  EXPECT_TRUE(g.SetNeighbors(0, 1, {2}).ok());
  EXPECT_TRUE(g.SetNeighbors(2, 1, {0}).ok());
  EXPECT_TRUE(g.SetNeighbors(0, 2, {}).ok());
  return g;
}

std::vector<std::vector<float>> FixtureVectors() {
  return {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
}

TEST(HnswUpperLayerPin, FromSerializedKeepsUpperOmitsLayer0) {
  const HnswGraph g = MakeMultiLayerFixture();
  const std::string bytes = g.Serialize();

  auto pin_r = HnswUpperLayerPin::FromSerialized(bytes);
  ASSERT_TRUE(pin_r.ok()) << pin_r.status().message();
  const HnswUpperLayerPin& pin = pin_r.value();

  EXPECT_TRUE(pin.upper_layers_local());
  EXPECT_EQ(pin.node_count(), g.node_count());
  EXPECT_EQ(pin.entry_point(), g.entry_point());
  EXPECT_EQ(pin.max_level(), g.max_level());
  EXPECT_EQ(pin.segment_id(), 7u);
  EXPECT_EQ(pin.Neighbors(0, 1), g.Neighbors(0, 1));
  EXPECT_EQ(pin.Neighbors(2, 1), g.Neighbors(2, 1));
  EXPECT_EQ(pin.Neighbors(0, 2), g.Neighbors(0, 2));
  // Layer 0 is not pinned.
  EXPECT_TRUE(pin.Neighbors(0, 0).empty());
  EXPECT_TRUE(pin.Neighbors(2, 0).empty());

  const auto ranges = pin.PinRanges();
  ASSERT_GE(ranges.size(), 1u);
  EXPECT_EQ(ranges[0].start, 0u);
  EXPECT_EQ(ranges[0].end, pin.layout().node_table_end);
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[1].start, pin.layout().upper_begin);
  EXPECT_EQ(ranges[1].end, pin.layout().upper_end);
  EXPECT_GT(pin.layout().layer0_end, pin.layout().layer0_begin);
  EXPECT_LT(pin.pinned_bytes(), bytes.size());
}

TEST(HnswUpperLayerPin, Layer0OffsetsRoundTrip) {
  const HnswGraph g = MakeMultiLayerFixture();
  const std::string bytes = g.Serialize();
  auto pin_r = HnswUpperLayerPin::FromSerialized(bytes);
  ASSERT_TRUE(pin_r.ok());
  const auto& layout = pin_r.value().layout();

  for (uint32_t n = 0; n < g.node_count(); ++n) {
    auto nbs = HnswReadLayer0Neighbors(bytes, layout, n);
    ASSERT_TRUE(nbs.ok()) << nbs.status().message();
    EXPECT_EQ(nbs.value(), g.Neighbors(n, 0));
  }
}

TEST(HnswUpperLayerPin, SearchPinnedMatchesFullGraph) {
  const HnswGraph g = MakeMultiLayerFixture();
  const std::string bytes = g.Serialize();
  auto pin_r = HnswUpperLayerPin::FromSerialized(bytes);
  ASSERT_TRUE(pin_r.ok());
  const auto vectors = FixtureVectors();
  const std::vector<float> query = {0.1f, 0.2f};

  auto full = HnswSearch(Metric::kL2, g, vectors, query, /*top_k=*/2,
                         /*ef_search=*/8);
  auto pinned = HnswSearchPinned(
      Metric::kL2, pin_r.value(),
      [&](uint32_t node) -> Result<std::vector<uint32_t>> {
        return HnswReadLayer0Neighbors(bytes, pin_r.value().layout(), node);
      },
      vectors, query, /*top_k=*/2, /*ef_search=*/8);
  ASSERT_EQ(full.size(), pinned.size());
  for (size_t i = 0; i < full.size(); ++i) {
    EXPECT_EQ(full[i].first, pinned[i].first);
    EXPECT_FLOAT_EQ(full[i].second, pinned[i].second);
  }
}

TEST(HnswUpperLayerPin, S3ColdSearchWithinBoundPinsSurviveCacheClear) {
  FakeS3Server::Options opt;
  opt.bucket = "aster-hnsw-pin";
  FakeS3Server fake(opt);
  ASSERT_TRUE(fake.Start().ok());

  // Larger graph so layer-0 dominates the .hnsw payload.
  HnswParams params;
  params.m = 8;
  params.ef_construction = 32;
  params.ef_search_default = 16;
  params.max_layers = 8;
  std::vector<std::vector<float>> vectors;
  for (int i = 0; i < 64; ++i) {
    vectors.push_back({static_cast<float>(i % 8), static_cast<float>(i / 8)});
  }
  HnswBuilder builder(Metric::kL2, params, /*rng_seed=*/99);
  auto built = builder.Build(vectors);
  ASSERT_TRUE(built.ok()) << built.status().message();
  ASSERT_GE(built.value().max_level(), 1u);

  const std::string bytes = built.value().Serialize();
  const std::string key = "index/seg_000001.hnsw";

  S3Config cfg;
  cfg.endpoint = fake.endpoint();
  cfg.bucket = fake.bucket();
  cfg.path_style = true;
  cfg.block_cache_block_size = 64;
  cfg.block_cache_max_blocks = 4;  // small LRU — without pin, upper bytes drop
  cfg.multipart_threshold = 1024 * 1024;
  S3Storage store(cfg);
  ASSERT_TRUE(store.Put(key, bytes).ok());

  auto pin_r = HnswUpperLayerPin::FromSerialized(bytes);
  ASSERT_TRUE(pin_r.ok()) << pin_r.status().message();
  const HnswUpperLayerPin& pin = pin_r.value();
  for (const HnswPinRange& r : pin.PinRanges()) {
    ASSERT_TRUE(store
                    .PinRange(key, r.start, r.end,
                              bytes.substr(r.start, r.end - r.start))
                    .ok());
    EXPECT_TRUE(store.HasPinned(key, r.start, r.end));
  }
  EXPECT_GT(store.pinned_bytes(), 0u);
  EXPECT_LT(pin.pinned_bytes(), bytes.size());

  // Simulate cold worker: drop LRU blocks; pins must remain.
  store.ClearCache();
  for (const HnswPinRange& r : pin.PinRanges()) {
    EXPECT_TRUE(store.HasPinned(key, r.start, r.end));
  }

  const uint64_t range_gets_before = store.range_gets();
  const uint64_t pin_hits_before = store.pin_hits();
  uint64_t layer0_fetches = 0;

  const std::vector<float> query = {3.1f, 4.2f};
  const auto t0 = std::chrono::steady_clock::now();
  auto hits = HnswSearchPinned(
      Metric::kL2, pin,
      [&](uint32_t node) -> Result<std::vector<uint32_t>> {
        ++layer0_fetches;
        const size_t off = pin.layout().layer0_offsets[node];
        const size_t max_list =
            2u + static_cast<size_t>(HnswLayer0MaxDegree(params)) * 4u;
        auto chunk = store.ReadRange(key, off, off + max_list);
        if (!chunk.ok()) return chunk.status();
        const std::string& raw = chunk.value();
        if (raw.size() < 2) {
          return Status::Corruption("hnsw pin test: short layer0 chunk");
        }
        const uint16_t degree = static_cast<uint16_t>(
            static_cast<uint8_t>(raw[0]) |
            (static_cast<uint16_t>(static_cast<uint8_t>(raw[1])) << 8));
        if (raw.size() < 2u + static_cast<size_t>(degree) * 4u) {
          return Status::Corruption("hnsw pin test: truncated layer0 list");
        }
        std::vector<uint32_t> nbs(degree);
        for (uint16_t i = 0; i < degree; ++i) {
          size_t p = 2u + static_cast<size_t>(i) * 4u;
          uint32_t v = 0;
          for (int b = 0; b < 4; ++b) {
            v |= static_cast<uint32_t>(static_cast<uint8_t>(raw[p + b]))
                 << (8 * b);
          }
          nbs[i] = v;
        }
        return nbs;
      },
      vectors, query, /*top_k=*/5, /*ef_search=*/16);
  const auto t1 = std::chrono::steady_clock::now();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  ASSERT_FALSE(hits.empty());
  EXPECT_LE(ms, kHnswS3ColdSearchBoundMs)
      << "cold search " << ms << " ms exceeds bound "
      << kHnswS3ColdSearchBoundMs << " ms";

  // Layer-0 neighbors came from S3 Range GETs; upper layers stayed in the pin.
  EXPECT_GT(layer0_fetches, 0u);
  EXPECT_GT(store.range_gets(), range_gets_before);

  // Pinned ranges remain after cold search; LRU may have filled for L0.
  for (const HnswPinRange& r : pin.PinRanges()) {
    EXPECT_TRUE(store.HasPinned(key, r.start, r.end));
    // Re-read via pin path (exact range) → pin hit, no new Range GET required.
    const uint64_t gets_before_pin_read = store.range_gets();
    auto again = store.ReadRange(key, r.start, r.end);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value(), bytes.substr(r.start, r.end - r.start));
    EXPECT_EQ(store.range_gets(), gets_before_pin_read);
  }
  EXPECT_GT(store.pin_hits(), pin_hits_before);

  // Contrast: naive full Read after ClearCache re-fetches the entire object.
  store.ClearCache();
  const uint64_t gets_before_full = store.range_gets();
  auto full = store.Read(key);
  ASSERT_TRUE(full.ok());
  EXPECT_EQ(full.value(), bytes);
  EXPECT_GT(store.range_gets(), gets_before_full);
  const uint64_t full_object_gets = store.range_gets() - gets_before_full;
  // Full object spans more blocks than a single layer-0 neighbor list.
  EXPECT_GT(full_object_gets, 1u);

  fake.Stop();
}

}  // namespace
}  // namespace aster

#endif  // ASTER_ENABLE_HNSW
