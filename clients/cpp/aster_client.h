#pragma once

// Aster C++ client (facade layer).
//
// The transport and Thrift-generated protocol land in milestone M5; this
// header fixes the public API surface so examples and downstream code can
// be written against it now.
//
// Example:
//   aster::client::Client client({{"10.0.0.1:7000", "10.0.0.2:7000"}});
//   auto products = client.Collection("products");
//   products.Upsert("doc-1", vec, {.tags = {"electronics"}});
//   auto hits = products.Search(query, {.top_k = 10, .ef_search = 128});

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace aster::client {

struct ClientOptions {
  std::vector<std::string> seed_nodes;
  bool tls = false;
  uint32_t request_timeout_ms = 5000;
  uint32_t max_retries = 2;
};

struct UpsertOptions {
  std::set<std::string> tags;
  std::string metadata;  // CBOR bytes
};

struct SearchOptions {
  uint32_t top_k = 10;
  uint32_t ef_search = 0;  // 0 = collection default
  std::set<std::string> tags;
};

struct Hit {
  std::string id;
  double score;
  std::string metadata;
};

class Collection {
 public:
  void Upsert(const std::string& id, std::span<const float> vector,
              const UpsertOptions& options = {});
  void Delete(const std::string& id);
  std::vector<Hit> Search(std::span<const float> vector,
                          const SearchOptions& options = {});
};

class Client {
 public:
  explicit Client(ClientOptions options);
  Collection Collection(const std::string& name);
};

}  // namespace aster::client
