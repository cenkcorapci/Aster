#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "aster/index/distance.h"
#include "aster/index/vector_index.h"

namespace aster {
namespace {

class ExactIndex final : public VectorIndex {
 public:
  ExactIndex(Metric metric, std::vector<IndexEntry> entries) : metric_(metric) {
    ids_.reserve(entries.size());
    vectors_.reserve(entries.size());
    for (auto& e : entries) {
      ids_.push_back(std::move(e.id));
      vectors_.emplace_back(e.vector.begin(), e.vector.end());
    }
  }

  size_t size() const override { return ids_.size(); }

  std::vector<SearchHit> Search(VectorView query, uint32_t top_k,
                                uint32_t /*ef_search*/) const override {
    std::vector<SearchHit> hits;
    hits.reserve(ids_.size());
    for (size_t i = 0; i < ids_.size(); ++i) {
      hits.push_back({ids_[i], Score(metric_, query, vectors_[i])});
    }
    const size_t k = std::min<size_t>(top_k, hits.size());
    std::partial_sort(hits.begin(), hits.begin() + k, hits.end(),
                      [](const SearchHit& a, const SearchHit& b) {
                        return a.score > b.score;
                      });
    hits.resize(k);
    return hits;
  }

 private:
  Metric metric_;
  std::vector<RowId> ids_;
  std::vector<std::vector<float>> vectors_;
};

}  // namespace

std::unique_ptr<VectorIndex> BuildExactIndex(Metric metric,
                                             std::vector<IndexEntry> entries) {
  return std::make_unique<ExactIndex>(metric, std::move(entries));
}

}  // namespace aster
