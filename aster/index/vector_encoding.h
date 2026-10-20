#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"

namespace aster {

// On-disk / in-memory vector payload encodings (SSTable header byte
// `vector_encoding`, docs/sstable-format.md §5 and docs/vector-encoding.md).
enum class VectorEncoding : uint8_t {
  kFloat32 = 0,
  kFloat16 = 1,
  kInt8Sq = 2,         // per-vector scalar int8 + f32 scale
  kBinaryRabitq = 3,   // 1-bit RaBitQ-style codes + f32 scale
};

const char* VectorEncodingName(VectorEncoding enc);
bool IsKnownVectorEncoding(uint8_t raw);

// Bytes of packed payload for `count` vectors of dimension `dim`
// (excludes SSTable framing / compression).
uint64_t PackedPayloadBytes(VectorEncoding enc, uint32_t dim, uint64_t count);

// Bytes for a single vector's codes (excluding shared headers).
uint64_t BytesPerVector(VectorEncoding enc, uint32_t dim);

// Encode one vector into `out` (must have capacity BytesPerVector).
// Returns bytes written.
Result<size_t> EncodeVector(VectorEncoding enc, VectorView src,
                            std::span<uint8_t> out);

// Decode one packed vector into `out` (size == dim).
Status DecodeVector(VectorEncoding enc, uint32_t dim,
                    std::span<const uint8_t> packed, std::span<float> out);

// Contiguous packed store for N vectors — the RAM-side counterpart of the
// SSTable vector block. Used by sealed segments to avoid holding float32.
class CompactVectorStore {
 public:
  CompactVectorStore() = default;

  static Result<CompactVectorStore> Encode(VectorEncoding enc, uint32_t dim,
                                           const float* data, size_t count);
  static Result<CompactVectorStore> Encode(VectorEncoding enc, uint32_t dim,
                                           const std::vector<Row>& rows);

  // Deserialize a previously serialized payload (no framing).
  static Result<CompactVectorStore> FromPayload(VectorEncoding enc,
                                                uint32_t dim, uint64_t count,
                                                std::vector<uint8_t> payload);

  VectorEncoding encoding() const { return encoding_; }
  uint32_t dimension() const { return dim_; }
  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }

  // Exact packed payload size (what goes after SSTable pad).
  size_t BytesUsed() const { return payload_.size(); }
  const std::vector<uint8_t>& payload() const { return payload_; }

  Status Decode(size_t index, std::span<float> out) const;
  Result<std::vector<float>> Decode(size_t index) const;

  // Expand all vectors to a dense float32 matrix (row-major). Expensive;
  // used for HNSW build and tests.
  Result<std::vector<float>> DecodeAll() const;

  // Span covering codes for vector `index`.
  std::span<const uint8_t> PackedAt(size_t index) const;

 private:
  VectorEncoding encoding_ = VectorEncoding::kFloat32;
  uint32_t dim_ = 0;
  size_t count_ = 0;
  size_t stride_ = 0;  // bytes per vector in payload_
  std::vector<uint8_t> payload_;
};

// IEEE-754 binary16 helpers (public for tests).
uint16_t Float32ToFloat16Bits(float value);
float Float16BitsToFloat32(uint16_t bits);

}  // namespace aster
