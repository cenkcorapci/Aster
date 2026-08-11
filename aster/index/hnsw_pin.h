#pragma once

#include "aster/core/features.h"

#if ASTER_ENABLE_HNSW

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"
#include "aster/index/hnsw_graph.h"

namespace aster {

// Byte-range layout of a serialized `.hnsw` (docs/hnsw-format.md) so S3
// backends can pin upper layers without retaining layer 0 in RAM.
// Offsets are absolute file offsets; end values are exclusive.
struct HnswGraphLayout {
  size_t file_size = 0;
  size_t node_table_begin = HnswGraph::kHeaderBytes;
  size_t node_table_end = HnswGraph::kHeaderBytes;
  size_t layer0_begin = HnswGraph::kHeaderBytes;
  size_t layer0_end = HnswGraph::kHeaderBytes;
  size_t upper_begin = HnswGraph::kHeaderBytes;  // layers ≥ 1 adjacency
  size_t upper_end = HnswGraph::kHeaderBytes;
  size_t footer_begin = 0;

  // File offset of each layer-0 neighbor list (u16 degree + u32[degree]),
  // indexed by node id. Empty when node_count == 0.
  std::vector<size_t> layer0_offsets;
};

// Contiguous [start, end) ranges that must stay local for upper-layer search.
// Two ranges when max_level > 0 (header+nodes, then upper adjacency); one
// when the graph is flat (header+nodes only — no upper adjacency).
struct HnswPinRange {
  size_t start = 0;
  size_t end = 0;  // exclusive
};

// Local pin of entry point + node table + layers ≥ 1. Layer 0 is omitted so
// cold S3 search does not keep (or re-fetch) the bulk adjacency each query.
// See docs/indexing.md §10.3.1.
class HnswUpperLayerPin {
 public:
  HnswUpperLayerPin() = default;

  static Result<HnswUpperLayerPin> FromSerialized(std::string_view bytes);
  static Result<HnswGraphLayout> ComputeLayout(std::string_view bytes);

  const HnswParams& params() const { return params_; }
  const HnswGraphLayout& layout() const { return layout_; }
  uint64_t segment_id() const { return segment_id_; }
  uint32_t entry_point() const { return entry_point_; }
  uint16_t max_level() const { return max_level_; }
  uint32_t node_count() const { return static_cast<uint32_t>(levels_.size()); }

  uint8_t NodeLevel(uint32_t node) const;
  uint32_t RowOrdinal(uint32_t node) const;

  // Layers ≥ 1 only. Layer 0 always returns an empty list (not pinned).
  const std::vector<uint32_t>& Neighbors(uint32_t node, uint16_t layer) const;

  // Byte ranges to pin in an object-store cache (non-evictable).
  std::vector<HnswPinRange> PinRanges() const;

  // RAM footprint of the pinned adjacency + node table (excludes layout
  // offset vector overhead beyond the vectors themselves).
  size_t pinned_bytes() const;

  // True when layers ≥ 1 are held locally (always true for a successful pin
  // of a non-empty multi-layer graph; also true for flat graphs).
  bool upper_layers_local() const { return true; }

 private:
  HnswParams params_;
  HnswGraphLayout layout_;
  uint64_t segment_id_ = 0;
  uint32_t entry_point_ = HnswGraph::kNoEntry;
  uint16_t max_level_ = 0;
  std::vector<uint8_t> levels_;
  std::vector<uint32_t> row_ordinals_;
  // neighbors_[layer_index][node] for layer_index = layer - 1 (layers ≥ 1).
  std::vector<std::vector<std::vector<uint32_t>>> upper_neighbors_;
};

// Fetch one node's layer-0 neighbor list from a serialized `.hnsw` view
// using layout.layer0_offsets (no full-graph reparse).
Result<std::vector<uint32_t>> HnswReadLayer0Neighbors(
    std::string_view file_bytes, const HnswGraphLayout& layout, uint32_t node);

using HnswLayer0NeighborFn =
    std::function<Result<std::vector<uint32_t>>(uint32_t node)>;

// HNSW search that walks layers ≥ 1 exclusively from `pin` (no layer-0 I/O
// during descent) and resolves layer-0 neighbors via `layer0`.
// Same return contract as HnswSearch (node_id, score), best first.
std::vector<std::pair<uint32_t, float>> HnswSearchPinned(
    Metric metric, const HnswUpperLayerPin& pin, HnswLayer0NeighborFn layer0,
    const std::vector<std::vector<float>>& vectors, VectorView query,
    uint32_t top_k, uint32_t ef_search);

// Documented cold-search wall-clock bound for the M8-T02 FakeS3 fixture
// (loopback, pin resident, LRU cleared). See docs/indexing.md §10.3.1.
// Allows for per-neighbor Range GET RTT on FakeS3 while still excluding
// pathological full-object re-fetch on every query.
inline constexpr int kHnswS3ColdSearchBoundMs = 100;

}  // namespace aster

#endif  // ASTER_ENABLE_HNSW
