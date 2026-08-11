#include "Aster.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

// Smoke test: generated types + AsterIf service interface compile and link.
TEST(RpcCodegen, TypesAndServiceInterface) {
  aster::rpc::CollectionConfig cfg;
  cfg.name = "demo";
  cfg.vector.dimension = 4;
  cfg.vector.metric = aster::rpc::DistanceMetric::L2;
  aster::rpc::ResourceLimits lim;
  lim.__set_maxVectors(100);
  lim.__set_isolation(aster::rpc::IsolationLevel::SHARED);
  cfg.__set_resourceLimits(lim);

  aster::rpc::Document doc;
  doc.id = "a";
  doc.vector = std::string(16, '\0');

  aster::rpc::SearchRequest req;
  req.collection = cfg.name;
  req.vector = doc.vector;
  req.__set_topK(3);

  aster::rpc::AsterNull handler;
  handler.createCollection(cfg.name);
  handler.configureCollection(cfg);
  handler.upsert(cfg.name, doc, aster::rpc::ConsistencyLevel::ONE);

  aster::rpc::Document got;
  handler.get(got, cfg.name, doc.id, aster::rpc::ConsistencyLevel::ONE);

  aster::rpc::SearchResponse resp;
  handler.search(resp, req);

  EXPECT_TRUE(resp.hits.empty());
}

}  // namespace
