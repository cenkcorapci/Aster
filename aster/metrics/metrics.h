#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

namespace aster {

// Minimal metrics registry; the Prometheus text-format endpoint
// (docs/design.md, "Monitoring") is exposed by the server in milestone M4.
class MetricsRegistry {
 public:
  static MetricsRegistry& Instance();

  std::atomic<uint64_t>& Counter(const std::string& name);

  // Renders "name value" lines (Prometheus text exposition format subset).
  std::string Render() const;

 private:
  std::map<std::string, std::atomic<uint64_t>> counters_;
};

}  // namespace aster
