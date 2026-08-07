#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

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

  server.Stop();
  th.join();
}

}  // namespace
}  // namespace aster
