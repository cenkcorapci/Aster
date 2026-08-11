#pragma once

#include <string_view>

namespace aster {
namespace recall_gate {

// Resolves ASTER_RECALL_SCALE / CLI scale name to pr (default) or nightly.
std::string_view ScaleFromEnvOrDefault(const char* fallback);

// Runs SIFT-like (L2, 128-d) and GloVe-like (cosine, 100-d) subsets.
// Asserts mean recall@10 >= 0.95 at ef_search=128 vs exact. Prints lines
// when `print` is true. Returns false if either corpus fails the floor.
bool Run(std::string_view scale, bool print);

}  // namespace recall_gate
}  // namespace aster
