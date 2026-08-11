// Aster CLI:
//   bazel run //aster/cli:aster -- demo [--data-dir PATH]
//   bazel run //aster/cli:aster -- serve --data-dir PATH [--port 8080] [--api-key KEY]
//   bazel run //aster/cli:aster -- serve-rpc --data-dir PATH [--port 9090]
//
// `serve` is the Firebase-style HTTP API for local / single-node SaaS.
// `serve-rpc` is the framed-TCP Thrift Aster service (M4-T02).
// Container: ASTER_DATA_DIR=/data

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>

#include "aster/db/db.h"
#include "aster/distributed/ring.h"
#include "aster/rpc/handler.h"
#include "aster/rpc/server.h"
#include "aster/server/catalog.h"
#include "aster/server/http_api.h"

namespace {
constexpr const char* kVersion = "0.1.0-dev";
constexpr uint32_t kDim = 16;
constexpr int kRows = 1000;

void PrintUsage(const char* argv0) {
  std::fprintf(stderr,
               "Usage:\n"
               "  %s demo [--data-dir PATH]\n"
               "  %s serve --data-dir PATH [--host 127.0.0.1] [--port 8080] "
               "[--api-key KEY]\n"
               "  %s serve-rpc --data-dir PATH [--host 127.0.0.1] "
               "[--port 9090]\n"
               "  %s [--data-dir PATH]          # same as demo\n"
               "\n"
               "Environment:\n"
               "  ASTER_DATA_DIR   Default data directory\n"
               "  ASTER_API_KEY    Optional API key for serve\n"
               "\n"
               "Aster %s\n",
               argv0, argv0, argv0, argv0, kVersion);
}

int RunDemo(const std::string& data_dir) {
  std::printf("aster %s (single-node demo)\n", kVersion);
  if (data_dir.empty()) {
    std::printf("mode: in-memory\n\n");
  } else {
    std::printf("mode: durable  data_dir=%s\n\n", data_dir.c_str());
  }

  aster::Db::Options options;
  options.dimension = kDim;
  options.metric = aster::Metric::kCosine;
  options.data_dir = data_dir;

  std::unique_ptr<aster::Db> owned;
  aster::Db* db = nullptr;
  if (!data_dir.empty()) {
    auto opened = aster::Db::Open(options);
    if (!opened.ok()) {
      std::fprintf(stderr, "error: open failed: %s\n",
                   opened.status().message().c_str());
      return 1;
    }
    owned = std::move(opened.value());
    db = owned.get();
  } else {
    owned = std::make_unique<aster::Db>(options);
    db = owned.get();
  }

  std::mt19937 rng(42);
  std::normal_distribution<float> dist(0.0f, 1.0f);

  for (int i = 0; i < kRows; ++i) {
    aster::Row row;
    row.id = "doc-" + std::to_string(i);
    row.vector.resize(kDim);
    for (auto& x : row.vector) x = dist(rng);
    row.timestamp = static_cast<aster::Timestamp>(i + 1);
    row.tags.insert(i % 2 == 0 ? "even" : "odd");
    if (!db->Upsert(std::move(row)).ok()) return 1;
    if ((i + 1) % 250 == 0) {
      if (!db->Flush().ok()) return 1;
    }
  }
  std::printf("inserted %d rows across %zu segments (+ memtable rows: %zu)\n",
              kRows, db->segment_count(), db->memtable_rows());

  aster::SearchRequest req;
  req.vector.resize(kDim);
  for (auto& x : req.vector) x = dist(rng);
  req.top_k = 5;
  req.tags = {"even"};

  auto hits = db->Search(req);
  std::printf("top-%u results (tag filter: even):\n", req.top_k);
  for (const auto& hit : hits) {
    std::printf("  %-10s score=%.4f\n", hit.id.c_str(), hit.score);
  }

  if (!db->Compact().ok()) return 1;
  std::printf("\nafter full compaction: %zu segment(s)\n", db->segment_count());

  aster::Ring ring(64);
  ring.AddNode("node-a");
  ring.AddNode("node-b");
  ring.AddNode("node-c");
  auto replicas = ring.GetReplicas(hits.empty() ? "doc-0" : hits[0].id, 2);
  std::printf("ring placement (RF=2): %s -> [%s, %s]\n",
              hits.empty() ? "doc-0" : hits[0].id.c_str(),
              replicas[0].c_str(), replicas[1].c_str());
  return 0;
}

int RunServe(int argc, char** argv) {
  const char* env_dir = std::getenv("ASTER_DATA_DIR");
  const char* env_key = std::getenv("ASTER_API_KEY");
  std::string data_dir = env_dir ? env_dir : "";
  std::string host = "127.0.0.1";
  uint16_t port = 8080;
  std::string api_key = env_key ? env_key : "";

  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(argv[i], "--data-dir") == 0) {
      data_dir = need("--data-dir");
    } else if (std::strcmp(argv[i], "--host") == 0) {
      host = need("--host");
    } else if (std::strcmp(argv[i], "--port") == 0) {
      port = static_cast<uint16_t>(std::atoi(need("--port")));
    } else if (std::strcmp(argv[i], "--api-key") == 0) {
      api_key = need("--api-key");
    } else {
      std::fprintf(stderr, "error: unknown argument: %s\n", argv[i]);
      PrintUsage(argv[0]);
      return 2;
    }
  }

  if (data_dir.empty()) {
    std::fprintf(stderr, "error: serve requires --data-dir or ASTER_DATA_DIR\n");
    return 2;
  }

  aster::Catalog::Options opt;
  opt.data_dir = data_dir;
  opt.wal_sync = aster::SyncPolicy::kEveryMs;
  auto catalog = aster::Catalog::Open(opt);
  if (!catalog.ok()) {
    std::fprintf(stderr, "error: catalog open failed: %s\n",
                 catalog.status().message().c_str());
    return 1;
  }

  aster::ApiHandler handler(catalog.value().get(), api_key);
  aster::HttpServer::Options hop;
  hop.host = host;
  hop.port = port;
  aster::HttpServer server(hop, handler);
  if (auto st = server.Listen(); !st.ok()) {
    std::fprintf(stderr, "error: listen failed: %s\n", st.message().c_str());
    return 1;
  }

  std::printf("aster %s serve  http://%s:%u\n", kVersion, host.c_str(),
              server.port());
  std::printf("data_dir=%s  api_key=%s\n", data_dir.c_str(),
              api_key.empty() ? "(none)" : "(set)");
  std::printf("endpoints: /health /metrics /v1/collections /v1/usage\n");
  server.Serve();
  return 0;
}

int RunServeRpc(int argc, char** argv) {
  const char* env_dir = std::getenv("ASTER_DATA_DIR");
  std::string data_dir = env_dir ? env_dir : "";
  std::string host = "127.0.0.1";
  uint16_t port = 9090;

  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(argv[i], "--data-dir") == 0) {
      data_dir = need("--data-dir");
    } else if (std::strcmp(argv[i], "--host") == 0) {
      host = need("--host");
    } else if (std::strcmp(argv[i], "--port") == 0) {
      port = static_cast<uint16_t>(std::atoi(need("--port")));
    } else {
      std::fprintf(stderr, "error: unknown argument: %s\n", argv[i]);
      PrintUsage(argv[0]);
      return 2;
    }
  }

  if (data_dir.empty()) {
    std::fprintf(stderr,
                 "error: serve-rpc requires --data-dir or ASTER_DATA_DIR\n");
    return 2;
  }

  aster::Catalog::Options opt;
  opt.data_dir = data_dir;
  opt.wal_sync = aster::SyncPolicy::kEveryMs;
  auto catalog = aster::Catalog::Open(opt);
  if (!catalog.ok()) {
    std::fprintf(stderr, "error: catalog open failed: %s\n",
                 catalog.status().message().c_str());
    return 1;
  }

  auto handler =
      std::make_shared<aster::rpc::AsterHandler>(catalog.value().get());
  aster::rpc::ThriftServer::Options sop;
  sop.host = host;
  sop.port = port;
  aster::rpc::ThriftServer server(sop, handler);
  if (auto st = server.Listen(); !st.ok()) {
    std::fprintf(stderr, "error: thrift listen failed: %s\n",
                 st.message().c_str());
    return 1;
  }

  std::printf("aster %s serve-rpc  thrift://%s:%u (framed TCP)\n", kVersion,
              host.c_str(), server.port());
  std::printf("data_dir=%s\n", data_dir.c_str());
  std::printf("service: Aster (createCollection/upsert/get/remove/search)\n");
  server.Serve();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "serve") == 0) {
    return RunServe(argc, argv);
  }
  if (argc >= 2 && std::strcmp(argv[1], "serve-rpc") == 0) {
    return RunServeRpc(argc, argv);
  }

  // demo (explicit or legacy flags)
  const char* env = std::getenv("ASTER_DATA_DIR");
  std::string data_dir = env ? env : "";
  int start = 1;
  if (argc >= 2 && std::strcmp(argv[1], "demo") == 0) start = 2;
  for (int i = start; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(argv[i], "--data-dir") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: --data-dir requires a path\n");
        return 2;
      }
      data_dir = argv[++i];
      continue;
    }
    std::fprintf(stderr, "error: unknown argument: %s\n", argv[i]);
    PrintUsage(argv[0]);
    return 2;
  }
  return RunDemo(data_dir);
}
