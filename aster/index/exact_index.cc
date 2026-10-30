#include <algorithm>
#include <cmath>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

#include "aster/index/distance.h"
#include "aster/index/vector_encoding.h"
#include "aster/index/vector_index.h"

namespace aster {
namespace {

// Exact index over a shared row vector (one float copy in Segment::rows).
// Search keeps a bounded min-heap of size top_k — O(k) scratch, not O(n).
class ExactIndex final : public VectorIndex {
 public:
  ExactIndex(Metric metric, std::shared_ptr<const std::vector<Row>> rows)
      : metric_(metric), rows_(std::move(rows)) {
    if (!rows_) return;
    live_.reserve(rows_->size());
    for (size_t i = 0; i < rows_->size(); ++i) {
      if (!(*rows_)[i].tombstone && !(*rows_)[i].vector.empty()) {
        live_.push_back(i);
      }
    }
    if (metric_ == Metric::kCosine) {
      norms_.resize(live_.size());
      for (size_t j = 0; j < live_.size(); ++j) {
        const auto& v = (*rows_)[live_[j]].vector;
        float n2 = 0.0f;
        for (float x : v) n2 += x * x;
        norms_[j] = std::sqrt(n2);
      }
    }
  }

  size_t size() const override { return live_.size(); }

  std::vector<SearchHit> Search(VectorView query, uint32_t top_k,
                                uint32_t /*ef_search*/) const override {
    if (live_.empty() || top_k == 0 || query.empty()) return {};

    float qnorm = 0.0f;
    if (metric_ == Metric::kCosine) {
      for (float x : query) qnorm += x * x;
      qnorm = std::sqrt(qnorm);
    }

    // Min-heap of (score, live_ordinal); keeps the top_k highest scores.
    using Node = std::pair<float, size_t>;
    auto worse = [](const Node& a, const Node& b) { return a.first > b.first; };
    std::priority_queue<Node, std::vector<Node>, decltype(worse)> heap(worse);

    for (size_t j = 0; j < live_.size(); ++j) {
      const Row& row = (*rows_)[live_[j]];
      float score;
      if (metric_ == Metric::kCosine) {
        score = CosineSimilarityPreNorm(query, qnorm, row.vector, norms_[j]);
      } else {
        score = Score(metric_, query, row.vector);
      }
      if (heap.size() < top_k) {
        heap.emplace(score, j);
      } else if (score > heap.top().first) {
        heap.pop();
        heap.emplace(score, j);
      }
    }

    std::vector<SearchHit> hits;
    hits.resize(heap.size());
    for (size_t i = hits.size(); i > 0; --i) {
      const auto [score, j] = heap.top();
      heap.pop();
      hits[i - 1] = {(*rows_)[live_[j]].id, score};
    }
    return hits;
  }

 private:
  Metric metric_;
  std::shared_ptr<const std::vector<Row>> rows_;
  std::vector<size_t> live_;
  std::vector<float> norms_;  // parallel to live_ when metric is Cosine
};

// Exact scan that decodes compact codes into a reusable scratch buffer.
class ExactIndexPacked final : public VectorIndex {
 public:
  ExactIndexPacked(Metric metric,
                   std::shared_ptr<const std::vector<Row>> rows,
                   std::shared_ptr<const CompactVectorStore> packed,
                   std::shared_ptr<const std::vector<uint32_t>> live_slots)
      : metric_(metric),
        rows_(std::move(rows)),
        packed_(std::move(packed)),
        live_slots_(std::move(live_slots)) {
    if (!rows_ || !packed_ || !live_slots_) return;
    live_.reserve(rows_->size());
    for (size_t i = 0; i < rows_->size(); ++i) {
      if ((*rows_)[i].tombstone) continue;
      if (i >= live_slots_->size()) continue;
      if ((*live_slots_)[i] == 0xFFFFFFFFu) continue;
      live_.push_back(i);
    }
    scratch_.resize(packed_->dimension());
    if (metric_ == Metric::kCosine) {
      norms_.resize(live_.size());
      for (size_t j = 0; j < live_.size(); ++j) {
        const uint32_t slot = (*live_slots_)[live_[j]];
        if (!packed_->Decode(slot, scratch_).ok()) {
          norms_[j] = 0.0f;
          continue;
        }
        float n2 = 0.0f;
        for (float x : scratch_) n2 += x * x;
        norms_[j] = std::sqrt(n2);
      }
    }
  }

  size_t size() const override { return live_.size(); }

  std::vector<SearchHit> Search(VectorView query, uint32_t top_k,
                                uint32_t /*ef_search*/) const override {
    if (live_.empty() || top_k == 0 || query.empty() || !packed_) return {};

    float qnorm = 0.0f;
    if (metric_ == Metric::kCosine) {
      for (float x : query) qnorm += x * x;
      qnorm = std::sqrt(qnorm);
    }

    using Node = std::pair<float, size_t>;
    auto worse = [](const Node& a, const Node& b) { return a.first > b.first; };
    std::priority_queue<Node, std::vector<Node>, decltype(worse)> heap(worse);

    // Mutable scratch: Search is const but decoding needs a buffer. Use a
    // thread-local to stay re-entrant under concurrent readers.
    thread_local std::vector<float> tls_scratch;
    tls_scratch.resize(packed_->dimension());

    for (size_t j = 0; j < live_.size(); ++j) {
      const size_t ord = live_[j];
      const uint32_t slot = (*live_slots_)[ord];
      const auto packed = packed_->PackedAt(slot);
      float score;
      if (metric_ == Metric::kCosine) {
        if (!packed_->Decode(slot, tls_scratch).ok()) continue;
        score = CosineSimilarityPreNorm(query, qnorm, tls_scratch, norms_[j]);
      } else {
        score = ScorePacked(metric_, packed_->encoding(), packed_->dimension(),
                            query, packed);
      }
      if (heap.size() < top_k) {
        heap.emplace(score, j);
      } else if (score > heap.top().first) {
        heap.pop();
        heap.emplace(score, j);
      }
    }

    std::vector<SearchHit> hits(heap.size());
    for (size_t i = hits.size(); i > 0; --i) {
      const auto [score, j] = heap.top();
      heap.pop();
      hits[i - 1] = {(*rows_)[live_[j]].id, score};
    }
    return hits;
  }

 private:
  Metric metric_;
  std::shared_ptr<const std::vector<Row>> rows_;
  std::shared_ptr<const CompactVectorStore> packed_;
  std::shared_ptr<const std::vector<uint32_t>> live_slots_;
  std::vector<size_t> live_;
  std::vector<float> norms_;
  mutable std::vector<float> scratch_;
};

}  // namespace

std::unique_ptr<VectorIndex> BuildExactIndex(Metric metric,
                                             std::vector<IndexEntry> entries) {
  auto rows = std::make_shared<std::vector<Row>>();
  rows->reserve(entries.size());
  for (auto& e : entries) {
    Row row;
    row.id = std::move(e.id);
    row.vector.assign(e.vector.begin(), e.vector.end());
    rows->push_back(std::move(row));
  }
  return std::make_unique<ExactIndex>(metric, std::move(rows));
}

std::unique_ptr<VectorIndex> BuildExactIndex(
    Metric metric, std::shared_ptr<const std::vector<Row>> rows) {
  return std::make_unique<ExactIndex>(metric, std::move(rows));
}

std::unique_ptr<VectorIndex> BuildExactIndex(
    Metric metric, std::shared_ptr<const std::vector<Row>> rows,
    std::shared_ptr<const CompactVectorStore> packed,
    std::shared_ptr<const std::vector<uint32_t>> live_slots) {
  return std::make_unique<ExactIndexPacked>(metric, std::move(rows),
                                            std::move(packed),
                                            std::move(live_slots));
}

}  // namespace aster
