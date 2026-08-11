#include "aster/index/hnsw_graph.h"

#if ASTER_ENABLE_HNSW

#include <array>
#include <cstring>
#include <fstream>
#include <utility>

namespace aster {
namespace {

constexpr uint32_t kMaxNodes = 50'000'000u;

std::array<uint32_t, 256> MakeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

// IEEE CRC-32 (same poly as aster::Crc32 in storage/wal.cc).
uint32_t Crc32Local(const void* data, size_t size) {
  static const std::array<uint32_t, 256> kTable = MakeCrcTable();
  uint32_t c = 0xFFFFFFFFu;
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < size; ++i) {
    c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

void AppendU8(std::string& b, uint8_t v) { b.push_back(static_cast<char>(v)); }
void AppendU16(std::string& b, uint16_t v) {
  AppendU8(b, static_cast<uint8_t>(v));
  AppendU8(b, static_cast<uint8_t>(v >> 8));
}
void AppendU32(std::string& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) AppendU8(b, static_cast<uint8_t>(v >> (8 * i)));
}
void AppendU64(std::string& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) AppendU8(b, static_cast<uint8_t>(v >> (8 * i)));
}

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
    return Status::Corruption("hnsw: truncated file");
  }
  return Status::Ok();
}

}  // namespace

HnswGraph::HnswGraph(HnswParams params) : params_(params) {}

uint32_t HnswGraph::MaxDegreeForLayer(uint16_t layer) const {
  return layer == 0 ? HnswLayer0MaxDegree(params_) : params_.m;
}

uint8_t HnswGraph::NodeLevel(uint32_t node) const {
  return levels_[node];
}

uint32_t HnswGraph::RowOrdinal(uint32_t node) const {
  return row_ordinals_[node];
}

const std::vector<uint32_t>& HnswGraph::Neighbors(uint32_t node,
                                                  uint16_t layer) const {
  return neighbors_[layer][node];
}

Result<uint32_t> HnswGraph::AddNode(uint32_t row_ordinal, uint8_t level) {
  if (params_.m == 0) {
    return Status::InvalidArgument("hnsw: m must be > 0");
  }
  if (params_.max_layers == 0 || params_.max_layers > 255) {
    return Status::InvalidArgument("hnsw: max_layers out of range");
  }
  if (level >= params_.max_layers) {
    return Status::InvalidArgument("hnsw: node level exceeds max_layers");
  }
  if (node_count() >= kMaxNodes) {
    return Status(StatusCode::kResourceExhausted, "hnsw: node count limit");
  }

  const uint32_t id = node_count();
  levels_.push_back(level);
  row_ordinals_.push_back(row_ordinal);

  if (level > max_level_) {
    const uint16_t old = max_level_;
    max_level_ = level;
    neighbors_.resize(static_cast<size_t>(max_level_) + 1);
    for (uint16_t L = static_cast<uint16_t>(old + 1); L <= max_level_; ++L) {
      neighbors_[L].resize(id);  // prior nodes absent at new layers
    }
  }
  if (neighbors_.empty()) {
    neighbors_.resize(1);
  }
  for (uint16_t L = 0; L <= max_level_; ++L) {
    neighbors_[L].resize(id + 1);
  }

  if (entry_point_ == kNoEntry || level > NodeLevel(entry_point_)) {
    entry_point_ = id;
  }
  return id;
}

Status HnswGraph::SetNeighbors(uint32_t node, uint16_t layer,
                               std::vector<uint32_t> neighbors) {
  if (node >= node_count()) {
    return Status::InvalidArgument("hnsw: node out of range");
  }
  if (layer > NodeLevel(node)) {
    return Status::InvalidArgument("hnsw: layer above node level");
  }
  if (neighbors.size() > MaxDegreeForLayer(layer)) {
    return Status::InvalidArgument("hnsw: degree exceeds layer max");
  }
  for (uint32_t nb : neighbors) {
    if (nb >= node_count()) {
      return Status::InvalidArgument("hnsw: neighbor out of range");
    }
    if (nb == node) {
      return Status::InvalidArgument("hnsw: self-loop forbidden");
    }
    if (layer > NodeLevel(nb)) {
      return Status::InvalidArgument("hnsw: neighbor missing on layer");
    }
  }
  neighbors_[layer][node] = std::move(neighbors);
  return Status::Ok();
}

Status HnswGraph::set_entry_point(uint32_t node) {
  if (node_count() == 0) {
    if (node != kNoEntry) {
      return Status::InvalidArgument("hnsw: empty graph has no entry");
    }
    entry_point_ = kNoEntry;
    return Status::Ok();
  }
  if (node >= node_count()) {
    return Status::InvalidArgument("hnsw: entry point out of range");
  }
  entry_point_ = node;
  return Status::Ok();
}

bool HnswGraph::operator==(const HnswGraph& other) const {
  return params_.m == other.params_.m &&
         params_.ef_construction == other.params_.ef_construction &&
         params_.ef_search_default == other.params_.ef_search_default &&
         params_.max_layers == other.params_.max_layers &&
         segment_id_ == other.segment_id_ &&
         entry_point_ == other.entry_point_ &&
         max_level_ == other.max_level_ && levels_ == other.levels_ &&
         row_ordinals_ == other.row_ordinals_ &&
         neighbors_ == other.neighbors_;
}

std::string HnswGraph::Serialize() const {
  std::string body;
  body.reserve(static_cast<size_t>(node_count()) * 16u);

  for (uint32_t i = 0; i < node_count(); ++i) {
    AppendU32(body, row_ordinals_[i]);
    AppendU8(body, levels_[i]);
    AppendU8(body, 0);
    AppendU8(body, 0);
    AppendU8(body, 0);
  }

  for (uint16_t layer = 0; layer <= max_level_; ++layer) {
    for (uint32_t node = 0; node < node_count(); ++node) {
      if (levels_[node] < layer) continue;
      const auto& nbs = neighbors_[layer][node];
      AppendU16(body, static_cast<uint16_t>(nbs.size()));
      for (uint32_t nb : nbs) AppendU32(body, nb);
    }
  }

  std::string out;
  out.reserve(static_cast<size_t>(kHeaderBytes) + body.size() + kFooterBytes);

  // Fixed 64-byte header (CRC covers [0, 60)).
  AppendU32(out, kMagic);
  AppendU16(out, kFormatVersion);
  AppendU16(out, kHeaderBytes);
  AppendU32(out, 0);  // flags
  AppendU32(out, params_.m);
  AppendU32(out, HnswLayer0MaxDegree(params_));
  AppendU32(out, params_.ef_construction);
  AppendU32(out, params_.ef_search_default);
  AppendU32(out, params_.max_layers);
  AppendU32(out, entry_point_);
  AppendU32(out, node_count());
  AppendU16(out, max_level_);
  AppendU16(out, 0);  // reserved0
  AppendU64(out, segment_id_);
  AppendU32(out, static_cast<uint32_t>(body.size()));  // payload_bytes
  AppendU32(out, 0);                                   // reserved1
  if (out.size() != 60) {
    // Defensive: keep on-disk header self-consistent even if fields drift.
    out.resize(60, '\0');
  }
  AppendU32(out, Crc32Local(out.data(), 60));

  out.append(body);

  AppendU32(out, Crc32Local(body.data(), body.size()));
  AppendU32(out, kFooterMagic);
  AppendU16(out, kFormatVersion);
  AppendU16(out, 0);
  return out;
}

Status HnswGraph::WriteToFile(const std::string& path) const {
  const std::string bytes = Serialize();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return Status::IoError("hnsw: open for write failed");
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out) return Status::IoError("hnsw: write failed");
  return Status::Ok();
}

Result<HnswGraph> HnswGraph::Load(std::string_view bytes) {
  if (bytes.size() < static_cast<size_t>(kHeaderBytes) + kFooterBytes) {
    return Status::Corruption("hnsw: file too small");
  }

  size_t off = 0;
  const uint32_t magic = ReadU32(bytes, off);
  if (magic != kMagic) return Status::Corruption("hnsw: bad magic");
  const uint16_t version = ReadU16(bytes, off);
  if (version != kFormatVersion) {
    return Status::Corruption("hnsw: unsupported version");
  }
  const uint16_t header_bytes = ReadU16(bytes, off);
  if (header_bytes != kHeaderBytes) {
    return Status::Corruption("hnsw: bad header_bytes");
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
  const uint64_t segment_id = ReadU64(bytes, off);
  const uint32_t payload_bytes = ReadU32(bytes, off);
  ReadU32(bytes, off);  // reserved1
  const uint32_t header_crc = ReadU32(bytes, off);

  if (off != kHeaderBytes) {
    return Status::Corruption("hnsw: header size mismatch");
  }
  if (Crc32Local(bytes.data(), 60) != header_crc) {
    return Status::Corruption("hnsw: header crc mismatch");
  }
  if (params.m == 0 || m0 != HnswLayer0MaxDegree(params)) {
    return Status::Corruption("hnsw: invalid m/m0");
  }
  if (params.max_layers == 0 || params.max_layers > 255) {
    return Status::Corruption("hnsw: invalid max_layers");
  }
  if (node_count > kMaxNodes) {
    return Status::Corruption("hnsw: node_count too large");
  }
  if (max_level >= params.max_layers) {
    return Status::Corruption("hnsw: max_level exceeds max_layers");
  }
  if (node_count == 0) {
    if (entry != kNoEntry || max_level != 0) {
      return Status::Corruption("hnsw: empty graph invariants");
    }
  } else if (entry >= node_count) {
    return Status::Corruption("hnsw: entry point out of range");
  }

  const size_t expect_total =
      static_cast<size_t>(kHeaderBytes) + payload_bytes + kFooterBytes;
  if (bytes.size() != expect_total) {
    return Status::Corruption("hnsw: length check failed");
  }

  const std::string_view body = bytes.substr(kHeaderBytes, payload_bytes);
  size_t foff = kHeaderBytes + payload_bytes;
  const uint32_t body_crc = ReadU32(bytes, foff);
  const uint32_t footer_magic = ReadU32(bytes, foff);
  const uint16_t footer_ver = ReadU16(bytes, foff);
  ReadU16(bytes, foff);  // reserved
  if (footer_magic != kFooterMagic || footer_ver != kFormatVersion) {
    return Status::Corruption("hnsw: bad footer");
  }
  if (Crc32Local(body.data(), body.size()) != body_crc) {
    return Status::Corruption("hnsw: body crc mismatch");
  }

  HnswGraph g(params);
  g.segment_id_ = segment_id;
  g.entry_point_ = entry;
  g.max_level_ = node_count == 0 ? 0 : max_level;
  g.levels_.resize(node_count);
  g.row_ordinals_.resize(node_count);
  if (node_count > 0) {
    g.neighbors_.assign(static_cast<size_t>(g.max_level_) + 1,
                        std::vector<std::vector<uint32_t>>(node_count));
  }

  size_t bo = 0;
  for (uint32_t i = 0; i < node_count; ++i) {
    if (auto st = Need(body, bo, 8); !st.ok()) return st;
    g.row_ordinals_[i] = ReadU32(body, bo);
    g.levels_[i] = ReadU8(body, bo);
    ReadU8(body, bo);
    ReadU8(body, bo);
    ReadU8(body, bo);
    if (g.levels_[i] > g.max_level_) {
      return Status::Corruption("hnsw: node level > max_level");
    }
  }

  for (uint16_t layer = 0; layer <= g.max_level_; ++layer) {
    for (uint32_t node = 0; node < node_count; ++node) {
      if (g.levels_[node] < layer) continue;
      if (auto st = Need(body, bo, 2); !st.ok()) return st;
      const uint16_t degree = ReadU16(body, bo);
      if (degree > g.MaxDegreeForLayer(layer)) {
        return Status::Corruption("hnsw: degree exceeds max");
      }
      if (auto st = Need(body, bo, static_cast<size_t>(degree) * 4); !st.ok()) {
        return st;
      }
      auto& nbs = g.neighbors_[layer][node];
      nbs.resize(degree);
      for (uint16_t d = 0; d < degree; ++d) {
        const uint32_t nb = ReadU32(body, bo);
        if (nb >= node_count || nb == node) {
          return Status::Corruption("hnsw: bad neighbor");
        }
        if (g.levels_[nb] < layer) {
          return Status::Corruption("hnsw: neighbor missing on layer");
        }
        nbs[d] = nb;
      }
    }
  }

  if (bo != body.size()) {
    return Status::Corruption("hnsw: trailing body bytes");
  }

  // Entry should sit on the top layer when the graph is non-empty.
  if (node_count > 0 && g.NodeLevel(entry) != g.max_level_) {
    return Status::Corruption("hnsw: entry not on max_level");
  }

  return g;
}

Result<HnswGraph> HnswGraph::LoadFromFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Status::IoError("hnsw: open for read failed");
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  if (!in && !in.eof()) return Status::IoError("hnsw: read failed");
  return Load(bytes);
}

}  // namespace aster

#else  // !ASTER_ENABLE_HNSW

namespace aster {
namespace {
// Non-empty TU under Tiny so the object file still links cleanly.
[[maybe_unused]] constexpr int kHnswDisabledStub = 0;
}  // namespace
}  // namespace aster

#endif  // ASTER_ENABLE_HNSW
