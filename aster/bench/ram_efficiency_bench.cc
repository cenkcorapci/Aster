// Measures sealed-segment anonymous vector heap vs logical payload after
// flush+reopen. Reports heap/anonymous separately from file-backed size.
//
//   bazel run //aster/bench:ram_efficiency_bench -- --n 20000 --dim 768

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>

#include "aster/db/db.h"
#include "aster/index/vector_encoding.h"
#include "aster/storage/sstable.h"

namespace {

double RssMb() {
  struct rusage ru {};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
#if defined(__APPLE__)
  return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
  return static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  size_t n = 10000;
  uint32_t dim = 768;
  const char* encoding = "int8_sq";
  std::string data_dir = "/tmp/aster_ram_eff";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--n") == 0 && i + 1 < argc) {
      n = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--dim") == 0 && i + 1 < argc) {
      dim = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--encoding") == 0 && i + 1 < argc) {
      encoding = argv[++i];
    } else if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
      data_dir = argv[++i];
    }
  }

  aster::VectorEncoding enc = aster::VectorEncoding::kInt8Sq;
  if (std::strcmp(encoding, "float32") == 0) {
    enc = aster::VectorEncoding::kFloat32;
  } else if (std::strcmp(encoding, "float16") == 0) {
    enc = aster::VectorEncoding::kFloat16;
  } else if (std::strcmp(encoding, "binary_rabitq") == 0) {
    enc = aster::VectorEncoding::kBinaryRabitq;
  }

  ::system(("rm -rf '" + data_dir + "' && mkdir -p '" + data_dir + "'").c_str());

  aster::Db::Options opt;
  opt.dimension = dim;
  opt.metric = aster::Metric::kL2;
  opt.vector_encoding = enc;
  opt.data_dir = data_dir;
  opt.storage_policy.vectors = aster::Residency::kCold;
  opt.memtable_flush_bytes = 1ull << 30;
  opt.background_index_build = false;

  auto opened = aster::Db::Open(opt);
  if (!opened.ok()) {
    std::fprintf(stderr, "open failed: %s\n", opened.status().message().c_str());
    return 1;
  }
  auto db = std::move(opened.value());

  const double rss_before = RssMb();
  for (size_t i = 0; i < n; ++i) {
    aster::Row row;
    char id[32];
    std::snprintf(id, sizeof(id), "v%08zu", i);
    row.id = id;
    row.timestamp = static_cast<aster::Timestamp>(i + 1);
    row.vector.assign(dim, 0.0f);
    row.vector[i % dim] = 1.0f;
    if (auto st = db->Upsert(std::move(row)); !st.ok()) {
      std::fprintf(stderr, "upsert failed: %s\n", st.message().c_str());
      return 1;
    }
  }
  if (auto st = db->Flush(); !st.ok()) {
    std::fprintf(stderr, "flush failed: %s\n", st.message().c_str());
    return 1;
  }

  // Reopen to force the mmap sealed path.
  db.reset();
  opened = aster::Db::Open(opt);
  if (!opened.ok()) {
    std::fprintf(stderr, "reopen failed: %s\n", opened.status().message().c_str());
    return 1;
  }
  db = std::move(opened.value());

  const uint64_t raw_f32 = static_cast<uint64_t>(n) * dim * 4ull;
  const uint64_t logical = aster::PackedPayloadBytes(enc, dim, n);
  uint64_t heap = 0;
  uint64_t logical_sum = 0;
  // Probe via a point get + search so pages fault in.
  std::vector<float> q(dim, 0.0f);
  q[0] = 1.0f;
  aster::SearchRequest req;
  req.vector = q;
  req.top_k = 10;
  (void)db->Search(req);
  auto got = db->Get("v00000000");
  if (!got) {
    std::fprintf(stderr, "missing row after reopen\n");
    return 1;
  }

  // Approximate sealed heap from SSTable reader on the on-disk segment.
  // Db does not expose segments publicly; re-open the first .ast.
  std::string seg = data_dir + "/seg_000001.ast";
  auto reader = aster::SstableReader::Open(seg, opt.storage_policy);
  if (reader.ok() && reader.value()->packed_vectors()) {
    heap = reader.value()->packed_vectors()->HeapBytesUsed();
    logical_sum = reader.value()->packed_vectors()->BytesUsed();
  }

  const double rss_after = RssMb();
  std::printf(
      "n=%zu dim=%u encoding=%s\n"
      "raw_float32_bytes=%llu\n"
      "logical_packed_bytes=%llu\n"
      "anonymous_vector_heap_bytes=%llu\n"
      "file_backed=%s\n"
      "rss_max_mb_before=%.2f rss_max_mb_after=%.2f\n"
      "reduction_vs_raw=%.1f%%\n",
      n, dim, encoding,
      static_cast<unsigned long long>(raw_f32),
      static_cast<unsigned long long>(logical_sum ? logical_sum : logical),
      static_cast<unsigned long long>(heap),
      heap == 0 ? "yes" : "no", rss_before, rss_after,
      raw_f32 > 0
          ? (100.0 * (1.0 - static_cast<double>(heap) / static_cast<double>(raw_f32)))
          : 0.0);

  if (heap != 0) {
    std::fprintf(stderr,
                 "FAIL: expected anonymous vector heap 0 for cold mmap, got "
                 "%llu\n",
                 static_cast<unsigned long long>(heap));
    return 2;
  }
  return 0;
}
