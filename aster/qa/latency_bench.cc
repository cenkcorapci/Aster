// M2-T11: latency_bench
//
// Usage:
//   bazel run -c opt //aster/qa:latency_bench -- --scale=smoke
//   bazel run -c opt //aster/qa:latency_bench -- --scale=ci --out-json /tmp/lat.json
//
// The CI gate uses the `ci` scale (1M×384d, ef_search=128) and checks:
//   p50_ms < 5.0

#include <cstdio>
#include <cstring>
#include <string_view>

#include "aster/qa/latency_bench_lib.h"

int main(int argc, char** argv) {
  std::string_view scale = "smoke";
  double p50_threshold_ms = 5.0;
  std::string_view out_json_path;
  bool print = true;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") {
      std::printf(
          "Usage: latency_bench [--scale=smoke|ci] [--out-json PATH] "
          "[--p50-threshold-ms N] [--quiet]\n");
      return 0;
    } else if (a.rfind("--scale=", 0) == 0) {
      scale = argv[i] + std::strlen("--scale=");
    } else if (a == "--scale" && i + 1 < argc) {
      scale = argv[++i];
    } else if (a.rfind("--out-json=", 0) == 0) {
      out_json_path = argv[i] + std::strlen("--out-json=");
    } else if (a == "--out-json" && i + 1 < argc) {
      out_json_path = argv[++i];
    } else if (a.rfind("--p50-threshold-ms=", 0) == 0) {
      p50_threshold_ms =
          std::strtod(argv[i] + std::strlen("--p50-threshold-ms="), nullptr);
    } else if (a == "--p50-threshold-ms" && i + 1 < argc) {
      p50_threshold_ms = std::strtod(argv[++i], nullptr);
    } else if (a == "--quiet") {
      print = false;
    } else {
      std::fprintf(stderr, "latency_bench: unknown flag: %s\n", argv[i]);
      return 2;
    }
  }

  aster::latency_bench::Result r;
  const bool ok = aster::latency_bench::Run(scale, print, p50_threshold_ms,
                                              out_json_path, &r);
  // Use distinct exit codes so CI can differentiate harness vs gate failure.
  return ok ? 0 : 2;
}

