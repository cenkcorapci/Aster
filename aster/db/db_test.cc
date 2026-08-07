#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "aster/db/db.h"

namespace aster {
namespace {

Row MakeRow(const std::string& id, std::vector<float> vec, Timestamp ts,
            std::set<std::string> tags = {}) {
  Row row;
  row.id = id;
  row.vector = std::move(vec);
  row.timestamp = ts;
  row.tags = std::move(tags);
  return row;
}

Db::Options SmallDb() {
  Db::Options options;
  options.dimension = 2;
  options.metric = Metric::kL2;
  return options;
}

TEST(Db, SearchSpansMemtableAndSegments) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("seg-row", {1.0f, 0.0f}, 10)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Upsert(MakeRow("mem-row", {0.9f, 0.0f}, 20)).ok());

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 2;
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 2u);
  EXPECT_EQ(hits[0].id, "seg-row");
  EXPECT_EQ(hits[1].id, "mem-row");
}

TEST(Db, DeleteHidesRowEvenIfIndexedInSegment) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 10)).ok());
  ASSERT_TRUE(db.Flush().ok());  // "a" is now baked into a segment index
  ASSERT_TRUE(db.Delete("a", 20).ok());

  EXPECT_FALSE(db.Get("a").has_value());
  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  auto hits = db.Search(req);
  EXPECT_TRUE(hits.empty());
}

TEST(Db, UpdateInMemtableShadowsSegmentVersion) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 10)).ok());
  ASSERT_TRUE(db.Flush().ok());
  // Move "a" far away from the query point.
  ASSERT_TRUE(db.Upsert(MakeRow("a", {-1.0f, 0.0f}, 20)).ok());

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 1;
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 1u);
  // Score must reflect the latest vector, not the stale segment copy.
  EXPECT_FLOAT_EQ(hits[0].score, -4.0f);  // -L2^2 of (1,0) vs (-1,0)
}

TEST(Db, TagFiltering) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("x", {1.0f, 0.0f}, 10, {"red", "sale"})).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("y", {1.0f, 0.1f}, 10, {"blue"})).ok());

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.tags = {"red"};
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].id, "x");
}

TEST(Db, CompactionPurgesTombstonesAndKeepsLatest) {
  Db db(SmallDb());
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 10)).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("b", {0.0f, 1.0f}, 10)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Delete("b", 20).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("a", {0.5f, 0.5f}, 30)).ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_EQ(db.segment_count(), 2u);

  ASSERT_TRUE(db.Compact().ok());
  ASSERT_EQ(db.segment_count(), 1u);
  EXPECT_FALSE(db.Get("b").has_value());
  ASSERT_TRUE(db.Get("a").has_value());
  EXPECT_FLOAT_EQ(db.Get("a")->vector[0], 0.5f);

  SearchRequest req;
  req.vector = {0.5f, 0.5f};
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].id, "a");
}

TEST(Db, DimensionValidation) {
  Db db(SmallDb());
  EXPECT_FALSE(db.Upsert(MakeRow("bad", {1.0f}, 10)).ok());
}

TEST(Db, DurableOpenRecoversFlushedAndWalRows) {
  const std::string dir = ::testing::TempDir() + "/aster_db_durable";
  {
    Db::Options options = SmallDb();
    options.data_dir = dir;
    options.wal_sync = SyncPolicy::kNever;
    auto db = Db::Open(options);
    ASSERT_TRUE(db.ok()) << db.status().message();
    ASSERT_TRUE(db.value()
                    ->Upsert(MakeRow("flushed", {1.0f, 0.0f}, 10))
                    .ok());
    ASSERT_TRUE(db.value()->Flush().ok());
    ASSERT_TRUE(
        db.value()->Upsert(MakeRow("wal-only", {0.0f, 1.0f}, 20)).ok());
    // Destroy without flush: wal-only must come back via replay.
  }

  Db::Options options = SmallDb();
  options.data_dir = dir;
  options.wal_sync = SyncPolicy::kNever;
  auto db = Db::Open(options);
  ASSERT_TRUE(db.ok()) << db.status().message();
  EXPECT_EQ(db.value()->segment_count(), 1u);
  ASSERT_TRUE(db.value()->Get("flushed").has_value());
  ASSERT_TRUE(db.value()->Get("wal-only").has_value());
  EXPECT_FLOAT_EQ(db.value()->Get("wal-only")->vector[1], 1.0f);
}

}  // namespace
}  // namespace aster
