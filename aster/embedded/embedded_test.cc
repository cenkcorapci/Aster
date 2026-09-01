#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "aster/embedded/db.h"

namespace aster {
namespace embedded {
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

TEST(EmbeddedDb, UpsertSearchFlushCompact) {
  Db::Options opt;
  opt.dimension = 2;
  opt.metric = Metric::kL2;
  opt.memtable_flush_rows = 2;
  Db db(opt);

  ASSERT_TRUE(db.Upsert(MakeRow("a", {0.0f, 0.0f}, 1, {"even"})).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("b", {1.0f, 0.0f}, 2, {"odd"})).ok());
  EXPECT_EQ(db.segment_count(), 1u);  // auto-flush at 2 rows

  ASSERT_TRUE(db.Upsert(MakeRow("c", {0.0f, 1.0f}, 3, {"even"})).ok());
  ASSERT_TRUE(db.Flush().ok());
  EXPECT_GE(db.segment_count(), 2u);

  SearchRequest req;
  req.vector = {0.0f, 0.0f};
  req.top_k = 2;
  req.tags = {"even"};
  auto hits = db.Search(req);
  ASSERT_FALSE(hits.empty());
  EXPECT_EQ(hits[0].id, "a");

  ASSERT_TRUE(db.Compact().ok());
  EXPECT_EQ(db.segment_count(), 1u);
  EXPECT_TRUE(db.Get("b").has_value());
}

TEST(EmbeddedDb, DimensionGuard) {
  Db::Options opt;
  opt.dimension = 2;
  Db db(opt);
  EXPECT_FALSE(db.Upsert(MakeRow("x", {1.0f}, 1)).ok());
}

TEST(EmbeddedDb, GetDeleteAndLww) {
  Db::Options opt;
  opt.dimension = 2;
  opt.metric = Metric::kL2;
  Db db(opt);

  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  auto got = db.Get("a");
  ASSERT_TRUE(got.has_value());
  EXPECT_FLOAT_EQ(got->vector[0], 1.0f);

  ASSERT_TRUE(db.Upsert(MakeRow("a", {0.0f, 1.0f}, 2)).ok());
  got = db.Get("a");
  ASSERT_TRUE(got.has_value());
  EXPECT_FLOAT_EQ(got->vector[1], 1.0f);

  ASSERT_TRUE(db.Delete("a", 3).ok());
  EXPECT_FALSE(db.Get("a").has_value());
}

TEST(EmbeddedDb, FlushReopenViaMemtableOnly) {
  Db::Options opt;
  opt.dimension = 2;
  opt.metric = Metric::kL2;
  opt.memtable_flush_rows = 100;
  Db db(opt);
  ASSERT_TRUE(db.Upsert(MakeRow("p", {0.5f, 0.5f}, 1)).ok());
  ASSERT_TRUE(db.Flush().ok());
  EXPECT_EQ(db.segment_count(), 1u);

  SearchRequest req;
  req.vector = {0.5f, 0.5f};
  req.top_k = 1;
  auto hits = db.Search(req);
  ASSERT_FALSE(hits.empty());
  EXPECT_EQ(hits[0].id, "p");
}

TEST(EmbeddedDb, CosineTagFilterAndCompactNoOp) {
  Db::Options opt;
  opt.dimension = 2;
  opt.metric = Metric::kCosine;
  opt.max_segments_before_compact = 0;
  Db db(opt);

  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1, {"keep"})).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("b", {0.0f, 1.0f}, 2, {"drop"})).ok());

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 2;
  req.tags = {"keep"};
  auto hits = db.Search(req);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].id, "a");

  EXPECT_TRUE(db.Compact().ok());
  EXPECT_EQ(db.segment_count(), 0u);
}

TEST(EmbeddedDb, RejectsZeroDimension) {
  Db::Options opt;
  opt.dimension = 0;
  Db db(opt);
  EXPECT_FALSE(db.Upsert(MakeRow("x", {1.0f, 0.0f}, 1)).ok());
}

}  // namespace
}  // namespace embedded
}  // namespace aster
