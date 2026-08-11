#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "aster/db/db.h"
#include "aster/db/resource_limits.h"
#include "aster/server/catalog.h"

namespace aster {
namespace {

Row MakeRow(const std::string& id, std::vector<float> vec, Timestamp ts) {
  Row row;
  row.id = id;
  row.vector = std::move(vec);
  row.timestamp = ts;
  return row;
}

TEST(ResourceLimits, PolicyHelpers) {
  EXPECT_EQ(IsolationLevelFromString("SHARED"), IsolationLevel::kShared);
  EXPECT_EQ(IsolationLevelFromString("dedicated"), IsolationLevel::kDedicated);
  EXPECT_FALSE(IsolationLevelFromString("nope").has_value());
  EXPECT_STREQ(ToString(IsolationLevel::kDedicated), "DEDICATED");
  EXPECT_TRUE(UsesHardLocalEnforcement(IsolationLevel::kShared));
  EXPECT_TRUE(UsesHardLocalEnforcement(IsolationLevel::kDedicated));

  ResourceLimits unlimited;
  EXPECT_TRUE(unlimited.unlimited());
  ResourceLimits capped;
  capped.max_vectors = 10;
  EXPECT_FALSE(capped.unlimited());
}

TEST(ResourceLimits, MaxVectorsExceededReturnsStatus) {
  Db::Options opt;
  opt.dimension = 2;
  opt.metric = Metric::kL2;
  opt.wal_sync = SyncPolicy::kNever;
  opt.resource_limits.max_vectors = 2;
  opt.resource_limits.isolation = IsolationLevel::kShared;

  Db db(opt);
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  ASSERT_TRUE(db.Upsert(MakeRow("b", {0.0f, 1.0f}, 2)).ok());

  // Update of an existing live id is allowed.
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 1.0f}, 3)).ok());

  auto st = db.Upsert(MakeRow("c", {0.5f, 0.5f}, 4));
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), StatusCode::kResourceExhausted);
  EXPECT_NE(st.message().find("max vectors"), std::string::npos);
}

TEST(ResourceLimits, StorageQuotaExceededReturnsStatus) {
  Db::Options opt;
  opt.dimension = 2;
  opt.metric = Metric::kL2;
  opt.wal_sync = SyncPolicy::kNever;
  // One vector = 8 bytes; allow exactly one live vector.
  opt.resource_limits.storage_quota_bytes = 8;
  opt.resource_limits.isolation = IsolationLevel::kDedicated;

  Db db(opt);
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  auto st = db.Upsert(MakeRow("b", {0.0f, 1.0f}, 2));
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.code(), StatusCode::kResourceExhausted);
  EXPECT_NE(st.message().find("storage quota"), std::string::npos);
}

TEST(ResourceLimits, MaxQpsExceededReturnsStatus) {
  Db::Options opt;
  opt.dimension = 2;
  opt.metric = Metric::kL2;
  opt.wal_sync = SyncPolicy::kNever;
  opt.resource_limits.max_qps = 2;

  Db db(opt);
  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 1;

  auto ok1 = db.TrySearch(req);
  ASSERT_TRUE(ok1.ok()) << ok1.status().message();
  auto ok2 = db.TrySearch(req);
  ASSERT_TRUE(ok2.ok()) << ok2.status().message();

  auto denied = db.TrySearch(req);
  EXPECT_FALSE(denied.ok());
  EXPECT_EQ(denied.status().code(), StatusCode::kResourceExhausted);
  EXPECT_NE(denied.status().message().find("QPS"), std::string::npos);

  // Legacy Search swallows the Status into an empty hit list.
  EXPECT_TRUE(db.Search(req).empty());
}

TEST(ResourceLimits, SetResourceLimitsOnlineAndMemoryBudget) {
  Db::Options opt;
  opt.dimension = 2;
  opt.metric = Metric::kL2;
  opt.wal_sync = SyncPolicy::kNever;

  Db db(opt);
  ResourceLimits lim;
  lim.max_vectors = 1;
  lim.memory_budget_bytes = 1 << 20;
  lim.isolation = IsolationLevel::kDedicated;
  ASSERT_TRUE(db.SetResourceLimits(lim).ok());
  EXPECT_EQ(db.resource_limits().max_vectors, 1u);
  EXPECT_EQ(db.resource_limits().isolation, IsolationLevel::kDedicated);

  ASSERT_TRUE(db.Upsert(MakeRow("a", {1.0f, 0.0f}, 1)).ok());
  auto st = db.Upsert(MakeRow("b", {0.0f, 1.0f}, 2));
  EXPECT_EQ(st.code(), StatusCode::kResourceExhausted);
}

TEST(ResourceLimits, CatalogPersistsLimitsAndEnforces) {
  const std::string root =
      ::testing::TempDir() + "/aster_catalog_resource_limits";
  Catalog::Options cat_opt;
  cat_opt.data_dir = root;
  cat_opt.wal_sync = SyncPolicy::kNever;

  {
    auto cat = Catalog::Open(cat_opt);
    ASSERT_TRUE(cat.ok()) << cat.status().message();
    CollectionInfo info;
    info.name = "c1";
    info.dimension = 2;
    info.metric = Metric::kL2;
    info.resource_limits.max_vectors = 1;
    info.resource_limits.max_qps = 1;
    info.resource_limits.isolation = IsolationLevel::kDedicated;
    ASSERT_TRUE(cat.value()->CreateCollection(info).ok());
    ASSERT_TRUE(
        cat.value()->Upsert("c1", MakeRow("a", {1.0f, 0.0f}, 1)).ok());
    auto st =
        cat.value()->Upsert("c1", MakeRow("b", {0.0f, 1.0f}, 2));
    EXPECT_EQ(st.code(), StatusCode::kResourceExhausted);

    SearchRequest req;
    req.vector = {1.0f, 0.0f};
    req.top_k = 1;
    auto ok = cat.value()->Search("c1", req);
    ASSERT_TRUE(ok.ok()) << ok.status().message();
    auto denied = cat.value()->Search("c1", req);
    EXPECT_FALSE(denied.ok());
    EXPECT_EQ(denied.status().code(), StatusCode::kResourceExhausted);
  }

  auto reopened = Catalog::Open(cat_opt);
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  auto got = reopened.value()->GetCollection("c1");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->resource_limits.max_vectors, 1u);
  EXPECT_EQ(got->resource_limits.max_qps, 1u);
  EXPECT_EQ(got->resource_limits.isolation, IsolationLevel::kDedicated);

  auto st =
      reopened.value()->Upsert("c1", MakeRow("b", {0.0f, 1.0f}, 3));
  EXPECT_EQ(st.code(), StatusCode::kResourceExhausted);
}

}  // namespace
}  // namespace aster
