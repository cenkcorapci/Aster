#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/transport/TSocket.h>

#include "Aster.h"
#include "aster/rpc/handler.h"
#include "aster/rpc/server.h"
#include "aster/server/catalog.h"

namespace aster {
namespace rpc {
namespace {

std::string TempDir(const char* name) {
  return std::string(::testing::TempDir()) + "/" + name;
}

std::string EncodeVector(const std::vector<float>& v) {
  std::string out(v.size() * sizeof(float), '\0');
  if (!v.empty()) {
    std::memcpy(out.data(), v.data(), out.size());
  }
  return out;
}

std::vector<float> MakeUnitVector(uint32_t dim, uint32_t seed) {
  std::vector<float> v(dim);
  double norm2 = 0.0;
  for (uint32_t i = 0; i < dim; ++i) {
    const uint32_t x = (seed * 1664525u + i * 1013904223u);
    v[i] = static_cast<float>((x % 2001) / 1000.0 - 1.0);
    norm2 += static_cast<double>(v[i]) * v[i];
  }
  const float inv =
      norm2 > 0.0 ? static_cast<float>(1.0 / std::sqrt(norm2)) : 1.0f;
  for (float& x : v) x *= inv;
  return v;
}

class ThriftClient {
 public:
  explicit ThriftClient(uint16_t port) {
    socket_ = std::make_shared<::apache::thrift::transport::TSocket>(
        "127.0.0.1", static_cast<int>(port));
    transport_ =
        std::make_shared<::apache::thrift::transport::TFramedTransport>(
            socket_);
    auto protocol =
        std::make_shared<::apache::thrift::protocol::TBinaryProtocol>(
            transport_);
    client_ = std::make_unique<AsterClient>(protocol);
    transport_->open();
  }

  ~ThriftClient() {
    try {
      if (transport_) transport_->close();
    } catch (...) {
    }
  }

  AsterClient* operator->() { return client_.get(); }

 private:
  std::shared_ptr<::apache::thrift::transport::TSocket> socket_;
  std::shared_ptr<::apache::thrift::transport::TTransport> transport_;
  std::unique_ptr<AsterClient> client_;
};

TEST(ThriftServer, UpsertGetSearchLocalhost) {
  const std::string dir = TempDir("aster_thrift_rpc");
  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;
  auto cat = Catalog::Open(opt);
  ASSERT_TRUE(cat.ok()) << cat.status().message();

  auto handler = std::make_shared<AsterHandler>(cat.value().get());
  ThriftServer::Options sop;
  sop.host = "127.0.0.1";
  sop.port = 0;
  ThriftServer server(sop, handler);
  ASSERT_TRUE(server.Listen().ok()) << "listen failed";
  ASSERT_GT(server.port(), 0);

  std::thread th([&] { server.Serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ThriftClient client(server.port());

  CollectionConfig cfg;
  cfg.__set_name("demo");
  VectorConfig vc;
  vc.__set_dimension(4);
  vc.__set_metric(DistanceMetric::COSINE);
  cfg.__set_vector(vc);
  client->createCollection(cfg);

  const auto vec = MakeUnitVector(4, 7);
  Document doc;
  doc.__set_id("d1");
  doc.__set_vector(EncodeVector(vec));
  doc.__set_tags({"even"});
  doc.__set_timestampMicros(1);
  client->upsert("demo", doc, ConsistencyLevel::ONE);

  Document got;
  client->get(got, "demo", "d1", ConsistencyLevel::ONE);
  EXPECT_EQ(got.id, "d1");
  ASSERT_EQ(got.vector.size(), vec.size() * sizeof(float));
  EXPECT_TRUE(got.__isset.tags);
  EXPECT_EQ(got.tags.count("even"), 1u);

  SearchRequest req;
  req.__set_collection("demo");
  req.__set_vector(EncodeVector(vec));
  req.__set_topK(3);
  SearchResponse resp;
  client->search(resp, req);
  ASSERT_FALSE(resp.hits.empty());
  EXPECT_EQ(resp.hits[0].id, "d1");
  EXPECT_GT(resp.hits[0].score, 0.9);

  server.Stop();
  th.join();
}

}  // namespace
}  // namespace rpc
}  // namespace aster
