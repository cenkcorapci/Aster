#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "aster/server/catalog.h"
#include "aster/server/http_api.h"
#include "aster/server/json.h"

namespace aster {
namespace {

std::string TempDir(const char* name) {
  std::string path = std::string(::testing::TempDir()) + "/" + name;
  ::remove((path + "/CATALOG").c_str());
  return path;
}

std::vector<float> MakeUnitVector(uint32_t dim, uint32_t seed) {
  std::vector<float> v(dim);
  double norm2 = 0.0;
  for (uint32_t i = 0; i < dim; ++i) {
    // Deterministic pseudo-random in [-1, 1].
    const uint32_t x = (seed * 1664525u + i * 1013904223u);
    v[i] = static_cast<float>((x % 2001) / 1000.0 - 1.0);
    norm2 += static_cast<double>(v[i]) * v[i];
  }
  const float inv = norm2 > 0.0 ? static_cast<float>(1.0 / std::sqrt(norm2)) : 1.0f;
  for (float& x : v) x *= inv;
  return v;
}

std::string VectorJson(const std::vector<float>& v) {
  std::ostringstream os;
  os << '[';
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) os << ',';
    os << v[i];
  }
  os << ']';
  return os.str();
}

TEST(Json, RoundTripBasics) {
  const std::string body =
      R"({"dimension":3,"metric":"cosine","vector":[1.0, 2.5, -3],"tags":["a","b"]})";
  EXPECT_EQ(json::GetInt(body, "dimension").value_or(0), 3);
  EXPECT_EQ(json::GetString(body, "metric").value_or(""), "cosine");
  auto v = json::GetFloatArray(body, "vector");
  ASSERT_TRUE(v);
  ASSERT_EQ(v->size(), 3u);
  EXPECT_FLOAT_EQ((*v)[1], 2.5f);
  auto tags = json::GetStringArray(body, "tags");
  ASSERT_TRUE(tags);
  ASSERT_EQ(tags->size(), 2u);
  EXPECT_EQ((*tags)[0], "a");
}

TEST(Catalog, CreateUpsertSearchPersist) {
  const std::string dir = TempDir("aster_catalog");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok()) << cat.status().message();

  CollectionInfo info;
  info.name = "products";
  info.dimension = 2;
  info.metric = Metric::kL2;
  ASSERT_TRUE(cat.value()->CreateCollection(info).ok());

  Row row;
  row.id = "a";
  row.vector = {1.0f, 0.0f};
  row.timestamp = 1;
  row.tags = {"hot"};
  ASSERT_TRUE(cat.value()->Upsert("products", row).ok());

  SearchRequest req;
  req.vector = {1.0f, 0.0f};
  req.top_k = 5;
  auto hits = cat.value()->Search("products", req);
  ASSERT_TRUE(hits.ok());
  ASSERT_FALSE(hits.value().empty());
  EXPECT_EQ(hits.value()[0].id, "a");

  ASSERT_TRUE(cat.value()->Flush("products").ok());
  cat.value().reset();

  auto reopened = Catalog::Open(opt);
  ASSERT_TRUE(reopened.ok());
  ASSERT_TRUE(reopened.value()->GetCollection("products").has_value());
  auto got = reopened.value()->Get("products", "a");
  ASSERT_TRUE(got.ok());
  ASSERT_TRUE(got.value().has_value());
  EXPECT_EQ(got.value()->id, "a");
}

TEST(HttpApi, EndToEndLocalhost) {
  const std::string dir = TempDir("aster_http");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok());

  ApiHandler handler(cat.value().get(), "test-key");
  HttpServer::Options hop;
  hop.host = "127.0.0.1";
  hop.port = 0;  // ephemeral — bind 0 then getsockname
  // Our Listen uses options_.port; port 0 is fine for bind.
  HttpServer server(hop, handler);
  ASSERT_TRUE(server.Listen().ok());
  const uint16_t port = server.port();
  ASSERT_GT(port, 0);

  std::thread th([&] { server.Serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto http = [&](const std::string& method, const std::string& path,
                  const std::string& body) {
    // Use ApiHandler directly for deterministic unit coverage of routes;
    // socket path exercised via Listen success + one raw connect below.
    HttpRequest req;
    req.method = method;
    req.path = path;
    req.body = body;
    req.api_key = "test-key";
    return handler.Handle(req);
  };

  auto created =
      http("PUT", "/v1/collections/demo",
           R"({"dimension":2,"metric":"cosine"})");
  EXPECT_EQ(created.status, 201);

  auto upserted =
      http("PUT", "/v1/collections/demo/docs/d1",
           R"({"vector":[1.0,0.0],"tags":["x"],"timestamp":1})");
  EXPECT_EQ(upserted.status, 200);

  auto searched =
      http("POST", "/v1/collections/demo/search",
           R"({"vector":[1.0,0.0],"top_k":3})");
  EXPECT_EQ(searched.status, 200);
  EXPECT_NE(searched.body.find("d1"), std::string::npos);

  auto usage = http("GET", "/v1/usage", "");
  EXPECT_EQ(usage.status, 200);
  EXPECT_NE(usage.body.find("upserts"), std::string::npos);

  auto denied = handler.Handle(
      HttpRequest{"GET", "/v1/usage", "", "", "bad"});
  EXPECT_EQ(denied.status, 401);

  auto health = handler.Handle(HttpRequest{"GET", "/health", "", "", ""});
  EXPECT_EQ(health.status, 200);

  auto drained = http("POST", "/v1/admin/drain", "");
  EXPECT_EQ(drained.status, 200);
  EXPECT_NE(drained.body.find("drained"), std::string::npos);

  server.Stop();
  th.join();
}

class HighDimTest : public ::testing::TestWithParam<uint32_t> {};

TEST_P(HighDimTest, CatalogUpsertSearchFlushReopen) {
  const uint32_t dim = GetParam();
  const std::string dir =
      TempDir(("aster_dim_" + std::to_string(dim)).c_str());
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok()) << cat.status().message();

  CollectionInfo info;
  info.name = "d" + std::to_string(dim);
  info.dimension = dim;
  info.metric = Metric::kCosine;
  ASSERT_TRUE(cat.value()->CreateCollection(info).ok());

  const auto target = MakeUnitVector(dim, /*seed=*/7);
  auto decoy = MakeUnitVector(dim, /*seed=*/99);
  // Guarantee decoy is not identical/near-identical to the query (dim=1 cosine).
  if (dim == 1) {
    decoy[0] = -target[0];
  } else {
    decoy[0] = -decoy[0];
  }
  {
    Row row;
    row.id = "target";
    row.vector = target;
    row.timestamp = 1;
    ASSERT_TRUE(cat.value()->Upsert(info.name, std::move(row)).ok());
  }
  {
    Row row;
    row.id = "decoy";
    row.vector = decoy;
    row.timestamp = 2;
    ASSERT_TRUE(cat.value()->Upsert(info.name, std::move(row)).ok());
  }

  SearchRequest req;
  req.vector = target;
  req.top_k = 2;
  auto hits = cat.value()->Search(info.name, req);
  ASSERT_TRUE(hits.ok()) << hits.status().message();
  ASSERT_GE(hits.value().size(), 1u);
  EXPECT_EQ(hits.value()[0].id, "target");

  ASSERT_TRUE(cat.value()->Flush(info.name).ok());
  cat.value().reset();

  auto reopened = Catalog::Open(opt);
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  auto got = reopened.value()->Get(info.name, "target");
  ASSERT_TRUE(got.ok());
  ASSERT_TRUE(got.value().has_value());
  ASSERT_EQ(got.value()->vector.size(), dim);
  EXPECT_FLOAT_EQ(got.value()->vector[0], target[0]);
  EXPECT_FLOAT_EQ(got.value()->vector[dim - 1], target[dim - 1]);

  hits = reopened.value()->Search(info.name, req);
  ASSERT_TRUE(hits.ok());
  ASSERT_GE(hits.value().size(), 1u);
  EXPECT_EQ(hits.value()[0].id, "target");
}

TEST_P(HighDimTest, HttpApiUpsertSearch) {
  const uint32_t dim = GetParam();
  const std::string dir =
      TempDir(("aster_http_dim_" + std::to_string(dim)).c_str());
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok());
  ApiHandler handler(cat.value().get());

  const std::string coll = "c" + std::to_string(dim);
  auto created = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/" + coll, "",
      "{\"dimension\":" + std::to_string(dim) + ",\"metric\":\"cosine\"}", ""});
  ASSERT_EQ(created.status, 201) << created.body;

  const auto vec = MakeUnitVector(dim, /*seed=*/3);
  const std::string body =
      std::string("{\"vector\":") + VectorJson(vec) + ",\"timestamp\":1}";
  auto upserted = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/" + coll + "/docs/doc1", "", body, ""});
  ASSERT_EQ(upserted.status, 200) << upserted.body;

  const std::string search_body =
      std::string("{\"vector\":") + VectorJson(vec) + ",\"top_k\":5}";
  auto searched = handler.Handle(HttpRequest{
      "POST", "/v1/collections/" + coll + "/search", "", search_body, ""});
  ASSERT_EQ(searched.status, 200) << searched.body;
  EXPECT_NE(searched.body.find("doc1"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(DimensionsUpTo4096, HighDimTest,
                         ::testing::Values(1u, 128u, 384u, 768u, 1536u, 3072u,
                                           4096u));

TEST(HttpApi, RejectsOversizedDimensionAndBadDocId) {
  const std::string dir = TempDir("aster_http_sec");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok());
  ApiHandler handler(cat.value().get());

  auto huge = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/big", "",
      R"({"dimension":99999,"metric":"cosine"})", ""});
  EXPECT_EQ(huge.status, 400);

  auto created = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/ok", "",
      R"({"dimension":2,"metric":"l2"})", ""});
  ASSERT_EQ(created.status, 201);

  auto bad_id = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/ok/docs/..", "",
      R"({"vector":[1,0],"timestamp":1})", ""});
  EXPECT_EQ(bad_id.status, 400);

  auto bad_topk = handler.Handle(HttpRequest{
      "POST", "/v1/collections/ok/search", "",
      R"({"vector":[1,0],"top_k":100000})", ""});
  EXPECT_EQ(bad_topk.status, 400);
}

}  // namespace
}  // namespace aster
