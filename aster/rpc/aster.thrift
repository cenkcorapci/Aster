/**
 * Aster wire protocol (Apache Thrift IDL).
 *
 * This file is the single source of truth for every client library in
 * //clients/... — C++ stubs live in gen-cpp/ (see scripts/regen-thrift-cpp.sh;
 * M4-T01). Per-language client codegen is milestone M5. Transport is framed
 * TCP with optional TLS.
 */

namespace cpp aster.rpc
namespace py aster.rpc
namespace go aster.rpc
namespace rs aster_rpc
namespace java io.aster.rpc
namespace js aster_rpc

enum DistanceMetric {
  L2 = 0,
  DOT = 1,
  COSINE = 2,
}

enum ConsistencyLevel {
  ONE = 0,
  LOCAL_ONE = 1,
  QUORUM = 2,
  ALL = 3,
}

struct VectorConfig {
  1: required i32 dimension,
  2: required DistanceMetric metric,
}

struct HnswConfig {
  1: optional i32 m = 16,
  2: optional i32 efConstruction = 128,
  3: optional i32 efSearchDefault = 64,
}

struct CollectionConfig {
  1: required string name,
  2: required VectorConfig vector,
  3: optional HnswConfig hnsw,
  4: optional i32 replicationFactor = 1,
}

struct Document {
  1: required string id,
  2: required binary vector,          // little-endian float32 array
  3: optional binary metadata,        // CBOR-encoded
  4: optional set<string> tags,
  5: optional i64 timestampMicros,    // assigned by coordinator if absent
}

struct SearchRequest {
  1: required string collection,
  2: required binary vector,
  3: optional i32 topK = 10,
  4: optional i32 efSearch,           // collection default if absent
  5: optional set<string> tags,
  6: optional ConsistencyLevel consistency = ConsistencyLevel.ONE,
}

struct SearchHit {
  1: required string id,
  2: required double score,
  3: optional binary metadata,
}

struct SearchResponse {
  1: required list<SearchHit> hits,
}

exception AsterError {
  1: required i32 code,
  2: required string message,
}

service Aster {
  // Collection lifecycle:
  //   createCollection: create durable placeholder (dimension/metric unknown yet)
  //   configureCollection: provide vector config, then collection becomes usable
  void createCollection(1: string name) throws (1: AsterError e),
  void configureCollection(1: CollectionConfig config) throws (1: AsterError e),
  void dropCollection(1: string name) throws (1: AsterError e),

  void upsert(1: string collection, 2: Document doc,
              3: ConsistencyLevel consistency) throws (1: AsterError e),
  Document get(1: string collection, 2: string id,
               3: ConsistencyLevel consistency) throws (1: AsterError e),
  void remove(1: string collection, 2: string id,
              3: ConsistencyLevel consistency) throws (1: AsterError e),

  SearchResponse search(1: SearchRequest req) throws (1: AsterError e),
}
