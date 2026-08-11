#include "aster/rpc/handler.h"

#include <chrono>
#include <cstring>

#include "aster/core/status.h"
#include "aster/core/types.h"

namespace aster {
namespace rpc {
namespace {

[[noreturn]] void ThrowStatus(const Status& st) {
  AsterError err;
  err.__set_code(static_cast<int32_t>(st.code()));
  err.__set_message(st.message().empty() ? "error" : st.message());
  throw err;
}

void ThrowIfNotOk(const Status& st) {
  if (!st.ok()) ThrowStatus(st);
}

Metric ToMetric(DistanceMetric::type m) {
  switch (m) {
    case DistanceMetric::L2:
      return Metric::kL2;
    case DistanceMetric::DOT:
      return Metric::kDot;
    case DistanceMetric::COSINE:
      return Metric::kCosine;
  }
  ThrowStatus(Status::InvalidArgument("unknown distance metric"));
}

std::vector<float> DecodeVector(const std::string& bytes) {
  if (bytes.size() % sizeof(float) != 0) {
    ThrowStatus(Status::InvalidArgument("vector byte length not multiple of 4"));
  }
  const size_t n = bytes.size() / sizeof(float);
  std::vector<float> out(n);
  if (n > 0) {
    std::memcpy(out.data(), bytes.data(), bytes.size());
  }
  return out;
}

std::string EncodeVector(const std::vector<float>& v) {
  std::string out(v.size() * sizeof(float), '\0');
  if (!v.empty()) {
    std::memcpy(out.data(), v.data(), out.size());
  }
  return out;
}

Timestamp NowMicros() {
  return static_cast<Timestamp>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace

AsterHandler::AsterHandler(Catalog* catalog) : catalog_(catalog) {}

void AsterHandler::createCollection(const CollectionConfig& config) {
  CollectionInfo info;
  info.name = config.name;
  if (config.vector.dimension <= 0) {
    ThrowStatus(Status::InvalidArgument("dimension must be > 0"));
  }
  info.dimension = static_cast<uint32_t>(config.vector.dimension);
  info.metric = ToMetric(config.vector.metric);
  ThrowIfNotOk(catalog_->CreateCollection(info));
}

void AsterHandler::dropCollection(const std::string& name) {
  ThrowIfNotOk(catalog_->DropCollection(name));
}

void AsterHandler::upsert(const std::string& collection, const Document& doc,
                          const ConsistencyLevel::type /*consistency*/) {
  // Consistency is single-node ONE today; QUORUM/ALL land with M7.
  Row row;
  row.id = doc.id;
  row.vector = DecodeVector(doc.vector);
  if (doc.__isset.metadata) row.metadata = doc.metadata;
  if (doc.__isset.tags) row.tags = doc.tags;
  if (doc.__isset.timestampMicros) {
    row.timestamp = static_cast<Timestamp>(doc.timestampMicros);
  } else {
    row.timestamp = NowMicros();
  }
  ThrowIfNotOk(catalog_->Upsert(collection, std::move(row)));
}

void AsterHandler::get(Document& _return, const std::string& collection,
                       const std::string& id,
                       const ConsistencyLevel::type /*consistency*/) {
  auto got = catalog_->Get(collection, id);
  if (!got.ok()) ThrowStatus(got.status());
  if (!got.value().has_value()) {
    ThrowStatus(Status::NotFound("document not found"));
  }
  const Row& row = *got.value();
  _return.__set_id(row.id);
  _return.__set_vector(EncodeVector(row.vector));
  if (!row.metadata.empty()) _return.__set_metadata(row.metadata);
  if (!row.tags.empty()) _return.__set_tags(row.tags);
  _return.__set_timestampMicros(static_cast<int64_t>(row.timestamp));
}

void AsterHandler::remove(const std::string& collection, const std::string& id,
                          const ConsistencyLevel::type /*consistency*/) {
  ThrowIfNotOk(catalog_->Delete(collection, id, NowMicros()));
}

void AsterHandler::search(SearchResponse& _return, const SearchRequest& req) {
  ::aster::SearchRequest local;
  local.vector = DecodeVector(req.vector);
  local.top_k = req.__isset.topK ? static_cast<uint32_t>(req.topK) : 10u;
  if (req.__isset.efSearch) {
    local.ef_search = static_cast<uint32_t>(req.efSearch);
  }
  if (req.__isset.tags) local.tags = req.tags;

  auto hits = catalog_->Search(req.collection, local);
  if (!hits.ok()) ThrowStatus(hits.status());

  _return.hits.clear();
  _return.hits.reserve(hits.value().size());
  for (const auto& h : hits.value()) {
    SearchHit out;
    out.__set_id(h.id);
    out.__set_score(static_cast<double>(h.score));
    _return.hits.push_back(std::move(out));
  }
}

}  // namespace rpc
}  // namespace aster
