#include "aster/index/vector_encoding.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "aster/index/distance.h"

namespace aster {
namespace {

constexpr uint32_t kBinaryRabitqSeed = 0xA57E9u;

void WriteF32LE(uint8_t* out, float v) {
  uint32_t u = 0;
  static_assert(sizeof(float) == 4, "float must be 32-bit");
  std::memcpy(&u, &v, sizeof(u));
  out[0] = static_cast<uint8_t>(u & 0xff);
  out[1] = static_cast<uint8_t>((u >> 8) & 0xff);
  out[2] = static_cast<uint8_t>((u >> 16) & 0xff);
  out[3] = static_cast<uint8_t>((u >> 24) & 0xff);
}

float ReadF32LE(const uint8_t* p) {
  uint32_t u = static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
  float v;
  std::memcpy(&v, &u, sizeof(v));
  return v;
}

uint16_t ReadU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

// Deterministic ±1 from (seed, index) — cheap JL-style preconditioner for
// binary RaBitQ (docs/vector-encoding.md).
inline float RabitqSign(uint32_t index) {
  uint32_t x = index * 0x9E3779B9u + kBinaryRabitqSeed;
  x ^= x >> 16;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  return (x & 1u) ? 1.0f : -1.0f;
}

size_t StrideFor(VectorEncoding enc, uint32_t dim) {
  return static_cast<size_t>(BytesPerVector(enc, dim));
}

Status EncodeOne(VectorEncoding enc, VectorView src, uint8_t* out) {
  const uint32_t dim = static_cast<uint32_t>(src.size());
  switch (enc) {
    case VectorEncoding::kFloat32: {
      for (uint32_t i = 0; i < dim; ++i) {
        WriteF32LE(out + 4 * i, src[i]);
      }
      return Status::Ok();
    }
    case VectorEncoding::kFloat16: {
      for (uint32_t i = 0; i < dim; ++i) {
        const uint16_t bits = Float32ToFloat16Bits(src[i]);
        out[2 * i + 0] = static_cast<uint8_t>(bits & 0xff);
        out[2 * i + 1] = static_cast<uint8_t>((bits >> 8) & 0xff);
      }
      return Status::Ok();
    }
    case VectorEncoding::kInt8Sq: {
      float max_abs = 0.0f;
      for (uint32_t i = 0; i < dim; ++i) {
        max_abs = std::max(max_abs, std::fabs(src[i]));
      }
      const float scale =
          max_abs > 0.0f ? (max_abs / 127.0f) : 0.0f;
      WriteF32LE(out, scale);
      for (uint32_t i = 0; i < dim; ++i) {
        int32_t q = 0;
        if (scale > 0.0f) {
          q = static_cast<int32_t>(std::lround(src[i] / scale));
          q = std::clamp(q, -127, 127);
        }
        out[4 + i] = static_cast<uint8_t>(static_cast<int8_t>(q));
      }
      return Status::Ok();
    }
    case VectorEncoding::kBinaryRabitq: {
      double energy = 0.0;
      for (uint32_t i = 0; i < dim; ++i) {
        const float rx = src[i] * RabitqSign(i);
        energy += static_cast<double>(rx) * static_cast<double>(rx);
      }
      const float scale =
          dim > 0 ? static_cast<float>(std::sqrt(energy / dim)) : 0.0f;
      WriteF32LE(out, scale);
      const uint32_t nbytes = (dim + 7) / 8;
      std::memset(out + 4, 0, nbytes);
      for (uint32_t i = 0; i < dim; ++i) {
        const float rx = src[i] * RabitqSign(i);
        if (rx >= 0.0f) {
          out[4 + (i / 8)] =
              static_cast<uint8_t>(out[4 + (i / 8)] | (1u << (i % 8)));
        }
      }
      return Status::Ok();
    }
  }
  return Status::InvalidArgument("unknown vector encoding");
}

Status DecodeOne(VectorEncoding enc, uint32_t dim, const uint8_t* packed,
                 float* out) {
  switch (enc) {
    case VectorEncoding::kFloat32: {
      for (uint32_t i = 0; i < dim; ++i) {
        out[i] = ReadF32LE(packed + 4 * i);
      }
      return Status::Ok();
    }
    case VectorEncoding::kFloat16: {
      for (uint32_t i = 0; i < dim; ++i) {
        out[i] = Float16BitsToFloat32(ReadU16LE(packed + 2 * i));
      }
      return Status::Ok();
    }
    case VectorEncoding::kInt8Sq: {
      const float scale = ReadF32LE(packed);
      for (uint32_t i = 0; i < dim; ++i) {
        const int8_t code = static_cast<int8_t>(packed[4 + i]);
        out[i] = static_cast<float>(code) * scale;
      }
      return Status::Ok();
    }
    case VectorEncoding::kBinaryRabitq: {
      const float scale = ReadF32LE(packed);
      for (uint32_t i = 0; i < dim; ++i) {
        const bool bit = (packed[4 + (i / 8)] >> (i % 8)) & 1u;
        const float s = bit ? 1.0f : -1.0f;
        out[i] = s * scale * RabitqSign(i);
      }
      return Status::Ok();
    }
  }
  return Status::InvalidArgument("unknown vector encoding");
}

}  // namespace

const char* VectorEncodingName(VectorEncoding enc) {
  switch (enc) {
    case VectorEncoding::kFloat32:
      return "float32";
    case VectorEncoding::kFloat16:
      return "float16";
    case VectorEncoding::kInt8Sq:
      return "int8_sq";
    case VectorEncoding::kBinaryRabitq:
      return "binary_rabitq";
  }
  return "unknown";
}

bool IsKnownVectorEncoding(uint8_t raw) {
  return raw <= static_cast<uint8_t>(VectorEncoding::kBinaryRabitq);
}

uint64_t BytesPerVector(VectorEncoding enc, uint32_t dim) {
  switch (enc) {
    case VectorEncoding::kFloat32:
      return static_cast<uint64_t>(dim) * 4u;
    case VectorEncoding::kFloat16:
      return static_cast<uint64_t>(dim) * 2u;
    case VectorEncoding::kInt8Sq:
      return static_cast<uint64_t>(dim) + 4u;
    case VectorEncoding::kBinaryRabitq:
      return static_cast<uint64_t>((dim + 7u) / 8u) + 4u;
  }
  return 0;
}

uint64_t PackedPayloadBytes(VectorEncoding enc, uint32_t dim, uint64_t count) {
  return BytesPerVector(enc, dim) * count;
}

uint16_t Float32ToFloat16Bits(float value) {
  uint32_t f = 0;
  std::memcpy(&f, &value, sizeof(f));
  const uint32_t sign = (f >> 16) & 0x8000u;
  int32_t exp = static_cast<int32_t>((f >> 23) & 0xff) - 127 + 15;
  uint32_t mant = f & 0x7fffffu;

  if ((f & 0x7fffffffu) == 0) return static_cast<uint16_t>(sign);
  // NaN / Inf
  if (((f >> 23) & 0xff) == 0xff) {
    return static_cast<uint16_t>(sign | 0x7c00u |
                                 (mant ? 0x200u : 0));
  }
  if (exp <= 0) {
    if (exp < -10) return static_cast<uint16_t>(sign);
    mant |= 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(1 - exp);
    uint32_t half = mant >> (shift + 13);
    if ((mant >> (shift + 12)) & 1u) ++half;
    return static_cast<uint16_t>(sign | half);
  }
  if (exp >= 31) {
    return static_cast<uint16_t>(sign | 0x7c00u);  // overflow → Inf
  }
  uint16_t half = static_cast<uint16_t>(sign | (exp << 10) | (mant >> 13));
  if (mant & 0x1000u) {
    // Round to nearest even-ish: bump mantissa.
    half = static_cast<uint16_t>(half + 1);
  }
  return half;
}

float Float16BitsToFloat32(uint16_t bits) {
  const uint32_t sign = static_cast<uint32_t>(bits & 0x8000u) << 16;
  uint32_t exp = (bits >> 10) & 0x1fu;
  uint32_t mant = bits & 0x3ffu;
  uint32_t f;
  if (exp == 0) {
    if (mant == 0) {
      f = sign;
    } else {
      exp = 1;
      while ((mant & 0x400u) == 0) {
        mant <<= 1;
        --exp;
      }
      mant &= 0x3ffu;
      f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
  } else if (exp == 31) {
    f = sign | 0x7f800000u | (mant << 13);
  } else {
    f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &f, sizeof(out));
  return out;
}

Result<size_t> EncodeVector(VectorEncoding enc, VectorView src,
                            std::span<uint8_t> out) {
  if (!IsKnownVectorEncoding(static_cast<uint8_t>(enc))) {
    return Status::InvalidArgument("unknown vector encoding");
  }
  const size_t need = StrideFor(enc, static_cast<uint32_t>(src.size()));
  if (out.size() < need) {
    return Status::InvalidArgument("encode buffer too small");
  }
  if (auto st = EncodeOne(enc, src, out.data()); !st.ok()) return st;
  return need;
}

Status DecodeVector(VectorEncoding enc, uint32_t dim,
                    std::span<const uint8_t> packed, std::span<float> out) {
  if (!IsKnownVectorEncoding(static_cast<uint8_t>(enc))) {
    return Status::InvalidArgument("unknown vector encoding");
  }
  if (out.size() < dim) {
    return Status::InvalidArgument("decode buffer too small");
  }
  const size_t need = StrideFor(enc, dim);
  if (packed.size() < need) {
    return Status::Corruption("packed vector truncated");
  }
  return DecodeOne(enc, dim, packed.data(), out.data());
}

Result<CompactVectorStore> CompactVectorStore::Encode(VectorEncoding enc,
                                                      uint32_t dim,
                                                      const float* data,
                                                      size_t count) {
  if (!IsKnownVectorEncoding(static_cast<uint8_t>(enc))) {
    return Status::InvalidArgument("unknown vector encoding");
  }
  if (dim == 0 && count > 0) {
    return Status::InvalidArgument("dimension must be > 0");
  }
  CompactVectorStore store;
  store.encoding_ = enc;
  store.dim_ = dim;
  store.count_ = count;
  store.stride_ = StrideFor(enc, dim);
  store.owned_.assign(store.stride_ * count, 0);
  store.payload_ptr_ = store.owned_.data();
  store.payload_size_ = store.owned_.size();
  for (size_t i = 0; i < count; ++i) {
    VectorView view(data + i * dim, dim);
    if (auto st =
            EncodeOne(enc, view, store.owned_.data() + i * store.stride_);
        !st.ok()) {
      return st;
    }
  }
  return store;
}

Result<CompactVectorStore> CompactVectorStore::Encode(
    VectorEncoding enc, uint32_t dim, const std::vector<Row>& rows) {
  size_t live = 0;
  for (const Row& row : rows) {
    if (!row.tombstone) ++live;
  }
  std::vector<float> dense(live * dim);
  size_t w = 0;
  for (const Row& row : rows) {
    if (row.tombstone) continue;
    for (uint32_t d = 0; d < dim; ++d) {
      dense[w * dim + d] = d < row.vector.size() ? row.vector[d] : 0.0f;
    }
    ++w;
  }
  return Encode(enc, dim, dense.data(), live);
}

Result<CompactVectorStore> CompactVectorStore::FromPayload(
    VectorEncoding enc, uint32_t dim, uint64_t count,
    std::vector<uint8_t> payload) {
  if (!IsKnownVectorEncoding(static_cast<uint8_t>(enc))) {
    return Status::InvalidArgument("unknown vector encoding");
  }
  const size_t stride = StrideFor(enc, dim);
  const uint64_t need = stride * count;
  if (payload.size() != need) {
    return Status::Corruption("compact vector payload size mismatch");
  }
  CompactVectorStore store;
  store.encoding_ = enc;
  store.dim_ = dim;
  store.count_ = static_cast<size_t>(count);
  store.stride_ = stride;
  store.owned_ = std::move(payload);
  store.payload_ptr_ = store.owned_.data();
  store.payload_size_ = store.owned_.size();
  return store;
}

Result<CompactVectorStore> CompactVectorStore::FromMappedPayload(
    VectorEncoding enc, uint32_t dim, uint64_t count,
    std::span<const uint8_t> payload, std::shared_ptr<void> keep_alive) {
  if (!IsKnownVectorEncoding(static_cast<uint8_t>(enc))) {
    return Status::InvalidArgument("unknown vector encoding");
  }
  const size_t stride = StrideFor(enc, dim);
  const uint64_t need = stride * count;
  if (payload.size() != need) {
    return Status::Corruption("compact vector payload size mismatch");
  }
  if (need > 0 && (payload.data() == nullptr || keep_alive == nullptr)) {
    return Status::InvalidArgument("mapped payload requires keep_alive");
  }
  CompactVectorStore store;
  store.encoding_ = enc;
  store.dim_ = dim;
  store.count_ = static_cast<size_t>(count);
  store.stride_ = stride;
  store.payload_ptr_ = payload.data();
  store.payload_size_ = payload.size();
  store.keep_alive_ = std::move(keep_alive);
  return store;
}

Result<CompactVectorStore> CompactVectorStore::PinToHeap() const {
  if (!is_file_backed()) {
    // Already heap-owned — return a deep copy so callers can detach mapping.
    return FromPayload(encoding_, dim_, count_,
                       std::vector<uint8_t>(payload_ptr_,
                                            payload_ptr_ + payload_size_));
  }
  std::vector<uint8_t> copy(payload_ptr_, payload_ptr_ + payload_size_);
  return FromPayload(encoding_, dim_, count_, std::move(copy));
}

std::span<const uint8_t> CompactVectorStore::PackedAt(size_t index) const {
  if (index >= count_ || payload_ptr_ == nullptr) return {};
  return std::span<const uint8_t>(payload_ptr_ + index * stride_, stride_);
}

float ScorePacked(Metric metric, VectorEncoding enc, uint32_t dim,
                  VectorView query, std::span<const uint8_t> packed) {
  if (query.size() != dim || packed.size() < StrideFor(enc, dim)) {
    return 0.0f;
  }
  switch (enc) {
    case VectorEncoding::kInt8Sq: {
      const float scale = ReadF32LE(packed.data());
      const int8_t* codes =
          reinterpret_cast<const int8_t*>(packed.data() + 4);
      if (metric == Metric::kL2) {
        double sum = 0.0;
        for (uint32_t i = 0; i < dim; ++i) {
          const double d =
              static_cast<double>(query[i]) -
              static_cast<double>(codes[i]) * static_cast<double>(scale);
          sum += d * d;
        }
        return static_cast<float>(-sum);
      }
      // Dot / Cosine: reconstruct via scale * code (same as decode path for
      // cosine energy); keep cosine via decode for norm fidelity.
      if (metric == Metric::kDot) {
        double sum = 0.0;
        for (uint32_t i = 0; i < dim; ++i) {
          sum += static_cast<double>(query[i]) *
                 (static_cast<double>(codes[i]) * static_cast<double>(scale));
        }
        return static_cast<float>(sum);
      }
      break;
    }
    case VectorEncoding::kBinaryRabitq: {
      if (metric != Metric::kDot && metric != Metric::kCosine) break;
      const float scale = ReadF32LE(packed.data());
      double sum = 0.0;
      for (uint32_t i = 0; i < dim; ++i) {
        const bool bit = (packed[4 + (i / 8)] >> (i % 8)) & 1u;
        const float s = bit ? 1.0f : -1.0f;
        sum += static_cast<double>(query[i]) *
               static_cast<double>(s * scale * RabitqSign(i));
      }
      if (metric == Metric::kDot) return static_cast<float>(sum);
      // Cosine ≈ dot / (|q| * scale*sqrt(D)); approximate with decode norms.
      break;
    }
    case VectorEncoding::kFloat32: {
      thread_local std::vector<float> scratch;
      scratch.resize(dim);
      if (!DecodeOne(enc, dim, packed.data(), scratch.data()).ok()) return 0.0f;
      return Score(metric, query, scratch);
    }
    case VectorEncoding::kFloat16:
      break;
  }
  thread_local std::vector<float> scratch;
  scratch.resize(dim);
  if (!DecodeOne(enc, dim, packed.data(), scratch.data()).ok()) return 0.0f;
  return Score(metric, query, scratch);
}

Status CompactVectorStore::Decode(size_t index, std::span<float> out) const {
  if (index >= count_) return Status::InvalidArgument("vector index OOB");
  return DecodeVector(encoding_, dim_, PackedAt(index), out);
}

Result<std::vector<float>> CompactVectorStore::Decode(size_t index) const {
  std::vector<float> out(dim_);
  if (auto st = Decode(index, out); !st.ok()) return st;
  return out;
}

Result<std::vector<float>> CompactVectorStore::DecodeAll() const {
  std::vector<float> out(count_ * dim_);
  for (size_t i = 0; i < count_; ++i) {
    if (auto st = Decode(i, std::span<float>(out.data() + i * dim_, dim_));
        !st.ok()) {
      return st;
    }
  }
  return out;
}

}  // namespace aster
