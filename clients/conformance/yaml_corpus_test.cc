// YAML conformance corpus runner (M5-T03).
//
// Starts the M4-T02 Thrift server as a fixture and drives each corpus file
// through the generated AsterClient (wire-level C++ reference until the
// idiomatic //clients/cpp transport lands in M5-T04).

#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/transport/TSocket.h>

#include <yaml-cpp/yaml.h>

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

std::vector<float> ParseVector(const YAML::Node& node) {
  std::vector<float> v;
  for (const auto& x : node) {
    v.push_back(x.as<float>());
  }
  return v;
}

DistanceMetric::type ParseMetric(const std::string& s) {
  if (s == "L2") return DistanceMetric::L2;
  if (s == "DOT") return DistanceMetric::DOT;
  if (s == "COSINE") return DistanceMetric::COSINE;
  throw std::runtime_error("unknown metric: " + s);
}

ConsistencyLevel::type ParseConsistency(const std::string& s) {
  if (s == "ONE") return ConsistencyLevel::ONE;
  if (s == "LOCAL_ONE") return ConsistencyLevel::LOCAL_ONE;
  if (s == "QUORUM") return ConsistencyLevel::QUORUM;
  if (s == "ALL") return ConsistencyLevel::ALL;
  throw std::runtime_error("unknown consistency: " + s);
}

ConsistencyLevel::type ConsistencyOrDefault(const YAML::Node& step) {
  if (step["consistency"]) {
    return ParseConsistency(step["consistency"].as<std::string>());
  }
  return ConsistencyLevel::ONE;
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

class ServerFixture {
 public:
  ServerFixture() {
    Catalog::Options opt;
    opt.data_dir = TempDir("aster_conformance");
    opt.wal_sync = SyncPolicy::kNever;
    auto cat = Catalog::Open(opt);
    if (!cat.ok()) {
      throw std::runtime_error(std::string("Catalog::Open failed: ") +
                               cat.status().message());
    }
    catalog_ = std::move(cat.value());
    handler_ = std::make_shared<AsterHandler>(catalog_.get());

    ThriftServer::Options sop;
    sop.host = "127.0.0.1";
    sop.port = 0;
    server_ = std::make_unique<ThriftServer>(sop, handler_);
    auto st = server_->Listen();
    if (!st.ok()) {
      throw std::runtime_error("ThriftServer::Listen failed");
    }
    serve_thread_ = std::thread([this] { server_->Serve(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ~ServerFixture() {
    if (server_) server_->Stop();
    if (serve_thread_.joinable()) serve_thread_.join();
  }

  uint16_t port() const { return server_->port(); }

 private:
  std::unique_ptr<Catalog> catalog_;
  std::shared_ptr<AsterHandler> handler_;
  std::unique_ptr<ThriftServer> server_;
  std::thread serve_thread_;
};

void ExpectNoError(bool expect_error, bool threw, const std::string& op) {
  if (expect_error) {
    ASSERT_TRUE(threw) << op << " expected AsterError but succeeded";
  } else {
    ASSERT_FALSE(threw) << op << " threw unexpected AsterError";
  }
}

void RunStep(ThriftClient& client, const YAML::Node& step, int index) {
  const std::string op = step["op"].as<std::string>();
  const bool expect_error =
      step["expect_error"] && step["expect_error"].as<bool>();
  bool threw = false;

  try {
    if (op == "createCollection") {
      client->createCollection(step["name"].as<std::string>());
    } else if (op == "configureCollection") {
      CollectionConfig cfg;
      cfg.__set_name(step["name"].as<std::string>());
      VectorConfig vc;
      vc.__set_dimension(step["dimension"].as<int32_t>());
      vc.__set_metric(ParseMetric(step["metric"].as<std::string>()));
      cfg.__set_vector(vc);
      client->configureCollection(cfg);
    } else if (op == "dropCollection") {
      client->dropCollection(step["name"].as<std::string>());
    } else if (op == "upsert") {
      Document doc;
      doc.__set_id(step["id"].as<std::string>());
      doc.__set_vector(EncodeVector(ParseVector(step["vector"])));
      if (step["tags"]) {
        std::set<std::string> tags;
        for (const auto& t : step["tags"]) {
          tags.insert(t.as<std::string>());
        }
        doc.__set_tags(tags);
      }
      if (step["timestampMicros"]) {
        doc.__set_timestampMicros(step["timestampMicros"].as<int64_t>());
      }
      client->upsert(step["collection"].as<std::string>(), doc,
                     ConsistencyOrDefault(step));
    } else if (op == "get") {
      Document got;
      client->get(got, step["collection"].as<std::string>(),
                  step["id"].as<std::string>(), ConsistencyOrDefault(step));
      if (step["expect"]) {
        const auto& exp = step["expect"];
        if (exp["id"]) {
          EXPECT_EQ(got.id, exp["id"].as<std::string>())
              << "step " << index << " get id";
        }
        if (exp["tags"]) {
          ASSERT_TRUE(got.__isset.tags) << "step " << index << " missing tags";
          for (const auto& t : exp["tags"]) {
            EXPECT_EQ(got.tags.count(t.as<std::string>()), 1u)
                << "step " << index << " missing tag " << t.as<std::string>();
          }
        }
      }
    } else if (op == "remove") {
      client->remove(step["collection"].as<std::string>(),
                     step["id"].as<std::string>(), ConsistencyOrDefault(step));
    } else if (op == "search") {
      SearchRequest req;
      req.__set_collection(step["collection"].as<std::string>());
      req.__set_vector(EncodeVector(ParseVector(step["vector"])));
      if (step["topK"]) req.__set_topK(step["topK"].as<int32_t>());
      if (step["efSearch"]) req.__set_efSearch(step["efSearch"].as<int32_t>());
      if (step["tags"]) {
        std::set<std::string> tags;
        for (const auto& t : step["tags"]) {
          tags.insert(t.as<std::string>());
        }
        req.__set_tags(tags);
      }
      if (step["consistency"]) {
        req.__set_consistency(ParseConsistency(step["consistency"].as<std::string>()));
      }
      SearchResponse resp;
      client->search(resp, req);
      if (step["expect"]) {
        const auto& exp = step["expect"];
        ASSERT_FALSE(resp.hits.empty()) << "step " << index << " empty hits";
        if (exp["first_id"]) {
          EXPECT_EQ(resp.hits[0].id, exp["first_id"].as<std::string>())
              << "step " << index << " first_id";
        }
        if (exp["min_score"]) {
          EXPECT_GE(resp.hits[0].score, exp["min_score"].as<double>())
              << "step " << index << " min_score";
        }
      }
    } else {
      FAIL() << "step " << index << " unknown op: " << op;
    }
  } catch (const AsterError& e) {
    threw = true;
    if (!expect_error) {
      FAIL() << "step " << index << " " << op << " AsterError code=" << e.code
             << " message=" << e.message;
    }
  }

  ExpectNoError(expect_error, threw, "step " + std::to_string(index) + " " + op);
}

std::string JoinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

std::string CorpusDir() {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  const char* workspace = std::getenv("TEST_WORKSPACE");
  if (srcdir && workspace) {
    return JoinPath(JoinPath(srcdir, workspace), "clients/conformance/corpus");
  }
  return "clients/conformance/corpus";
}

bool FileExists(const std::string& path) {
  std::ifstream in(path);
  return in.good();
}

std::string StemName(const char* filename) {
  std::string name(filename);
  const auto dot = name.rfind('.');
  if (dot != std::string::npos) name.resize(dot);
  for (char& c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
  }
  return name;
}

void RunCorpusFile(const std::string& path) {
  SCOPED_TRACE(path);
  ASSERT_TRUE(FileExists(path)) << "missing corpus file: " << path;
  const YAML::Node root = YAML::LoadFile(path);
  ASSERT_TRUE(root["steps"]) << path << " missing steps";
  ASSERT_TRUE(root["steps"].IsSequence());

  ServerFixture fixture;
  ThriftClient client(fixture.port());

  int i = 0;
  for (const auto& step : root["steps"]) {
    RunStep(client, step, i++);
    if (::testing::Test::HasFatalFailure()) return;
  }
}

class YamlCorpusTest : public ::testing::TestWithParam<const char*> {};

TEST_P(YamlCorpusTest, RunsAgainstThriftFixture) {
  RunCorpusFile(JoinPath(CorpusDir(), GetParam()));
}

INSTANTIATE_TEST_SUITE_P(
    Corpus, YamlCorpusTest,
    ::testing::Values("basic_lifecycle.yaml",
                      "upsert_requires_configure.yaml",
                      "search_and_remove.yaml"),
    [](const ::testing::TestParamInfo<const char*>& info) {
      return StemName(info.param);
    });

TEST(YamlCorpusDiscovery, KnownCasesExist) {
  const std::string dir = CorpusDir();
  EXPECT_TRUE(FileExists(JoinPath(dir, "basic_lifecycle.yaml")));
  EXPECT_TRUE(FileExists(JoinPath(dir, "upsert_requires_configure.yaml")));
  EXPECT_TRUE(FileExists(JoinPath(dir, "search_and_remove.yaml")));
}

}  // namespace
}  // namespace rpc
}  // namespace aster
