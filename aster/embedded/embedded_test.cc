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

}  // namespace
}  // namespace embedded
}  // namespace aster
