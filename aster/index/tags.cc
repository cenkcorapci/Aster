#include "aster/index/tags.h"

#include <algorithm>

namespace aster {
namespace {

void AppendU16(std::string& out, uint16_t v) {
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void AppendU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
}

uint16_t ReadU16(std::string_view b, size_t& off) {
  const uint16_t v = static_cast<uint8_t>(b[off]) |
                     (static_cast<uint16_t>(static_cast<uint8_t>(b[off + 1]))
                      << 8);
  off += 2;
  return v;
}

uint32_t ReadU32(std::string_view b, size_t& off) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(static_cast<uint8_t>(b[off++])) << (8 * i);
  }
  return v;
}

}  // namespace

void TagBitmap::Add(uint32_t ordinal) {
  auto it = std::lower_bound(ordinals_.begin(), ordinals_.end(), ordinal);
  if (it == ordinals_.end() || *it != ordinal) {
    ordinals_.insert(it, ordinal);
  }
}

bool TagBitmap::Contains(uint32_t ordinal) const {
  return std::binary_search(ordinals_.begin(), ordinals_.end(), ordinal);
}

void TagBitmap::AndInPlace(const TagBitmap& other) {
  std::vector<uint32_t> out;
  out.reserve(std::min(ordinals_.size(), other.ordinals_.size()));
  std::set_intersection(ordinals_.begin(), ordinals_.end(),
                        other.ordinals_.begin(), other.ordinals_.end(),
                        std::back_inserter(out));
  ordinals_ = std::move(out);
}

TagBitmap TagBitmap::And(const TagBitmap& a, const TagBitmap& b) {
  TagBitmap out = a;
  out.AndInPlace(b);
  return out;
}

std::string TagBitmap::SerializeDense(uint32_t universe_size) const {
  const uint32_t nbytes = (universe_size + 7u) / 8u;
  std::string bits(nbytes, '\0');
  for (uint32_t o : ordinals_) {
    if (o >= universe_size) continue;
    bits[o / 8] = static_cast<char>(static_cast<uint8_t>(bits[o / 8]) |
                                    (1u << (o % 8)));
  }
  return bits;
}

TagBitmap TagBitmap::DeserializeDense(std::string_view blob,
                                      uint32_t universe_size) {
  TagBitmap bm;
  const uint32_t limit =
      std::min(universe_size, static_cast<uint32_t>(blob.size() * 8));
  for (uint32_t i = 0; i < limit; ++i) {
    const size_t byte_i = i / 8;
    if ((static_cast<uint8_t>(blob[byte_i]) & (1u << (i % 8))) != 0) {
      bm.ordinals_.push_back(i);
    }
  }
  return bm;
}

TagIndex TagIndex::Build(const std::vector<Row>& rows) {
  TagIndex idx;
  idx.row_count_ = static_cast<uint32_t>(rows.size());
  for (uint32_t i = 0; i < rows.size(); ++i) {
    for (const auto& tag : rows[i].tags) {
      idx.tags_[tag].Add(i);
    }
  }
  return idx;
}

const TagBitmap* TagIndex::Find(std::string_view tag) const {
  auto it = tags_.find(std::string(tag));
  if (it == tags_.end()) return nullptr;
  return &it->second;
}

TagBitmap TagIndex::Matching(const std::set<std::string>& wanted) const {
  if (wanted.empty()) {
    TagBitmap all;
    for (uint32_t i = 0; i < row_count_; ++i) all.Add(i);
    return all;
  }
  TagBitmap result;
  bool first = true;
  for (const auto& tag : wanted) {
    const TagBitmap* bm = Find(tag);
    if (bm == nullptr) return TagBitmap{};
    if (first) {
      result = *bm;
      first = false;
    } else {
      result.AndInPlace(*bm);
    }
    if (result.empty()) return result;
  }
  return result;
}

double TagIndex::Selectivity(const std::set<std::string>& wanted) const {
  if (row_count_ == 0 || wanted.empty()) return 1.0;
  return static_cast<double>(MatchCount(wanted)) /
         static_cast<double>(row_count_);
}

std::string TagIndex::SerializePayload() const {
  if (tags_.empty()) return {};
  std::string payload;
  AppendU32(payload, static_cast<uint32_t>(tags_.size()));
  for (const auto& [tag, bm] : tags_) {
    AppendU16(payload, static_cast<uint16_t>(tag.size()));
    payload.append(tag);
    std::string bits = bm.SerializeDense(row_count_);
    AppendU32(payload, static_cast<uint32_t>(bits.size()));
    payload.append(bits);
  }
  return payload;
}

TagIndex TagIndex::ParsePayload(std::string_view payload,
                                uint32_t row_count) {
  TagIndex idx;
  idx.row_count_ = row_count;
  if (payload.empty()) return idx;
  size_t off = 0;
  if (off + 4 > payload.size()) return idx;
  const uint32_t tag_count = ReadU32(payload, off);
  for (uint32_t t = 0; t < tag_count; ++t) {
    if (off + 2 > payload.size()) break;
    const uint16_t tag_len = ReadU16(payload, off);
    if (off + tag_len + 4 > payload.size()) break;
    const std::string tag(payload.substr(off, tag_len));
    off += tag_len;
    const uint32_t blob_len = ReadU32(payload, off);
    if (off + blob_len > payload.size()) break;
    idx.tags_[tag] =
        TagBitmap::DeserializeDense(payload.substr(off, blob_len), row_count);
    off += blob_len;
  }
  return idx;
}

}  // namespace aster
