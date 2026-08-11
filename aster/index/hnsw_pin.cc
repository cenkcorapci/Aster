#include "aster/index/hnsw_pin.h"

#if ASTER_ENABLE_HNSW

#include <algorithm>
#include <queue>
#include <unordered_set>
#include <utility>

#include "aster/index/distance.h"

namespace aster {
namespace {

constexpr uint32_t kMaxNodes = 50'000'000u;

uint8_t ReadU8(std::string_view b, size_t& off) {
  return static_cast<uint8_t>(b[off++]);
}
uint16_t ReadU16(std::string_view b, size_t& off) {
  const uint16_t lo = ReadU8(b, off);
  const uint16_t hi = ReadU8(b, off);
  return static_cast<uint16_t>(lo | (hi << 8));
}
uint32_t ReadU32(std::string_view b, size_t& off) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(ReadU8(b, off)) << (8 * i);
  }
  return v;
}
uint64_t ReadU64(std::string_view b, size_t& off) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(ReadU8(b, off)) << (8 * i);
  }
  return v;
}

Status Need(std::string_view b, size_t off, size_t n) {
  if (off > b.size() || n > b.size() - off) {
    return Status::Corruption("hnsw pin: truncated file");
  }
  return Status::Ok();
}

uint32_t MaxDegreeForLayer(const HnswParams& p, uint16_t layer) {
  return layer == 0 ? HnswLayer0MaxDegree(p) : p.m;
}

struct ScoredNode {
  float score = 0.0f;
  uint32_t id = 0;
};

struct BetterFirst {
  bool operator()(const ScoredNode& a, const ScoredNode& b) const {
    if (a.score != b.score) return a.score < b.score;
    return a.id > b.id;
  }
};

struct WorseFirst {
  bool operator()(const ScoredNode& a, const ScoredNode& b) const {
    if (a.score != b.score) return a.score > b.score;
    return a.id < b.id;
  }
};

float NodeScore(Metric metric, VectorView query,
                const std::vector<std::vector<float>>& vectors, uint32_t id) {
  return Score(metric, query, vectors[id]);
}

std::vector<ScoredNode> SearchLayerPinned(
    Metric metric, VectorView query,
    const std::vector<std::vector<float>>& vectors,
    const HnswUpperLayerPin& pin, HnswLayer0NeighborFn layer0,
    const std::vector<uint32_t>& entry_points, uint32_t ef, uint16_t layer) {
  if (entry_points.empty() || ef == 0) return {};

  std::unordered_set<uint32_t> visited;
  visited.reserve(ef * 4);

  std::priority_queue<ScoredNode, std::vector<ScoredNode>, BetterFirst>
      candidates;
  std::priority_queue<ScoredNode, std::vector<ScoredNode>, WorseFirst> w;

  for (uint32_t ep : entry_points) {
    if (!visited.insert(ep).second) continue;
    const float s = NodeScore(metric, query, vectors, ep);
    candidates.push({s, ep});
    w.push({s, ep});
  }
  if (w.empty()) return {};

  while (w.size() > ef) w.pop();

  while (!candidates.empty()) {
    const ScoredNode c = candidates.top();
    candidates.pop();
    if (c.score < w.top().score) break;

    std::vector<uint32_t> neighbors;
    if (layer == 0) {
      auto got = layer0(c.id);
      if (!got.ok()) return {};
      neighbors = std::move(got).value();
    } else {
      neighbors = pin.Neighbors(c.id, layer);
    }

    for (uint32_t e : neighbors) {
      if (!visited.insert(e).second) continue;
      const float s = NodeScore(metric, query, vectors, e);
      if (w.size() < ef || s > w.top().score) {
        candidates.push({s, e});
        w.push({s, e});
        if (w.size() > ef) w.pop();
      }
    }
  }

  std::vector<ScoredNode> result;
  result.reserve(w.size());
  while (!w.empty()) {
    result.push_back(w.top());
    w.pop();
  }
  std::sort(result.begin(), result.end(),
            [](const ScoredNode& a, const ScoredNode& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.id < b.id;
            });
  return result;
}

}  // namespace

Result<HnswGraphLayout> HnswUpperLayerPin::ComputeLayout(
    std::string_view bytes) {
  if (bytes.size() < static_cast<size_t>(HnswGraph::kHeaderBytes) +
                         HnswGraph::kFooterBytes) {
    return Status::Corruption("hnsw pin: file too small");
  }

  size_t off = 0;
  const uint32_t magic = ReadU32(bytes, off);
  if (magic != HnswGraph::kMagic) {
    return Status::Corruption("hnsw pin: bad magic");
  }
  const uint16_t version = ReadU16(bytes, off);
  if (version != HnswGraph::kFormatVersion) {
    return Status::Corruption("hnsw pin: unsupported version");
  }
  const uint16_t header_bytes = ReadU16(bytes, off);
  if (header_bytes != HnswGraph::kHeaderBytes) {
    return Status::Corruption("hnsw pin: bad header_bytes");
  }
  ReadU32(bytes, off);  // flags
  HnswParams params;
  params.m = ReadU32(bytes, off);
  const uint32_t m0 = ReadU32(bytes, off);
  params.ef_construction = ReadU32(bytes, off);
  params.ef_search_default = ReadU32(bytes, off);
  params.max_layers = ReadU32(bytes, off);
  const uint32_t entry = ReadU32(bytes, off);
  const uint32_t node_count = ReadU32(bytes, off);
  const uint16_t max_level = ReadU16(bytes, off);
  ReadU16(bytes, off);  // reserved0
  ReadU64(bytes, off);  // segment_id
  const uint32_t payload_bytes = ReadU32(bytes, off);
  ReadU32(bytes, off);  // reserved1
  ReadU32(bytes, off);  // header_crc

  if (params.m == 0 || m0 != HnswLayer0MaxDegree(params)) {
    return Status::Corruption("hnsw pin: invalid m/m0");
  }
  if (params.max_layers == 0 || params.max_layers > 255) {
    return Status::Corruption("hnsw pin: invalid max_layers");
  }
  if (node_count > kMaxNodes) {
    return Status::Corruption("hnsw pin: node_count too large");
  }
  if (max_level >= params.max_layers) {
    return Status::Corruption("hnsw pin: max_level exceeds max_layers");
  }

  const size_t expect_total = static_cast<size_t>(HnswGraph::kHeaderBytes) +
                              payload_bytes + HnswGraph::kFooterBytes;
  if (bytes.size() != expect_total) {
    return Status::Corruption("hnsw pin: length check failed");
  }

  HnswGraphLayout layout;
  layout.file_size = bytes.size();
  layout.node_table_begin = HnswGraph::kHeaderBytes;
  layout.node_table_end =
      HnswGraph::kHeaderBytes + static_cast<size_t>(node_count) * 8u;
  layout.footer_begin =
      static_cast<size_t>(HnswGraph::kHeaderBytes) + payload_bytes;
  layout.layer0_offsets.assign(node_count, 0);

  if (layout.node_table_end > layout.footer_begin) {
    return Status::Corruption("hnsw pin: node table overruns body");
  }

  // Read levels so we know which nodes appear on each layer.
  std::vector<uint8_t> levels(node_count);
  size_t bo = layout.node_table_begin;
  for (uint32_t i = 0; i < node_count; ++i) {
    if (auto st = Need(bytes, bo, 8); !st.ok()) return st;
    ReadU32(bytes, bo);  // row_ordinal
    levels[i] = ReadU8(bytes, bo);
    ReadU8(bytes, bo);
    ReadU8(bytes, bo);
    ReadU8(bytes, bo);
    if (levels[i] > max_level) {
      return Status::Corruption("hnsw pin: node level > max_level");
    }
  }

  layout.layer0_begin = bo;
  const uint16_t walk_max = node_count == 0 ? 0 : max_level;
  for (uint16_t layer = 0; layer <= walk_max; ++layer) {
    if (layer == 1) layout.upper_begin = bo;
    for (uint32_t node = 0; node < node_count; ++node) {
      if (levels[node] < layer) continue;
      if (layer == 0) layout.layer0_offsets[node] = bo;
      if (auto st = Need(bytes, bo, 2); !st.ok()) return st;
      const uint16_t degree = ReadU16(bytes, bo);
      if (degree > MaxDegreeForLayer(params, layer)) {
        return Status::Corruption("hnsw pin: degree exceeds max");
      }
      const size_t nb_bytes = static_cast<size_t>(degree) * 4u;
      if (auto st = Need(bytes, bo, nb_bytes); !st.ok()) return st;
      bo += nb_bytes;
    }
    if (layer == 0) layout.layer0_end = bo;
  }

  if (node_count == 0 || max_level == 0) {
    // Flat or empty: no upper adjacency region.
    layout.upper_begin = layout.layer0_end;
    layout.upper_end = layout.layer0_end;
  } else {
    layout.upper_end = bo;
  }

  if (bo != layout.footer_begin) {
    return Status::Corruption("hnsw pin: body length mismatch");
  }
  if (node_count > 0 && entry >= node_count) {
    return Status::Corruption("hnsw pin: entry out of range");
  }
  (void)entry;
  return layout;
}

Result<HnswUpperLayerPin> HnswUpperLayerPin::FromSerialized(
    std::string_view bytes) {
  auto layout_r = ComputeLayout(bytes);
  if (!layout_r.ok()) return layout_r.status();

  auto loaded = HnswGraph::Load(bytes);
  if (!loaded.ok()) return loaded.status();
  const HnswGraph& g = loaded.value();

  HnswUpperLayerPin pin;
  pin.params_ = g.params();
  pin.layout_ = std::move(layout_r).value();
  pin.segment_id_ = g.segment_id();
  pin.entry_point_ = g.entry_point();
  pin.max_level_ = g.max_level();
  pin.levels_.resize(g.node_count());
  pin.row_ordinals_.resize(g.node_count());
  for (uint32_t i = 0; i < g.node_count(); ++i) {
    pin.levels_[i] = g.NodeLevel(i);
    pin.row_ordinals_[i] = g.RowOrdinal(i);
  }

  if (g.max_level() >= 1 && g.node_count() > 0) {
    pin.upper_neighbors_.assign(static_cast<size_t>(g.max_level()),
                                std::vector<std::vector<uint32_t>>(
                                    g.node_count()));
    for (uint16_t layer = 1; layer <= g.max_level(); ++layer) {
      auto& layer_vec = pin.upper_neighbors_[layer - 1];
      for (uint32_t node = 0; node < g.node_count(); ++node) {
        if (g.NodeLevel(node) < layer) continue;
        layer_vec[node] = g.Neighbors(node, layer);
      }
    }
  }
  return pin;
}

uint8_t HnswUpperLayerPin::NodeLevel(uint32_t node) const {
  return levels_.at(node);
}

uint32_t HnswUpperLayerPin::RowOrdinal(uint32_t node) const {
  return row_ordinals_.at(node);
}

const std::vector<uint32_t>& HnswUpperLayerPin::Neighbors(
    uint32_t node, uint16_t layer) const {
  static const std::vector<uint32_t> kEmpty;
  if (layer == 0 || layer > max_level_ || node >= node_count()) return kEmpty;
  if (levels_[node] < layer) return kEmpty;
  return upper_neighbors_[layer - 1][node];
}

std::vector<HnswPinRange> HnswUpperLayerPin::PinRanges() const {
  std::vector<HnswPinRange> ranges;
  // Header + node table (entry point, levels, row ordinals).
  ranges.push_back(
      HnswPinRange{0, layout_.node_table_end});
  if (layout_.upper_end > layout_.upper_begin) {
    ranges.push_back(
        HnswPinRange{layout_.upper_begin, layout_.upper_end});
  }
  return ranges;
}

size_t HnswUpperLayerPin::pinned_bytes() const {
  size_t n = levels_.size() + row_ordinals_.size() * sizeof(uint32_t);
  for (const auto& layer : upper_neighbors_) {
    for (const auto& nbs : layer) {
      n += nbs.size() * sizeof(uint32_t) + sizeof(uint16_t);
    }
  }
  return n;
}

Result<std::vector<uint32_t>> HnswReadLayer0Neighbors(
    std::string_view file_bytes, const HnswGraphLayout& layout, uint32_t node) {
  if (node >= layout.layer0_offsets.size()) {
    return Status::InvalidArgument("hnsw pin: layer0 node out of range");
  }
  size_t off = layout.layer0_offsets[node];
  if (auto st = Need(file_bytes, off, 2); !st.ok()) return st;
  const uint16_t degree = ReadU16(file_bytes, off);
  if (auto st = Need(file_bytes, off, static_cast<size_t>(degree) * 4);
      !st.ok()) {
    return st;
  }
  std::vector<uint32_t> nbs(degree);
  for (uint16_t i = 0; i < degree; ++i) nbs[i] = ReadU32(file_bytes, off);
  return nbs;
}

std::vector<std::pair<uint32_t, float>> HnswSearchPinned(
    Metric metric, const HnswUpperLayerPin& pin, HnswLayer0NeighborFn layer0,
    const std::vector<std::vector<float>>& vectors, VectorView query,
    uint32_t top_k, uint32_t ef_search) {
  if (pin.node_count() == 0 || top_k == 0 || query.empty()) return {};
  if (vectors.size() != pin.node_count()) return {};
  if (pin.entry_point() == HnswGraph::kNoEntry) return {};
  if (!layer0) return {};

  uint32_t ef = ef_search;
  if (ef == 0) ef = pin.params().ef_search_default;
  if (ef < top_k) ef = top_k;

  uint32_t ep = pin.entry_point();
  const uint16_t top = pin.max_level();
  for (uint16_t lc = top; lc > 0; --lc) {
    auto nearest = SearchLayerPinned(metric, query, vectors, pin, layer0, {ep},
                                     /*ef=*/1, lc);
    if (!nearest.empty()) ep = nearest.front().id;
  }

  auto found =
      SearchLayerPinned(metric, query, vectors, pin, layer0, {ep}, ef,
                        /*layer=*/0);
  if (found.size() > top_k) found.resize(top_k);

  std::vector<std::pair<uint32_t, float>> out;
  out.reserve(found.size());
  for (const ScoredNode& sn : found) {
    out.emplace_back(sn.id, sn.score);
  }
  return out;
}

}  // namespace aster

#else  // !ASTER_ENABLE_HNSW

namespace aster {
namespace {
[[maybe_unused]] constexpr int kHnswPinDisabledStub = 0;
}  // namespace
}  // namespace aster

#endif  // ASTER_ENABLE_HNSW
