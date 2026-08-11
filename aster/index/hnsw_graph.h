#pragma once

#include "aster/core/features.h"

#if ASTER_ENABLE_HNSW

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aster/core/status.h"

namespace aster {

// HNSW build/search parameters. See docs/indexing.md for how these interact
// with recall, latency, and memory. Layer 0 uses M0 = 2·M neighbor slots.
struct HnswParams {
  uint32_t m = 16;  // max neighbors per node (layers ≥ 1); layer 0 uses 2·m
  uint32_t ef_construction = 128;
  uint32_t ef_search_default = 64;
  uint32_t max_layers = 16;
};

inline uint32_t HnswLayer0MaxDegree(const HnswParams& p) { return 2u * p.m; }

// Immutable per-segment HNSW adjacency (topology only). Vectors live in the
// sibling SSTable; `RowOrdinal(i)` maps graph node i to a live-row slot.
// On-disk layout: docs/hnsw-format.md. Insert/search land in M2-T02/T03.
class HnswGraph {
 public:
  static constexpr uint32_t kNoEntry = 0xFFFFFFFFu;
  static constexpr uint32_t kMagic = 0x41535448u;        // "ASTH"
  static constexpr uint32_t kFooterMagic = 0x41535447u;  // "ASTG"
  static constexpr uint16_t kFormatVersion = 1;
  static constexpr uint16_t kHeaderBytes = 64;
  static constexpr uint16_t kFooterBytes = 16;

  HnswGraph() = default;
  explicit HnswGraph(HnswParams params);

  const HnswParams& params() const { return params_; }
  uint64_t segment_id() const { return segment_id_; }
  void set_segment_id(uint64_t id) { segment_id_ = id; }

  uint32_t node_count() const { return static_cast<uint32_t>(levels_.size()); }
  uint32_t entry_point() const { return entry_point_; }
  uint16_t max_level() const { return max_level_; }

  uint8_t NodeLevel(uint32_t node) const;
  uint32_t RowOrdinal(uint32_t node) const;
  const std::vector<uint32_t>& Neighbors(uint32_t node, uint16_t layer) const;

  // Append a node present on layers 0..level (inclusive). Returns node index.
  Result<uint32_t> AddNode(uint32_t row_ordinal, uint8_t level);

  // Replace the neighbor list at (node, layer). Neighbors must be valid node
  // indices; degree must not exceed M0 (layer 0) or M (layer > 0).
  Status SetNeighbors(uint32_t node, uint16_t layer,
                       std::vector<uint32_t> neighbors);

  Status set_entry_point(uint32_t node);

  bool operator==(const HnswGraph& other) const;
  bool operator!=(const HnswGraph& other) const { return !(*this == other); }

  std::string Serialize() const;
  Status WriteToFile(const std::string& path) const;

  static Result<HnswGraph> Load(std::string_view bytes);
  static Result<HnswGraph> LoadFromFile(const std::string& path);

 private:
  uint32_t MaxDegreeForLayer(uint16_t layer) const;

  HnswParams params_;
  uint64_t segment_id_ = 0;
  uint32_t entry_point_ = kNoEntry;
  uint16_t max_level_ = 0;
  std::vector<uint8_t> levels_;
  std::vector<uint32_t> row_ordinals_;
  // neighbors_[layer][node] — empty when node.level < layer.
  std::vector<std::vector<std::vector<uint32_t>>> neighbors_;
};

}  // namespace aster

#endif  // ASTER_ENABLE_HNSW
