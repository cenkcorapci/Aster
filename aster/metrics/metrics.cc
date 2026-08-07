#include "aster/metrics/metrics.h"

namespace aster {

MetricsRegistry& MetricsRegistry::Instance() {
  static MetricsRegistry instance;
  return instance;
}

std::atomic<uint64_t>& MetricsRegistry::Counter(const std::string& name) {
  return counters_[name];
}

std::string MetricsRegistry::Render() const {
  std::string out;
  for (const auto& [name, value] : counters_) {
    out += name + " " + std::to_string(value.load()) + "\n";
  }
  return out;
}

}  // namespace aster
