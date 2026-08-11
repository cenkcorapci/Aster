// Laptop write microbench: upserts/sec with flush + compaction (M1-T14).
//
//   bazel run //aster/qa:write_bench -- [flags]
//
// Flags:
//   --rows N          Number of upserts (default 200000)
//   --dim D           Vector dimension (default 32)
//   --flush-every N   Explicit Flush every N upserts (default 0 = size trigger)
//   --data-dir PATH   Durable dir (default: mkdtemp under /tmp)
//   --wal-sync MODE   always|everyms|never (default never for throughput)
//   --warmup N        Upserts before timing (default 1000)
//   --json            Emit one JSON object on stdout

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aster/db/db.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
  uint64_t rows = 300000;
  uint32_t dim = 8;
  int flush_every = 8000;
  std::string data_dir;
  aster::SyncPolicy wal_sync = aster::SyncPolicy::kNever;
  uint64_t warmup = 2000;
  bool json = false;
  size_t memtable_flush_bytes = 2 << 20;
  size_t compaction_tier_threshold = 4;
  size_t max_segments_before_compact = 8;
};

void Usage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s [--rows N] [--dim D] [--flush-every N] "
               "[--data-dir PATH] [--wal-sync always|everyms|never] "
               "[--warmup N] [--json]\n",
               argv0);
}

Config Parse(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      Usage(argv[0]);
      std::exit(0);
    } else if (std::strcmp(argv[i], "--rows") == 0) {
      c.rows = static_cast<uint64_t>(std::strtoull(need("--rows"), nullptr, 10));
    } else if (std::strcmp(argv[i], "--dim") == 0) {
      c.dim = static_cast<uint32_t>(std::strtoul(need("--dim"), nullptr, 10));
    } else if (std::strcmp(argv[i], "--flush-every") == 0) {
      c.flush_every =
          static_cast<int>(std::strtol(need("--flush-every"), nullptr, 10));
    } else if (std::strcmp(argv[i], "--data-dir") == 0) {
      c.data_dir = need("--data-dir");
    } else if (std::strcmp(argv[i], "--wal-sync") == 0) {
      const char* m = need("--wal-sync");
      if (std::strcmp(m, "always") == 0) {
        c.wal_sync = aster::SyncPolicy::kAlways;
      } else if (std::strcmp(m, "everyms") == 0) {
        c.wal_sync = aster::SyncPolicy::kEveryMs;
      } else if (std::strcmp(m, "never") == 0) {
        c.wal_sync = aster::SyncPolicy::kNever;
      } else {
        std::fprintf(stderr, "error: unknown --wal-sync %s\n", m);
        std::exit(2);
      }
    } else if (std::strcmp(argv[i], "--warmup") == 0) {
      c.warmup =
          static_cast<uint64_t>(std::strtoull(need("--warmup"), nullptr, 10));
    } else if (std::strcmp(argv[i], "--json") == 0) {
      c.json = true;
    } else {
      std::fprintf(stderr, "error: unknown flag %s\n", argv[i]);
      Usage(argv[0]);
      std::exit(2);
    }
  }
  return c;
}

const char* SyncName(aster::SyncPolicy p) {
  switch (p) {
    case aster::SyncPolicy::kAlways:
      return "always";
    case aster::SyncPolicy::kEveryMs:
      return "everyms";
    case aster::SyncPolicy::kNever:
      return "never";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = Parse(argc, argv);
  if (cfg.dim == 0 || cfg.rows == 0) {
    std::fprintf(stderr, "error: --dim and --rows must be > 0\n");
    return 2;
  }

  std::string dir = cfg.data_dir;
  if (dir.empty()) {
    char tmpl[] = "/tmp/aster_write_bench_XXXXXX";
    if (::mkdtemp(tmpl) == nullptr) {
      std::perror("mkdtemp");
      return 1;
    }
    dir = tmpl;
  } else if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    std::perror("mkdir");
    return 1;
  }

  aster::Db::Options opt;
  opt.dimension = cfg.dim;
  opt.metric = aster::Metric::kL2;
  opt.data_dir = dir;
  opt.wal_sync = cfg.wal_sync;
  opt.memtable_flush_bytes = cfg.memtable_flush_bytes;
  opt.memtable_flush_ms = 0;
  opt.compaction_tier_threshold = cfg.compaction_tier_threshold;
  opt.max_segments_before_compact = cfg.max_segments_before_compact;

  auto opened = aster::Db::Open(opt);
  if (!opened.ok()) {
    std::fprintf(stderr, "open failed: %s\n", opened.status().message().c_str());
    return 1;
  }
  auto& db = *opened.value();

  // Reuse scratch buffers so the timed loop measures engine cost, not alloc.
  aster::Row scratch;
  scratch.vector.assign(cfg.dim, 0.0f);
  if (cfg.dim > 1) scratch.vector[1] = 1.0f;
  char idbuf[32];

  auto upsert_n = [&](uint64_t start, uint64_t n, bool do_flush) -> bool {
    for (uint64_t i = 0; i < n; ++i) {
      const uint64_t id = start + i;
      std::snprintf(idbuf, sizeof(idbuf), "%08llx",
                    static_cast<unsigned long long>(id));
      scratch.id.assign(idbuf);
      scratch.vector[0] = static_cast<float>(id & 0xffffu) * 1e-4f;
      scratch.timestamp = id + 1;
      if (!db.Upsert(scratch).ok()) return false;
      if (do_flush && cfg.flush_every > 0 &&
          ((i + 1) % static_cast<uint64_t>(cfg.flush_every)) == 0) {
        if (!db.Flush().ok()) return false;
      }
    }
    return true;
  };

  if (cfg.warmup > 0) {
    if (!upsert_n(0, cfg.warmup, true)) {
      std::fprintf(stderr, "warmup upsert failed\n");
      return 1;
    }
    if (!db.Flush().ok()) {
      std::fprintf(stderr, "warmup flush failed\n");
      return 1;
    }
  }

  const auto t0 = Clock::now();
  if (!upsert_n(cfg.warmup, cfg.rows, true)) {
    std::fprintf(stderr, "timed upsert failed\n");
    return 1;
  }
  if (!db.Flush().ok()) {
    std::fprintf(stderr, "final flush failed\n");
    return 1;
  }
  const auto t1 = Clock::now();
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  const double ups = sec > 0.0 ? static_cast<double>(cfg.rows) / sec : 0.0;

  if (cfg.json) {
    std::printf(
        "{\"rows\":%llu,\"dim\":%u,\"seconds\":%.6f,\"upserts_per_sec\":%.1f,"
        "\"wal_sync\":\"%s\",\"flush_every\":%d,\"segments\":%zu,"
        "\"memtable_rows\":%zu,\"approx_rows\":%zu}\n",
        static_cast<unsigned long long>(cfg.rows), cfg.dim, sec, ups,
        SyncName(cfg.wal_sync), cfg.flush_every, db.segment_count(),
        db.memtable_rows(), db.approximate_row_count());
  } else {
    std::printf(
        "write_bench rows=%llu dim=%u wal_sync=%s flush_every=%d\n"
        "  elapsed_sec=%.4f upserts_per_sec=%.1f\n"
        "  segments=%zu memtable_rows=%zu approx_rows=%zu\n",
        static_cast<unsigned long long>(cfg.rows), cfg.dim,
        SyncName(cfg.wal_sync), cfg.flush_every, sec, ups, db.segment_count(),
        db.memtable_rows(), db.approximate_row_count());
  }
  return 0;
}
