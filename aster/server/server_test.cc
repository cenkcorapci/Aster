#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sstream>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
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

TEST(Catalog, DropCollectionDeletesDataDirectory) {
  const std::string dir = TempDir("aster_catalog_drop");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok());

  CollectionInfo info;
  info.name = "ephemeral";
  info.dimension = 2;
  info.metric = Metric::kL2;
  ASSERT_TRUE(cat.value()->CreateCollection(info).ok());

  Row row;
  row.id = "a";
  row.vector = {1.0f, 0.0f};
  row.timestamp = 1;
  ASSERT_TRUE(cat.value()->Upsert("ephemeral", row).ok());
  ASSERT_TRUE(cat.value()->Flush("ephemeral").ok());

  const std::string col_dir = dir + "/ephemeral";
  EXPECT_EQ(access(col_dir.c_str(), F_OK), 0);
  EXPECT_EQ(access((col_dir + "/seg_000001.ast").c_str(), F_OK), 0);

  ASSERT_TRUE(cat.value()->DropCollection("ephemeral").ok());
  EXPECT_NE(access(col_dir.c_str(), F_OK), 0);
  EXPECT_FALSE(cat.value()->GetCollection("ephemeral").has_value());
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

TEST(Json, GetNumberAndMissingKeys) {
  EXPECT_FALSE(json::GetString("{}", "missing").has_value());
  EXPECT_FALSE(json::GetInt("{}", "n").has_value());
  auto num = json::GetNumber(R"({"x":2.5})", "x");
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(*num, 2.5);
}

TEST(Json, EscapeAndStringArray) {
  EXPECT_EQ(json::Escape("\t"), "\\t");
  auto arr = json::GetStringArray(R"({"tags":["a","b\\c"]})", "tags");
  ASSERT_TRUE(arr.has_value());
  ASSERT_EQ(arr->size(), 2u);
  EXPECT_EQ(arr->at(1), "b\\c");
  EXPECT_FALSE(json::GetFloatArray(R"({"v":not_a_number})", "v").has_value());
  auto empty = json::GetFloatArray(R"({"v":[]})", "v");
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
}

TEST(Catalog, TwoPhaseCreateConfigureLifecycle) {
  const std::string dir = TempDir("aster_catalog_2phase");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok());

  ASSERT_TRUE(cat.value()->CreateCollectionCreated("pending").ok());
  Row row;
  row.id = "a";
  row.vector = {1.0f, 0.0f};
  row.timestamp = 1;
  EXPECT_FALSE(cat.value()->Upsert("pending", row).ok());

  CollectionInfo info;
  info.name = "pending";
  info.dimension = 2;
  info.metric = Metric::kL2;
  ASSERT_TRUE(cat.value()->ConfigureCollection(info).ok());
  ASSERT_TRUE(cat.value()->Upsert("pending", std::move(row)).ok());

  auto got = cat.value()->Get("pending", "a");
  ASSERT_TRUE(got.ok());
  ASSERT_TRUE(got.value().has_value());
}

TEST(HttpApi, FullRouteCoverage) {
  const std::string dir = TempDir("aster_http_full");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok());
  ApiHandler handler(cat.value().get());

  auto http = [&](const std::string& method, const std::string& path,
                  const std::string& body = "", const std::string& query = "") {
    HttpRequest req;
    req.method = method;
    req.path = path;
    req.query = query;
    req.body = body;
    return handler.Handle(req);
  };

  auto created = http("PUT", "/v1/collections/full",
                      R"({"dimension":2,"metric":"l2"})");
  EXPECT_EQ(created.status, 201);

  auto exists = http("PUT", "/v1/collections/full",
                     R"({"dimension":2,"metric":"l2"})");
  EXPECT_EQ(exists.status, 200);
  EXPECT_NE(exists.body.find("exists"), std::string::npos);

  auto listed = http("GET", "/v1/collections");
  EXPECT_EQ(listed.status, 200);
  EXPECT_NE(listed.body.find("full"), std::string::npos);

  auto got_col = http("GET", "/v1/collections/full");
  EXPECT_EQ(got_col.status, 200);
  EXPECT_NE(got_col.body.find("\"dimension\":2"), std::string::npos);

  auto bad_metric = http("PUT", "/v1/collections/bad",
                         R"({"dimension":2,"metric":"invalid"})");
  EXPECT_EQ(bad_metric.status, 400);

  auto upserted = http("PUT", "/v1/collections/full/docs/d1",
                       R"({"vector":[1.0,0.0],"tags":["t"],"metadata":"cb","timestamp":1})");
  EXPECT_EQ(upserted.status, 200);

  auto got_doc = http("GET", "/v1/collections/full/docs/d1");
  EXPECT_EQ(got_doc.status, 200);
  EXPECT_NE(got_doc.body.find("\"d1\""), std::string::npos);
  EXPECT_NE(got_doc.body.find("\"t\""), std::string::npos);

  auto searched = http("POST", "/v1/collections/full/search",
                       R"({"vector":[1.0,0.0],"top_k":2,"ef_search":64,"tags":["t"]})");
  EXPECT_EQ(searched.status, 200);
  EXPECT_NE(searched.body.find("d1"), std::string::npos);

  auto flushed = http("POST", "/v1/collections/full/flush");
  EXPECT_EQ(flushed.status, 200);

  auto compacted = http("POST", "/v1/collections/full/compact");
  EXPECT_EQ(compacted.status, 200);

  auto deleted = http("DELETE", "/v1/collections/full/docs/d1", "",
                      "timestamp=99");
  EXPECT_EQ(deleted.status, 200);

  auto missing_doc = http("GET", "/v1/collections/full/docs/d1");
  EXPECT_EQ(missing_doc.status, 404);

  auto metrics = http("GET", "/metrics");
  EXPECT_EQ(metrics.status, 200);
  EXPECT_NE(metrics.content_type.find("text/plain"), std::string::npos);

  auto not_found = http("GET", "/v1/collections/missing");
  EXPECT_EQ(not_found.status, 404);

  auto bad_route = http("POST", "/v1/collections/full/unknown");
  EXPECT_EQ(bad_route.status, 404);

  auto method_bad = http("POST", "/v1/collections");
  EXPECT_EQ(method_bad.status, 405);

  auto bad_ef = http("POST", "/v1/collections/full/search",
                     R"({"vector":[1.0,0.0],"ef_search":-1})");
  EXPECT_EQ(bad_ef.status, 400);

  auto bad_limits = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/lim", "",
      R"({"dimension":2,"metric":"l2","max_vectors":-1})", ""});
  EXPECT_EQ(bad_limits.status, 400);

  auto dropped = http("DELETE", "/v1/collections/full");
  EXPECT_EQ(dropped.status, 200);
  EXPECT_FALSE(cat.value()->GetCollection("full").has_value());
}

TEST(HttpApi, ResourceLimitsReturn429) {
  const std::string dir = TempDir("aster_http_limits");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok());
  ApiHandler handler(cat.value().get());

  auto created = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/limited", "",
      R"({"dimension":2,"metric":"l2","max_vectors":1,"max_qps":1})", ""});
  ASSERT_EQ(created.status, 201);

  auto first = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/limited/docs/d1", "",
      R"({"vector":[1.0,0.0],"timestamp":1})", ""});
  EXPECT_EQ(first.status, 200);

  auto second = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/limited/docs/d2", "",
      R"({"vector":[0.0,1.0],"timestamp":2})", ""});
  EXPECT_EQ(second.status, 429);

  auto search = handler.Handle(HttpRequest{
      "POST", "/v1/collections/limited/search", "",
      R"({"vector":[1.0,0.0],"top_k":1})", ""});
  EXPECT_EQ(search.status, 200);

  auto search2 = handler.Handle(HttpRequest{
      "POST", "/v1/collections/limited/search", "",
      R"({"vector":[1.0,0.0],"top_k":1})", ""});
  EXPECT_EQ(search2.status, 429);
}

TEST(HttpApi, StorageModeAndIsolationOnCreate) {
  const std::string dir = TempDir("aster_http_storage");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok());
  ApiHandler handler(cat.value().get());

  auto bad_mode = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/warm", "",
      R"({"dimension":2,"metric":"cosine","storage_mode":"icy"})", ""});
  EXPECT_EQ(bad_mode.status, 400);

  auto warm_without_s3 = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/warm", "",
      R"({"dimension":2,"metric":"cosine","storage_mode":"warm"})", ""});
  EXPECT_EQ(warm_without_s3.status, 400);

  auto created = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/hot", "",
      R"({"dimension":2,"metric":"cosine","isolation":"DEDICATED"})", ""});
  EXPECT_EQ(created.status, 201);

  auto info = cat.value()->GetCollection("hot");
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->storage_mode, StorageMode::kHot);
  EXPECT_EQ(info->resource_limits.isolation, IsolationLevel::kDedicated);
}

std::string RawHttpRequest(uint16_t port, const std::string& request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return {};
  }
  ::send(fd, request.data(), request.size(), 0);
  std::string resp;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    resp.append(buf, static_cast<size_t>(n));
    if (resp.find("\r\n\r\n") != std::string::npos) break;
  }
  ::close(fd);
  return resp;
}

TEST(HttpServer, RejectsMalformedHttpRequests) {
  const std::string dir = TempDir("aster_http_malformed");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok());
  ApiHandler handler(cat.value().get());
  HttpServer::Options hop;
  hop.host = "127.0.0.1";
  hop.port = 0;
  HttpServer server(hop, handler);
  ASSERT_TRUE(server.Listen().ok());
  const uint16_t port = server.port();
  std::thread th([&] { server.Serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const std::string bad_len =
      "POST /v1/collections/x/search HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n";
  EXPECT_TRUE(RawHttpRequest(port, bad_len).empty());

  const std::string bad_digits =
      "POST /v1/collections/x/search HTTP/1.1\r\nContent-Length: 12abc\r\n\r\n";
  EXPECT_TRUE(RawHttpRequest(port, bad_digits).empty());

  server.Stop();
  th.join();
}

TEST(HttpServer, RawSocketHandleClient) {
  const std::string dir = TempDir("aster_http_socket");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok());
  ApiHandler handler(cat.value().get());
  HttpServer::Options hop;
  hop.host = "127.0.0.1";
  hop.port = 0;
  HttpServer server(hop, handler);
  ASSERT_TRUE(server.Listen().ok());
  const uint16_t port = server.port();

  std::thread th([&] { server.Serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const std::string req =
      "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  const std::string resp = RawHttpRequest(port, req);
  EXPECT_NE(resp.find("200"), std::string::npos);
  EXPECT_NE(resp.find("ok"), std::string::npos);

  const std::string auth_req =
      "GET /v1/usage HTTP/1.1\r\nAuthorization: Bearer secret\r\n"
      "Connection: close\r\n\r\n";
  ApiHandler keyed(cat.value().get(), "secret");
  HttpServer keyed_server(hop, keyed);
  ASSERT_TRUE(keyed_server.Listen().ok());
  const uint16_t port2 = keyed_server.port();
  std::thread th2([&] { keyed_server.Serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const std::string auth_resp = RawHttpRequest(port2, auth_req);
  EXPECT_NE(auth_resp.find("200"), std::string::npos);

  auto created = handler.Handle(HttpRequest{
      "PUT", "/v1/collections/demo", "",
      R"({"dimension":2,"metric":"l2"})", ""});
  ASSERT_EQ(created.status, 201);

  const std::string body = R"({"vector":[1.0,0.0],"timestamp":1})";
  std::ostringstream post;
  post << "PUT /v1/collections/demo/docs/socket1 HTTP/1.1\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << body;
  const std::string post_resp = RawHttpRequest(port, post.str());
  EXPECT_NE(post_resp.find("200"), std::string::npos);

  server.Stop();
  keyed_server.Stop();
  th.join();
  th2.join();
}

TEST(HttpServer, ListenRejectsBadHost) {
  ApiHandler handler(nullptr);
  HttpServer::Options hop;
  hop.host = "not-an-ip";
  hop.port = 0;
  HttpServer server(hop, handler);
  EXPECT_FALSE(server.Listen().ok());
}

}  // namespace
}  // namespace aster
