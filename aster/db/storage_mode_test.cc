#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "aster/db/db.h"
#include "aster/db/storage_mode.h"
#include "aster/platform/s3_fake.h"
#include "aster/platform/s3_storage.h"
#include "aster/server/catalog.h"

#if ASTER_ENABLE_HNSW
#include "aster/index/hnsw_pin.h"
#endif

namespace aster {
namespace {

Row MakeRow(const std::string& id, std::vector<float> vec, Timestamp ts) {
  Row row;
  row.id = id;
  row.vector = std::move(vec);
  row.timestamp = ts;
  return row;
}

TEST(StorageMode, PolicyHelpers) {
  EXPECT_FALSE(MirrorsToObjectStore(StorageMode::kHot));
  EXPECT_TRUE(MirrorsToObjectStore(StorageMode::kWarm));
  EXPECT_TRUE(MirrorsToObjectStore(StorageMode::kCold));

  EXPECT_FALSE(ClearsBlockCacheOnSearch(StorageMode::kHot));
  EXPECT_FALSE(ClearsBlockCacheOnSearch(StorageMode::kWarm));
  EXPECT_TRUE(ClearsBlockCacheOnSearch(StorageMode::kCold));

  EXPECT_FALSE(PinsHnswUpperLayers(StorageMode::kHot));
  EXPECT_TRUE(PinsHnswUpperLayers(StorageMode::kWarm));
  EXPECT_TRUE(PinsHnswUpperLayers(StorageMode::kCold));

  EXPECT_EQ(StorageModeFromString("HOT"), StorageMode::kHot);
  EXPECT_EQ(StorageModeFromString("warm"), StorageMode::kWarm);
  EXPECT_EQ(StorageModeFromString("COLD"), StorageMode::kCold);
  EXPECT_FALSE(StorageModeFromString("nope").has_value());
  EXPECT_STREQ(ToString(StorageMode::kWarm), "WARM");
}

TEST(StorageMode, HotRejectsNothingAndWarmNeedsObjectStore) {
  Db::Options hot;
  hot.dimension = 2;
  hot.metric = Metric::kL2;
  hot.data_dir = ::testing::TempDir() + "/aster_mode_hot";
  hot.wal_sync = SyncPolicy::kNever;
  hot.storage_mode = StorageMode::kHot;
  auto ok = Db::Open(hot);
  ASSERT_TRUE(ok.ok()) << ok.status().message();
  EXPECT_EQ(ok.value()->storage_mode(), StorageMode::kHot);

  Db::Options warm;
  warm.dimension = 2;
  warm.metric = Metric::kL2;
  warm.data_dir = ::testing::TempDir() + "/aster_mode_warm_bad";
  warm.wal_sync = SyncPolicy::kNever;
  warm.storage_mode = StorageMode::kWarm;
  auto bad = Db::Open(warm);
  EXPECT_FALSE(bad.ok());
  EXPECT_NE(bad.status().message().find("object_store"), std::string::npos);
}

#if ASTER_ENABLE_HNSW

TEST(StorageMode, WarmMirrorsAndColdClearsCacheKeepsPins) {
  FakeS3Server::Options fake_opt;
  fake_opt.bucket = "aster-storage-mode";
  FakeS3Server fake(fake_opt);
  ASSERT_TRUE(fake.Start().ok());

  S3Config cfg;
  cfg.endpoint = fake.endpoint();
  cfg.bucket = fake.bucket();
  cfg.path_style = true;
  cfg.block_cache_block_size = 64;
  cfg.block_cache_max_blocks = 8;
  cfg.multipart_threshold = 1024 * 1024;
  auto store = std::make_shared<S3Storage>(cfg);

  const std::string dir = ::testing::TempDir() + "/aster_mode_warm_cold";
  Db::Options opt;
  opt.dimension = 2;
  opt.metric = Metric::kL2;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  opt.background_index_build = false;
  opt.compaction_tier_threshold = 0;
  opt.max_segments_before_compact = 0;
  opt.storage_mode = StorageMode::kWarm;
  opt.object_store = store;
  opt.hnsw_params.m = 8;
  opt.hnsw_params.ef_construction = 32;
  opt.hnsw_params.ef_search_default = 16;
  opt.hnsw_params.max_layers = 8;
  opt.hnsw_rng_seed = 7;

  auto opened = Db::Open(opt);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  auto& db = *opened.value();

  for (int i = 0; i < 48; ++i) {
    ASSERT_TRUE(
        db.Upsert(MakeRow("r" + std::to_string(i),
                          {static_cast<float>(i % 8), static_cast<float>(i / 8)},
                          static_cast<Timestamp>(i + 1)))
            .ok());
  }
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.BuildPendingIndexes().ok());
  ASSERT_TRUE(db.segment_uses_hnsw()[0]);

  // WARM mirrors SSTable + HNSW into the object store.
  EXPECT_TRUE(store->Exists("seg_000001.ast"));
  EXPECT_TRUE(store->Exists("index/seg_000001.hnsw"));

  // Pin upper layers while in WARM (policy pins for WARM and COLD).
  auto bytes = store->Read("index/seg_000001.hnsw");
  ASSERT_TRUE(bytes.ok());
  auto pin = HnswUpperLayerPin::FromSerialized(bytes.value());
  ASSERT_TRUE(pin.ok()) << pin.status().message();
  ASSERT_FALSE(pin.value().PinRanges().empty());
  for (const auto& r : pin.value().PinRanges()) {
    EXPECT_TRUE(store->HasPinned("index/seg_000001.hnsw", r.start, r.end));
  }

  SearchRequest req;
  req.vector = {3.1f, 4.2f};
  req.top_k = 5;
  req.ef_search = 16;

  // WARM keeps local searchable index; search still works.
  auto warm1 = db.Search(req);
  ASSERT_FALSE(warm1.empty());
  EXPECT_EQ(db.storage_mode(), StorageMode::kWarm);

  // Switch to COLD online.
  ASSERT_TRUE(db.SetStorageMode(StorageMode::kCold).ok());
  EXPECT_EQ(db.storage_mode(), StorageMode::kCold);

  // Pins must survive ClearCache (COLD search clears the LRU).
  store->ClearCache();
  for (const auto& r : pin.value().PinRanges()) {
    EXPECT_TRUE(store->HasPinned("index/seg_000001.hnsw", r.start, r.end));
  }

  const uint64_t pin_hits_before = store->pin_hits();
  auto cold1 = db.Search(req);
  ASSERT_FALSE(cold1.empty());
  // COLD Search clears the block cache; pinned ranges remain readable.
  for (const auto& r : pin.value().PinRanges()) {
    EXPECT_TRUE(store->HasPinned("index/seg_000001.hnsw", r.start, r.end));
    const uint64_t gets_before = store->range_gets();
    auto again = store->ReadRange("index/seg_000001.hnsw", r.start, r.end);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value(),
              bytes.value().substr(r.start, r.end - r.start));
    EXPECT_EQ(store->range_gets(), gets_before);
  }
  EXPECT_GT(store->pin_hits(), pin_hits_before);

  // Mirrored objects remain after COLD switch.
  EXPECT_TRUE(store->Exists("seg_000001.ast"));
  EXPECT_TRUE(store->Exists("index/seg_000001.hnsw"));

  // Back to HOT: no longer requires object store I/O on search.
  ASSERT_TRUE(db.SetStorageMode(StorageMode::kHot).ok());
  EXPECT_EQ(db.storage_mode(), StorageMode::kHot);
  auto hot_hits = db.Search(req);
  ASSERT_FALSE(hot_hits.empty());
  EXPECT_EQ(hot_hits[0].id, cold1[0].id);

  fake.Stop();
}

TEST(StorageMode, CatalogPersistsModeAndOpensWithObjectStore) {
  FakeS3Server::Options fake_opt;
  fake_opt.bucket = "aster-catalog-mode";
  FakeS3Server fake(fake_opt);
  ASSERT_TRUE(fake.Start().ok());

  S3Config cfg;
  cfg.endpoint = fake.endpoint();
  cfg.bucket = fake.bucket();
  cfg.path_style = true;
  auto store = std::make_shared<S3Storage>(cfg);

  const std::string root = ::testing::TempDir() + "/aster_catalog_mode";
  Catalog::Options cat_opt;
  cat_opt.data_dir = root;
  cat_opt.wal_sync = SyncPolicy::kNever;
  cat_opt.object_store = store;

  {
    auto cat = Catalog::Open(cat_opt);
    ASSERT_TRUE(cat.ok()) << cat.status().message();
    CollectionInfo info;
    info.name = "c1";
    info.dimension = 2;
    info.metric = Metric::kL2;
    info.storage_mode = StorageMode::kCold;
    ASSERT_TRUE(cat.value()->CreateCollection(info).ok());
    ASSERT_TRUE(cat.value()
                    ->Upsert("c1", MakeRow("a", {1.0f, 0.0f}, 1))
                    .ok());
    ASSERT_TRUE(cat.value()->Flush("c1").ok());
  }

  auto reopened = Catalog::Open(cat_opt);
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  auto got = reopened.value()->GetCollection("c1");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->storage_mode, StorageMode::kCold);
  EXPECT_TRUE(store->Exists("seg_000001.ast"));

  fake.Stop();
}

#endif  // ASTER_ENABLE_HNSW

}  // namespace
}  // namespace aster
