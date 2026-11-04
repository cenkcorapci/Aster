#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <set>
#include <string>

#include "Aster.h"
#include "aster/rpc/handler.h"
#include "aster/server/catalog.h"

namespace aster {
namespace rpc {
namespace {

std::string TempDir(const char* name) {
  return std::string(::testing::TempDir()) + "/" + name;
}

class HandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Catalog::Options opt;
    opt.data_dir = TempDir("handler_fixture");
    opt.wal_sync = SyncPolicy::kNever;
    auto opened = Catalog::Open(opt);
    ASSERT_TRUE(opened.ok());
    catalog_ = std::move(opened.value());
    handler_ = std::make_unique<AsterHandler>(catalog_.get());
  }

  std::unique_ptr<Catalog> catalog_;
  std::unique_ptr<AsterHandler> handler_;
};

TEST_F(HandlerTest, ConfigureRejectsInvalidFields) {
  CollectionConfig cfg;
  cfg.__set_name("bad");
  VectorConfig vc;
  vc.__set_dimension(0);
  vc.__set_metric(DistanceMetric::COSINE);
  cfg.__set_vector(vc);
  try {
    handler_->configureCollection(cfg);
    FAIL() << "expected dimension error";
  } catch (const AsterError& e) {
    EXPECT_EQ(e.code, static_cast<int32_t>(StatusCode::kInvalidArgument));
  }

  vc.__set_dimension(4);
  vc.__set_metric(static_cast<DistanceMetric::type>(99));
  cfg.__set_vector(vc);
  try {
    handler_->configureCollection(cfg);
    FAIL() << "expected metric error";
  } catch (const AsterError&) {
  }

  handler_->createCollection("bad");
  vc.__set_metric(DistanceMetric::L2);
  cfg.__set_vector(vc);
  cfg.__set_storageMode(static_cast<StorageMode::type>(99));
  try {
    handler_->configureCollection(cfg);
    FAIL() << "expected storage mode error";
  } catch (const AsterError&) {
  }

  cfg.__set_storageMode(StorageMode::WARM);
  ResourceLimits lim;
  lim.__set_maxVectors(-1);
  cfg.__set_resourceLimits(lim);
  try {
    handler_->configureCollection(cfg);
    FAIL() << "expected maxVectors error";
  } catch (const AsterError&) {
  }

  lim.__set_maxVectors(0);
  lim.__set_memoryBudgetBytes(-1);
  cfg.__set_resourceLimits(lim);
  try {
    handler_->configureCollection(cfg);
    FAIL() << "expected memoryBudgetBytes error";
  } catch (const AsterError&) {
  }

  lim.__set_memoryBudgetBytes(0);
  lim.__set_maxQps(-1);
  cfg.__set_resourceLimits(lim);
  try {
    handler_->configureCollection(cfg);
    FAIL() << "expected maxQps error";
  } catch (const AsterError&) {
  }

  lim.__set_maxQps(0);
  lim.__set_storageQuotaBytes(-1);
  cfg.__set_resourceLimits(lim);
  try {
    handler_->configureCollection(cfg);
    FAIL() << "expected storageQuotaBytes error";
  } catch (const AsterError&) {
  }

  lim.__set_storageQuotaBytes(0);
  lim.__set_isolation(static_cast<IsolationLevel::type>(99));
  cfg.__set_resourceLimits(lim);
  try {
    handler_->configureCollection(cfg);
    FAIL() << "expected isolation error";
  } catch (const AsterError&) {
  }
}

TEST_F(HandlerTest, UpsertGetSearchRoundTrip) {
  CollectionConfig cfg;
  cfg.__set_name("demo");
  VectorConfig vc;
  vc.__set_dimension(3);
  vc.__set_metric(DistanceMetric::DOT);
  cfg.__set_vector(vc);
  handler_->createCollection(cfg.name);
  handler_->configureCollection(cfg);

  Document doc;
  doc.__set_id("x");
  std::string bytes(3 * sizeof(float), '\0');
  float vec[3] = {1.0f, 2.0f, 3.0f};
  std::memcpy(bytes.data(), vec, bytes.size());
  doc.__set_vector(bytes);
  doc.__set_metadata("meta");
  std::set<std::string> doc_tags;
  doc_tags.insert("t1");
  doc.__set_tags(doc_tags);
  handler_->upsert("demo", doc, ConsistencyLevel::ONE);

  Document got;
  handler_->get(got, "demo", "x", ConsistencyLevel::ONE);
  EXPECT_EQ(got.id, "x");
  EXPECT_TRUE(got.__isset.metadata);
  EXPECT_TRUE(got.__isset.tags);

  std::set<std::string> tags;
  tags.insert("t1");
  SearchRequest req;
  req.__set_collection("demo");
  req.__set_vector(bytes);
  req.__set_topK(2);
  req.__set_efSearch(32);
  req.__set_tags(tags);
  SearchResponse resp;
  handler_->search(resp, req);
  ASSERT_FALSE(resp.hits.empty());
  EXPECT_EQ(resp.hits[0].id, "x");

  Document bad;
  bad.__set_id("y");
  bad.__set_vector("abc");
  try {
    handler_->upsert("demo", bad, ConsistencyLevel::ONE);
    FAIL() << "expected bad vector length";
  } catch (const AsterError&) {
  }
}

TEST_F(HandlerTest, DropMissingCollectionFails) {
  try {
    handler_->dropCollection("missing");
    FAIL() << "expected drop error";
  } catch (const AsterError& e) {
    EXPECT_EQ(e.code, static_cast<int32_t>(StatusCode::kNotFound));
  }
}

}  // namespace
}  // namespace rpc
}  // namespace aster
