#include <gtest/gtest.h>

#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "aster/server/catalog.h"

// Smoke: a few tenants × mixed dim/row indexes via Catalog.
// Full matrix soak is //aster/bench:multi-tenant-bench + make bench-multitenant.

namespace aster {
namespace {

TEST(MultiTenantBenchSmoke, ThreeTenantsMixedIndexes) {
  const std::string dir =
      std::string(::testing::TempDir()) + "/aster_mt_smoke";
  ::mkdir(dir.c_str(), 0755);

  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  opt.memtable_flush_bytes = 2 << 20;
  auto opened = Catalog::Open(opt);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  auto& cat = *opened.value();

  struct Spec {
    uint32_t dim;
    int rows;
  };
  const Spec specs[] = {{64, 40}, {256, 30}, {2048, 16}};

  for (int t = 0; t < 3; ++t) {
    for (const auto& s : specs) {
      CollectionInfo info;
      info.name = "t" + std::to_string(t) + "_d" + std::to_string(s.dim) +
                  "_n" + std::to_string(s.rows);
      info.dimension = s.dim;
      info.metric = Metric::kCosine;
      ASSERT_TRUE(cat.CreateCollection(info).ok()) << info.name;

      std::vector<float> target(s.dim);
      const float inv = 1.f / std::sqrt(static_cast<float>(s.dim));
      for (float& x : target) x = inv;
      {
        Row row;
        row.id = "target";
        row.vector = target;
        row.timestamp = 1;
        ASSERT_TRUE(cat.Upsert(info.name, std::move(row)).ok());
      }
      for (int i = 0; i < s.rows; ++i) {
        Row row;
        row.id = "doc-" + std::to_string(i);
        row.vector.assign(s.dim, 0.f);
        // Orthogonal-ish one-hot axes — not equal to the uniform target.
        row.vector[static_cast<size_t>(i % static_cast<int>(s.dim))] = 1.f;
        row.timestamp = static_cast<Timestamp>(i + 2);
        ASSERT_TRUE(cat.Upsert(info.name, std::move(row)).ok());
      }
      ASSERT_TRUE(cat.Flush(info.name).ok());

      SearchRequest req;
      req.vector = target;
      req.top_k = 5;
      auto hits = cat.Search(info.name, req);
      ASSERT_TRUE(hits.ok()) << hits.status().message();
      ASSERT_FALSE(hits.value().empty());
      EXPECT_EQ(hits.value()[0].id, "target");
    }
  }

  EXPECT_EQ(cat.ListCollections().size(), 9u);
  const auto usage = cat.Usage();
  EXPECT_EQ(usage.collections, 9u);
  EXPECT_GE(usage.vectors_estimate, 9u * 16u);
}

}  // namespace
}  // namespace aster
