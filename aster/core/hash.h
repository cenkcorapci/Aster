#pragma once

#include <cstdint>
#include <string_view>

namespace aster {

// 64-bit hash used for ring placement and bloom filters.
// Placeholder implementation (FNV-1a + finalizer); will be replaced by
// XXH3-128 per docs/design.md when the hashing dependency is added.
uint64_t Hash64(std::string_view data, uint64_t seed = 0);

}  // namespace aster
