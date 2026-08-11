#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "aster/index/tags.h"

namespace aster {
namespace {

Row MakeRow(const std::string& id, std::set<std::string> tags) {
  Row row;
  row.id = id;
  row.vector = {1.0f};
  row.tags = std::move(tags);
  return row;
}

TEST(TagBitmap, AddContainsAnd) {
  TagBitmap a;
  a.Add(1);
  a.Add(3);
  a.Add(1);
  EXPECT_EQ(a.Cardinality(), 2u);
  EXPECT_TRUE(a.Contains(1));
  EXPECT_FALSE(a.Contains(2));

  TagBitmap b;
  b.Add(3);
  b.Add(5);
  TagBitmap both = TagBitmap::And(a, b);
  EXPECT_EQ(both.Cardinality(), 1u);
  EXPECT_TRUE(both.Contains(3));
}

TEST(TagBitmap, DenseSerializeRoundTrip) {
  TagBitmap bm;
  bm.Add(0);
  bm.Add(7);
  bm.Add(8);
  const std::string blob = bm.SerializeDense(16);
  TagBitmap back = TagBitmap::DeserializeDense(blob, 16);
  EXPECT_EQ(back.ordinals(), bm.ordinals());
}

TEST(TagIndex, BuildMatchingSelectivity) {
  std::vector<Row> rows = {
      MakeRow("a", {"red", "sale"}),
      MakeRow("b", {"blue"}),
      MakeRow("c", {"red"}),
      MakeRow("d", {}),
  };
  TagIndex idx = TagIndex::Build(rows);
  EXPECT_EQ(idx.row_count(), 4u);
  EXPECT_EQ(idx.MatchCount({"red"}), 2u);
  EXPECT_EQ(idx.MatchCount({"red", "sale"}), 1u);
  EXPECT_EQ(idx.MatchCount({"green"}), 0u);
  EXPECT_DOUBLE_EQ(idx.Selectivity({"red"}), 0.5);
  EXPECT_DOUBLE_EQ(idx.Selectivity({}), 1.0);

  TagBitmap matching = idx.Matching({"red"});
  ASSERT_EQ(matching.Cardinality(), 2u);
  EXPECT_TRUE(matching.Contains(0));
  EXPECT_TRUE(matching.Contains(2));
}

TEST(TagIndex, PayloadRoundTrip) {
  std::vector<Row> rows = {
      MakeRow("a", {"even", "hot"}),
      MakeRow("b", {"odd"}),
      MakeRow("c", {"even"}),
  };
  TagIndex idx = TagIndex::Build(rows);
  const std::string payload = idx.SerializePayload();
  TagIndex back = TagIndex::ParsePayload(payload, 3);
  EXPECT_EQ(back.MatchCount({"even"}), 2u);
  EXPECT_EQ(back.MatchCount({"even", "hot"}), 1u);
  EXPECT_EQ(back.MatchCount({"odd"}), 1u);
}

TEST(AdaptiveFetchK, FloorCeilingAndSelectivity) {
  const uint32_t k = 10;
  const uint32_t ef = 128;
  EXPECT_EQ(BaseFetchK(k), 36u);
  // Mild filter: still at least 2k+16.
  EXPECT_EQ(AdaptiveFetchK(k, ef, /*sigma=*/0.5), 36u);
  // Selective filter raises fetch_k above the unfiltered floor.
  const uint32_t selective = AdaptiveFetchK(k, ef, /*sigma=*/0.05);
  EXPECT_GT(selective, BaseFetchK(k));
  EXPECT_LE(selective, ef);
  // Very selective hits the ef_search ceiling.
  EXPECT_EQ(AdaptiveFetchK(k, ef, /*sigma=*/0.001), ef);
  // sigma=1 behaves like the unfiltered floor.
  EXPECT_EQ(AdaptiveFetchK(k, ef, /*sigma=*/1.0), BaseFetchK(k));
}

TEST(AdaptiveFetchK, AdaptsMonotonicallyWithSigma) {
  const uint32_t k = 10;
  const uint32_t ef = 200;
  uint32_t prev = AdaptiveFetchK(k, ef, 1.0);
  for (double sigma : {0.5, 0.2, 0.1, 0.05, 0.02}) {
    const uint32_t cur = AdaptiveFetchK(k, ef, sigma);
    EXPECT_GE(cur, prev);
    prev = cur;
  }
}

}  // namespace
}  // namespace aster
