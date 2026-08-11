#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>
#include <thread>
#include <vector>

#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TSSLSocket.h>

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

class AllowAllAccessManager
    : public ::apache::thrift::transport::AccessManager {
 public:
  Decision verify(const sockaddr_storage& /*sa*/) noexcept override {
    return Decision::ALLOW;
  }

  Decision verify(const std::string& /*host*/,
                   const char* /*name*/,
                   int /*size*/) noexcept override {
    return Decision::ALLOW;
  }

  Decision verify(const sockaddr_storage& /*sa*/,
                   const char* /*data*/,
                   int /*size*/) noexcept override {
    return Decision::ALLOW;
  }
};

std::pair<std::string, std::string> WriteSelfSignedCertKey(
    const std::string& dir) {
  const std::string mkdir_cmd = "mkdir -p '" + dir + "'";
  const int rc = std::system(mkdir_cmd.c_str());
  if (rc != 0) {
    throw std::runtime_error("failed to mkdir TLS dir: " + dir);
  }
  const std::string cert = dir + "/cert.pem";
  const std::string key = dir + "/key.pem";

  // Generate a short-lived self-signed cert for tests.
  // We keep verification disabled on the client side
  // (`ThriftClient(..., /*tls_insecure=*/true)`) so this doesn't need to be
  // trusted by any external CA.
  const std::string cmd = "openssl req -x509 -nodes -newkey rsa:2048 "
                           "-keyout '" +
                           key + "' -out '" + cert +
                           "' -days 1 -subj /CN=localhost >/dev/null 2>&1";
  const int openssl_rc = std::system(cmd.c_str());
  if (openssl_rc != 0) {
    throw std::runtime_error("failed to generate TLS cert/key via openssl");
  }
  return {cert, key};
}

class ThriftClient {
 public:
  explicit ThriftClient(uint16_t port, bool use_tls = false,
                         bool tls_insecure = true,
                         std::string trusted_cert_file = {})
      : use_tls_(use_tls) {
    if (!use_tls_) {
      socket_ =
          std::make_shared<::apache::thrift::transport::TSocket>(
              "127.0.0.1", static_cast<int>(port));
    } else {
      tls_factory_ =
          std::make_shared<::apache::thrift::transport::TSSLSocketFactory>(
              ::apache::thrift::transport::SSLProtocol::LATEST);
      tls_factory_->access(std::make_shared<AllowAllAccessManager>());
      if (!trusted_cert_file.empty()) {
        // Trust the server certificate so `TSSLSocket::authorize()` passes.
        tls_factory_->loadTrustedCertificates(trusted_cert_file.c_str(),
                                               nullptr);
        tls_factory_->authenticate(true);
      } else {
        // "insecure" test mode: don't verify server cert.
        tls_factory_->authenticate(!tls_insecure);
      }

      auto ssl_socket = tls_factory_->createSocket(
          "127.0.0.1", static_cast<int>(port));
      ssl_socket->server(false);
      socket_ = ssl_socket;
    }

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
  bool use_tls_;
  // IMPORTANT: `TSSLSocketFactory` must outlive any `TSSLSocket` instances it
  // creates; member destruction order is reverse of declaration order.
  std::shared_ptr<::apache::thrift::transport::TSSLSocketFactory> tls_factory_;
  std::shared_ptr<::apache::thrift::transport::TSocket> socket_;
  std::shared_ptr<::apache::thrift::transport::TTransport> transport_;
  std::unique_ptr<AsterClient> client_;
};

TEST(ThriftServer, UpsertGetSearchPlaintextWhenTLSEnabled) {
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

  const std::string tls_dir = TempDir("aster_thrift_rpc_tls");
  const auto [cert, key] = WriteSelfSignedCertKey(tls_dir);
  sop.tls = true;
  sop.tls_insecure = true;
  sop.tls_cert_file = cert;
  sop.tls_key_file = key;

  ThriftServer server(sop, handler);
  ASSERT_TRUE(server.Listen().ok()) << "listen failed";
  ASSERT_GT(server.port(), 0);

  std::thread th([&] { server.Serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Plaintext client should still work (TLS mode accepts plaintext too).
  // Destroy client before stopping the server to avoid TLS shutdown races.
  {
    ThriftClient plain_client(server.port(), /*use_tls=*/false);

  CollectionConfig cfg;
  cfg.__set_name("demo");
  VectorConfig vc;
  vc.__set_dimension(4);
  vc.__set_metric(DistanceMetric::COSINE);
  cfg.__set_vector(vc);
  client->createCollection(cfg.name);
  client->configureCollection(cfg);
  client->createCollection("pending");

  const auto vec = MakeUnitVector(4, 7);
  Document doc;
  doc.__set_id("d1");
  doc.__set_vector(EncodeVector(vec));
  doc.__set_tags({"even"});
  doc.__set_timestampMicros(1);

  // Lifecycle gating: upsert is rejected until configureCollection() ran.
  Document pending_doc = doc;
  pending_doc.__set_id("p1");
  try {
    client->upsert("pending", pending_doc, ConsistencyLevel::ONE);
    FAIL() << "expected upsert to throw for unconfigured collection";
  } catch (const AsterError&) {
  }

  client->upsert("demo", doc, ConsistencyLevel::ONE);

    Document got;
    plain_client->get(got, "demo_plain", "d1", ConsistencyLevel::ONE);
    EXPECT_EQ(got.id, "d1");
    ASSERT_EQ(got.vector.size(), vec.size() * sizeof(float));
    EXPECT_TRUE(got.__isset.tags);
    EXPECT_EQ(got.tags.count("even"), 1u);

    SearchRequest req;
    req.__set_collection("demo_plain");
    req.__set_vector(EncodeVector(vec));
    req.__set_topK(3);
    SearchResponse resp;
    plain_client->search(resp, req);
    ASSERT_FALSE(resp.hits.empty());
    EXPECT_EQ(resp.hits[0].id, "d1");
    EXPECT_GT(resp.hits[0].score, 0.9);
  }

  server.Stop();
  th.join();
}

TEST(ThriftServer, UpsertGetSearchTLSClient) {
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

  const std::string tls_dir = TempDir("aster_thrift_rpc_tls");
  const auto [cert, key] = WriteSelfSignedCertKey(tls_dir);
  sop.tls = true;
  sop.tls_insecure = true;
  sop.tls_cert_file = cert;
  sop.tls_key_file = key;

  ThriftServer server(sop, handler);
  ASSERT_TRUE(server.Listen().ok()) << "listen failed";
  ASSERT_GT(server.port(), 0);

  std::thread th([&] { server.Serve(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  {
    // TLS client should work over the same port too.
    // Destroy client before stopping the server to avoid TLS shutdown races.
    try {
      ThriftClient tls_client(server.port(), /*use_tls=*/true,
                               /*tls_insecure=*/true, cert);

      CollectionConfig cfg2;
      cfg2.__set_name("demo_tls");
      VectorConfig vc2;
      vc2.__set_dimension(4);
      vc2.__set_metric(DistanceMetric::COSINE);
      cfg2.__set_vector(vc2);
      tls_client->createCollection(cfg2);

      const auto vec = MakeUnitVector(4, 7);
      Document doc2;
      doc2.__set_id("d2");
      doc2.__set_vector(EncodeVector(vec));
      doc2.__set_tags({"even"});
      doc2.__set_timestampMicros(2);
      tls_client->upsert("demo_tls", doc2, ConsistencyLevel::ONE);

      Document got2;
      tls_client->get(got2, "demo_tls", "d2", ConsistencyLevel::ONE);
      EXPECT_EQ(got2.id, "d2");
    } catch (const std::exception& e) {
      FAIL() << "TLS client threw exception: " << e.what();
    } catch (...) {
      FAIL() << "TLS client threw unknown exception";
    }
  }

  server.Stop();
  th.join();
}

TEST(ThriftServer, CreateConfigurePersistsAfterRestart) {
  const std::string dir = TempDir("aster_thrift_rpc_restart");

  Catalog::Options opt;
  opt.data_dir = dir;
  opt.wal_sync = SyncPolicy::kNever;

  // First boot: create+configure+upsert.
  {
    auto cat = Catalog::Open(opt);
    ASSERT_TRUE(cat.ok()) << cat.status().message();
    auto handler = std::make_shared<AsterHandler>(cat.value().get());
    ThriftServer::Options sop;
    sop.host = "127.0.0.1";
    sop.port = 0;
    ThriftServer server(sop, handler);
    ASSERT_TRUE(server.Listen().ok());
    const uint16_t port = server.port();

    std::thread th([&] { server.Serve(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ThriftClient client(port);
    CollectionConfig cfg;
    cfg.__set_name("demo");
    VectorConfig vc;
    vc.__set_dimension(4);
    vc.__set_metric(DistanceMetric::COSINE);
    cfg.__set_vector(vc);
    client->createCollection(cfg.name);
    client->configureCollection(cfg);

    const auto vec = MakeUnitVector(4, 7);
    Document doc;
    doc.__set_id("d1");
    doc.__set_vector(EncodeVector(vec));
    doc.__set_timestampMicros(1);
    client->upsert("demo", doc, ConsistencyLevel::ONE);

    Document got;
    client->get(got, "demo", "d1", ConsistencyLevel::ONE);
    EXPECT_EQ(got.id, "d1");

    SearchRequest req;
    req.__set_collection("demo");
    req.__set_vector(EncodeVector(vec));
    req.__set_topK(3);
    SearchResponse resp;
    client->search(resp, req);
    ASSERT_FALSE(resp.hits.empty());
    EXPECT_EQ(resp.hits[0].id, "d1");

    server.Stop();
    th.join();
  }

  // Second boot: data must still be visible via RPC.
  {
    auto cat = Catalog::Open(opt);
    ASSERT_TRUE(cat.ok()) << cat.status().message();
    auto handler = std::make_shared<AsterHandler>(cat.value().get());
    ThriftServer::Options sop;
    sop.host = "127.0.0.1";
    sop.port = 0;
    ThriftServer server(sop, handler);
    ASSERT_TRUE(server.Listen().ok());
    const uint16_t port = server.port();

    std::thread th([&] { server.Serve(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ThriftClient client(port);
    Document got;
    client->get(got, "demo", "d1", ConsistencyLevel::ONE);
    EXPECT_EQ(got.id, "d1");

    SearchRequest req;
    req.__set_collection("demo");
    const auto vec = MakeUnitVector(4, 7);
    req.__set_vector(EncodeVector(vec));
    req.__set_topK(3);
    SearchResponse resp;
    client->search(resp, req);
    ASSERT_FALSE(resp.hits.empty());
    EXPECT_EQ(resp.hits[0].id, "d1");

    server.Stop();
    th.join();
  }
}

}  // namespace
}  // namespace rpc
}  // namespace aster
