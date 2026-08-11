#include "aster/storage/sstable.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "aster/core/features.h"
#include "aster/index/tags.h"
#include "aster/storage/wal.h"  // Crc32

#if ASTER_ENABLE_COMPRESSION
#include <lz4.h>
#include <zstd.h>
#endif

namespace aster {
namespace {

constexpr uint32_t kMagic = 0x41535453u;       // ASTS
constexpr uint32_t kFooterMagic = 0x41535446u;  // ASTF
constexpr uint16_t kFormatVersion = 1;
constexpr size_t kHeaderBytes = 128;
constexpr size_t kDirectoryBytes = 112;  // 7 * 16
constexpr size_t kPreludeBytes = kHeaderBytes + kDirectoryBytes;  // 240
constexpr size_t kFooterBytes = 40;
constexpr size_t kNumBlocks = 7;
constexpr uint32_t kNoVector = 0xFFFFFFFFu;
constexpr uint32_t kFlagHasTags = 1u << 1;
constexpr uint64_t kMaxRowsPerSstable = 50'000'000ull;
constexpr uint32_t kMaxDimension = 16384u;
constexpr uint64_t kMaxSstableFileBytes = 4ull << 30;  // 4 GiB


struct BlockDesc {
  uint64_t offset = 0;
  uint32_t on_disk_len = 0;
  uint8_t compression = 0;
};

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
void AppendF32(std::string& b, float f) {
  uint32_t u;
  static_assert(sizeof(float) == 4);
  std::memcpy(&u, &f, 4);
  AppendU32(b, u);
}

uint8_t ReadU8(const std::string& b, size_t& off) {
  return static_cast<uint8_t>(b[off++]);
}
uint16_t ReadU16(const std::string& b, size_t& off) {
  const uint16_t v = static_cast<uint16_t>(ReadU8(b, off)) |
                     (static_cast<uint16_t>(ReadU8(b, off)) << 8);
  return v;
}
uint32_t ReadU32(const std::string& b, size_t& off) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(ReadU8(b, off)) << (8 * i);
  return v;
}
uint64_t ReadU64(const std::string& b, size_t& off) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(ReadU8(b, off)) << (8 * i);
  return v;
}
float ReadF32(const std::string& b, size_t& off) {
  const uint32_t u = ReadU32(b, off);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

size_t Align8(size_t n) { return (n + 7) & ~size_t{7}; }

// Uncompressed image: u32 len + payload.
std::string WrapBlock(const std::string& payload) {
  std::string out;
  AppendU32(out, static_cast<uint32_t>(payload.size()));
  out.append(payload);
  return out;
}

Result<std::string> CompressBlockImage(const std::string& uncompressed,
                                       CompressionCodec codec) {
  if (codec == CompressionCodec::kNone) return uncompressed;
#if !ASTER_ENABLE_COMPRESSION
  return Status::InvalidArgument("compression disabled in this build");
#else
  if (codec == CompressionCodec::kLz4) {
    const int src_size = static_cast<int>(uncompressed.size());
    const int bound = LZ4_compressBound(src_size);
    if (bound <= 0) {
      return Status(StatusCode::kInternal, "lz4 compressBound failed");
    }
    std::string out(static_cast<size_t>(bound), '\0');
    const int n = LZ4_compress_default(uncompressed.data(), out.data(),
                                       src_size, bound);
    if (n <= 0) {
      return Status(StatusCode::kInternal, "lz4 compress failed");
    }
    out.resize(static_cast<size_t>(n));
    return out;
  }
  if (codec == CompressionCodec::kZstd) {
    const size_t bound = ZSTD_compressBound(uncompressed.size());
    std::string out(bound, '\0');
    const size_t n =
        ZSTD_compress(out.data(), bound, uncompressed.data(),
                      uncompressed.size(), /*level=*/1);
    if (ZSTD_isError(n)) {
      return Status(StatusCode::kInternal,
                    std::string("zstd compress: ") + ZSTD_getErrorName(n));
    }
    out.resize(n);
    return out;
  }
  return Status::InvalidArgument("unknown compression codec");
#endif
}

Result<std::string> DecompressBlockImage(const char* on_disk, size_t on_disk_len,
                                         CompressionCodec codec) {
  if (codec == CompressionCodec::kNone) {
    return std::string(on_disk, on_disk_len);
  }
#if !ASTER_ENABLE_COMPRESSION
  return Status::Corruption("compressed block requires ASTER_ENABLE_COMPRESSION");
#else
  if (codec == CompressionCodec::kLz4) {
    // Uncompressed image starts with u32 payload_len; total size = 4 + that.
    // LZ4_decompress_safe needs the exact dest capacity — peek after a
    // conservative upper bound: on_disk_len * 255 is LZ4's worst expansion,
    // but we know the image begins with a length prefix once decompressed.
    // Use LZ4_decompress_safe with a growing buffer seeded from a max of
    // 16x or at least 64 KiB, capped reasonably.
    size_t cap = std::max(on_disk_len * 8, size_t{65536});
    constexpr size_t kMaxUncompressed = 256ull << 20;  // 256 MiB
    if (cap > kMaxUncompressed) cap = kMaxUncompressed;
    for (int attempt = 0; attempt < 4; ++attempt) {
      std::string out(cap, '\0');
      const int n = LZ4_decompress_safe(on_disk, out.data(),
                                        static_cast<int>(on_disk_len),
                                        static_cast<int>(cap));
      if (n >= 0) {
        out.resize(static_cast<size_t>(n));
        return out;
      }
      if (cap >= kMaxUncompressed) break;
      cap = std::min(cap * 2, kMaxUncompressed);
    }
    return Status::Corruption("lz4 decompress failed");
  }
  if (codec == CompressionCodec::kZstd) {
    const unsigned long long frame_size =
        ZSTD_getFrameContentSize(on_disk, on_disk_len);
    size_t cap = 0;
    if (frame_size != ZSTD_CONTENTSIZE_ERROR &&
        frame_size != ZSTD_CONTENTSIZE_UNKNOWN &&
        frame_size < (256ull << 20)) {
      cap = static_cast<size_t>(frame_size);
    } else {
      cap = std::max(on_disk_len * 8, size_t{65536});
    }
    std::string out(cap, '\0');
    const size_t n =
        ZSTD_decompress(out.data(), cap, on_disk, on_disk_len);
    if (ZSTD_isError(n)) {
      return Status::Corruption(std::string("zstd decompress: ") +
                                ZSTD_getErrorName(n));
    }
    out.resize(n);
    return out;
  }
  return Status::Corruption("unknown compression codec");
#endif
}

std::string BuildBloomPayload(const BloomFilter& bloom) {
  return std::string(reinterpret_cast<const char*>(bloom.bits().data()),
                     bloom.bits().size());
}

std::string BuildSparsePayload(const std::vector<Row>& rows, uint32_t stride,
                               const std::vector<uint32_t>& id_record_offsets) {
  std::string payload;
  if (rows.empty()) return payload;
  auto emit = [&](uint32_t ordinal) {
    AppendU32(payload, ordinal);
    AppendU32(payload, id_record_offsets[ordinal]);
    const auto& id = rows[ordinal].id;
    AppendU16(payload, static_cast<uint16_t>(id.size()));
    payload.append(id);
  };
  emit(0);
  for (uint32_t i = stride; i < rows.size(); i += stride) emit(i);
  if ((rows.size() - 1) % stride != 0) {
    emit(static_cast<uint32_t>(rows.size() - 1));
  }
  return payload;
}

std::string BuildIdIndexPayload(const std::vector<Row>& rows,
                                std::vector<uint32_t>* record_offsets,
                                std::vector<uint32_t>* vector_slots) {
  std::string payload;
  record_offsets->clear();
  vector_slots->assign(rows.size(), kNoVector);
  uint32_t live_slot = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    record_offsets->push_back(static_cast<uint32_t>(payload.size()));
    const Row& row = rows[i];
    AppendU16(payload, static_cast<uint16_t>(row.id.size()));
    payload.append(row.id);
    const uint8_t flags = row.tombstone ? 0x01 : 0x00;
    AppendU8(payload, flags);
    AppendU8(payload, 0);
    AppendU64(payload, row.timestamp);
    AppendU64(payload, row.version);
    if (row.tombstone) {
      AppendU32(payload, kNoVector);
    } else {
      (*vector_slots)[i] = live_slot;
      AppendU32(payload, live_slot);
      ++live_slot;
    }
    // metadata filled in a second pass — placeholder zeros, patched below
    AppendU32(payload, 0);  // metadata_offset
    AppendU32(payload, 0);  // metadata_len
    AppendU32(payload, static_cast<uint32_t>(i));  // tag_row_ordinal
  }
  return payload;
}

void PatchIdMetadata(std::string& id_payload,
                     const std::vector<uint32_t>& record_offsets,
                     const std::vector<Row>& rows,
                     const std::vector<uint32_t>& meta_offsets,
                     const std::vector<uint32_t>& meta_lens) {
  for (size_t i = 0; i < rows.size(); ++i) {
    // record: u16 id_len + id + u8 flags + u8 reserved + u64 ts + u64 ver
    //         + u32 vector_slot + u32 meta_off + u32 meta_len + u32 ordinal
    size_t off = record_offsets[i];
    const uint16_t id_len =
        static_cast<uint16_t>(static_cast<uint8_t>(id_payload[off]) |
                              (static_cast<uint8_t>(id_payload[off + 1]) << 8));
    off += 2 + id_len + 1 + 1 + 8 + 8 + 4;  // up to metadata_offset
    const uint32_t mo = meta_offsets[i];
    const uint32_t ml = meta_lens[i];
    id_payload[off + 0] = static_cast<char>(mo & 0xff);
    id_payload[off + 1] = static_cast<char>((mo >> 8) & 0xff);
    id_payload[off + 2] = static_cast<char>((mo >> 16) & 0xff);
    id_payload[off + 3] = static_cast<char>((mo >> 24) & 0xff);
    id_payload[off + 4] = static_cast<char>(ml & 0xff);
    id_payload[off + 5] = static_cast<char>((ml >> 8) & 0xff);
    id_payload[off + 6] = static_cast<char>((ml >> 16) & 0xff);
    id_payload[off + 7] = static_cast<char>((ml >> 24) & 0xff);
  }
}

std::string BuildVectorPayload(const std::vector<Row>& rows, uint32_t dimension,
                               uint64_t live_count) {
  if (live_count == 0 || dimension == 0) return {};
  // pad so (8 + pad) % 32 == 0 relative to uncompressed image base.
  uint32_t pad = 0;
  while ((8 + pad) % 32 != 0) ++pad;
  std::string payload;
  AppendU32(payload, pad);
  payload.append(pad, '\0');
  for (const Row& row : rows) {
    if (row.tombstone) continue;
    for (uint32_t d = 0; d < dimension; ++d) {
      AppendF32(payload, d < row.vector.size() ? row.vector[d] : 0.0f);
    }
  }
  return payload;
}

// Tag bitmap block (§7.6). TagIndex serializes dense little-endian bitsets
// via the roaring-compatible API (aster/index/tags.h); portable CRoaring may
// replace the blob encoding later without changing this frame layout.
std::string BuildTagsPayload(const std::vector<Row>& rows) {
  return TagIndex::Build(rows).SerializePayload();
}

Status ParseTagsPayload(const std::string& payload, size_t row_count,
                        std::vector<std::set<std::string>>* out) {
  out->assign(row_count, {});
  if (payload.empty()) return Status::Ok();
  const TagIndex idx =
      TagIndex::ParsePayload(payload, static_cast<uint32_t>(row_count));
  for (const auto& [tag, bm] : idx.tags()) {
    for (uint32_t ord : bm.ordinals()) {
      if (ord < row_count) (*out)[ord].insert(tag);
    }
  }
  return Status::Ok();
}


std::string BuildMetadataPayload(const std::vector<Row>& rows,
                                 std::vector<uint32_t>* offsets,
                                 std::vector<uint32_t>* lens) {
  std::string blob;
  offsets->clear();
  lens->clear();
  for (const Row& row : rows) {
    offsets->push_back(static_cast<uint32_t>(blob.size()));
    lens->push_back(static_cast<uint32_t>(row.metadata.size()));
    blob.append(row.metadata);
  }
  return blob;
}

}  // namespace

Status WriteSstable(const std::string& path, uint64_t segment_id, Metric metric,
                    const std::vector<Row>& rows,
                    const SstableWriteOptions& options) {
  if (options.compression != CompressionCodec::kNone &&
      !CompressionEnabled()) {
    return Status::InvalidArgument(
        "compression requires ASTER_ENABLE_COMPRESSION");
  }
  if (options.compression != CompressionCodec::kNone &&
      options.compression != CompressionCodec::kLz4 &&
      options.compression != CompressionCodec::kZstd) {
    return Status::InvalidArgument("unknown compression codec");
  }

  // Validate sorted unique ids.
  for (size_t i = 1; i < rows.size(); ++i) {
    if (rows[i].id <= rows[i - 1].id) {
      return Status::InvalidArgument("rows must be sorted unique by id");
    }
  }

  uint64_t live = 0;
  uint32_t dimension = 0;
  uint64_t min_ts = 0, max_ts = 0;
  for (const Row& row : rows) {
    if (!row.tombstone) {
      ++live;
      if (dimension == 0) dimension = static_cast<uint32_t>(row.vector.size());
      if (row.vector.size() != dimension && dimension != 0) {
        return Status::InvalidArgument("inconsistent vector dimension");
      }
    }
    if (min_ts == 0 || row.timestamp < min_ts) min_ts = row.timestamp;
    if (row.timestamp > max_ts) max_ts = row.timestamp;
  }

  std::vector<std::string> ids;
  ids.reserve(rows.size());
  for (const Row& row : rows) ids.push_back(row.id);
  BloomFilter bloom = rows.empty()
                          ? BloomFilter(64, 1, 0)
                          : BloomFilter::Build(ids);

  std::vector<uint32_t> id_record_offsets;
  std::vector<uint32_t> vector_slots;
  std::string id_payload =
      BuildIdIndexPayload(rows, &id_record_offsets, &vector_slots);

  std::vector<uint32_t> meta_offsets, meta_lens;
  std::string meta_payload =
      BuildMetadataPayload(rows, &meta_offsets, &meta_lens);
  PatchIdMetadata(id_payload, id_record_offsets, rows, meta_offsets, meta_lens);

  std::string sparse_payload =
      BuildSparsePayload(rows, options.sparse_stride, id_record_offsets);
  std::string bloom_payload = BuildBloomPayload(bloom);
  std::string vector_payload = BuildVectorPayload(rows, dimension, live);
  std::string tags_payload = BuildTagsPayload(rows);

  std::string blocks_raw[kNumBlocks];
  blocks_raw[0] = WrapBlock(bloom_payload);
  blocks_raw[1] = WrapBlock(sparse_payload);
  blocks_raw[2] = WrapBlock(id_payload);
  blocks_raw[3] = vector_payload.empty() ? std::string() : WrapBlock(vector_payload);
  blocks_raw[4] = meta_payload.empty() && rows.empty()
                      ? std::string()
                      : WrapBlock(meta_payload);
  blocks_raw[5] = tags_payload.empty() ? std::string() : WrapBlock(tags_payload);
  blocks_raw[6] = std::string();  // tree empty

  const CompressionCodec codec = options.compression;
  const uint8_t codec_u8 = static_cast<uint8_t>(codec);

  std::string blocks_on_disk[kNumBlocks];
  for (size_t i = 0; i < kNumBlocks; ++i) {
    if (blocks_raw[i].empty()) {
      blocks_on_disk[i].clear();
      continue;
    }
    auto compressed = CompressBlockImage(blocks_raw[i], codec);
    if (!compressed.ok()) return compressed.status();
    blocks_on_disk[i] = std::move(compressed.value());
  }

  BlockDesc descs[kNumBlocks];
  std::string file;
  file.resize(kPreludeBytes, '\0');

  size_t cursor = kPreludeBytes;
  for (size_t i = 0; i < kNumBlocks; ++i) {
    cursor = Align8(cursor);
    // Prefer 32-align for uncompressed vector block (mmap SIMD).
    if (i == 3 && !blocks_on_disk[i].empty() &&
        codec == CompressionCodec::kNone) {
      while (cursor % 32 != 0) ++cursor;
    }
    descs[i].offset = cursor;
    descs[i].on_disk_len = static_cast<uint32_t>(blocks_on_disk[i].size());
    descs[i].compression =
        blocks_on_disk[i].empty() ? 0 : codec_u8;
    if (file.size() < cursor) file.append(cursor - file.size(), '\0');
    file.append(blocks_on_disk[i]);
    cursor = file.size();
  }
  cursor = Align8(cursor);
  if (file.size() < cursor) file.append(cursor - file.size(), '\0');
  const size_t footer_offset = file.size();

  // Patch header.
  {
    std::string hdr;
    AppendU32(hdr, kMagic);
    AppendU16(hdr, kFormatVersion);
    AppendU16(hdr, static_cast<uint16_t>(kHeaderBytes));
    uint32_t flags = 0;  // HAS_TREE=0
    if (!tags_payload.empty()) flags |= kFlagHasTags;
    AppendU32(hdr, flags);
    AppendU32(hdr, dimension);
    AppendU8(hdr, static_cast<uint8_t>(metric));
    AppendU8(hdr, 0);  // float32
    AppendU16(hdr, 0);
    AppendU64(hdr, segment_id);
    AppendU64(hdr, rows.size());
    AppendU64(hdr, live);
    AppendU64(hdr, rows.empty() ? 0 : min_ts);
    AppendU64(hdr, rows.empty() ? 0 : max_ts);
    AppendU32(hdr, options.sparse_stride);
    AppendU32(hdr, bloom.num_bits());
    AppendU32(hdr, bloom.num_hashes());
    AppendU64(hdr, bloom.seed());
    AppendU32(hdr, 0);  // created_unix_s
    AppendU32(hdr, 0);
    AppendU64(hdr, 0);
    AppendU64(hdr, 0);
    AppendU64(hdr, 0);
    AppendU64(hdr, 0);
    AppendU64(hdr, 0);
    hdr.resize(kHeaderBytes, '\0');
    std::memcpy(file.data(), hdr.data(), kHeaderBytes);
  }

  // Patch directory.
  {
    std::string dir;
    for (size_t i = 0; i < kNumBlocks; ++i) {
      AppendU64(dir, descs[i].offset);
      AppendU32(dir, descs[i].on_disk_len);
      AppendU8(dir, descs[i].compression);
      AppendU8(dir, 0);
      AppendU16(dir, 0);
    }
    std::memcpy(file.data() + kHeaderBytes, dir.data(), kDirectoryBytes);
  }

  // Footer.
  std::string footer;
  for (size_t i = 0; i < kNumBlocks; ++i) {
    uint32_t crc = 0;
    if (descs[i].on_disk_len > 0) {
      crc = Crc32(file.data() + descs[i].offset, descs[i].on_disk_len);
    }
    AppendU32(footer, crc);
  }
  AppendU32(footer, Crc32(file.data(), kPreludeBytes));
  AppendU32(footer, kFooterMagic);
  AppendU32(footer, kFormatVersion);
  file.append(footer);
  (void)footer_offset;

  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return Status::IoError("open failed: " + tmp);
    out.write(file.data(), static_cast<std::streamsize>(file.size()));
    if (!out) return Status::IoError("write failed: " + tmp);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return Status::IoError("rename failed: " + path);
  }
  return Status::Ok();
}

Result<std::unique_ptr<SstableReader>> SstableReader::Open(
    const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Status::IoError("open failed: " + path);
  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (data.size() > kMaxSstableFileBytes) {
    return Status::Corruption("sstable exceeds size limit");
  }
  if (data.size() < kPreludeBytes + kFooterBytes) {
    return Status::Corruption("sstable too small");
  }

  size_t off = 0;
  const uint32_t magic = ReadU32(data, off);
  if (magic != kMagic) return Status::Corruption("bad sstable magic");
  const uint16_t version = ReadU16(data, off);
  if (version != kFormatVersion) {
    return Status::Corruption("unsupported sstable version");
  }
  const uint16_t header_bytes = ReadU16(data, off);
  if (header_bytes != kHeaderBytes) {
    return Status::Corruption("unexpected header size");
  }
  ReadU32(data, off);  // flags
  const uint32_t dimension = ReadU32(data, off);
  const uint8_t metric_u8 = ReadU8(data, off);
  ReadU8(data, off);   // encoding
  ReadU16(data, off);  // reserved0
  const uint64_t segment_id = ReadU64(data, off);
  const uint64_t row_count = ReadU64(data, off);
  const uint64_t live_row_count = ReadU64(data, off);
  ReadU64(data, off);  // min_ts
  ReadU64(data, off);  // max_ts
  const uint32_t sparse_stride = ReadU32(data, off);
  const uint32_t bloom_bits = ReadU32(data, off);
  const uint32_t bloom_hashes = ReadU32(data, off);
  const uint64_t bloom_seed = ReadU64(data, off);
  (void)sparse_stride;

  if (dimension > kMaxDimension) {
    return Status::Corruption("sstable dimension too large");
  }
  if (row_count > kMaxRowsPerSstable || live_row_count > row_count) {
    return Status::Corruption("sstable row_count invalid");
  }
  if (dimension > 0 && live_row_count >
                           (std::numeric_limits<size_t>::max() / dimension)) {
    return Status::Corruption("sstable vector block would overflow");
  }

  // Footer checks.
  const size_t footer_off = data.size() - kFooterBytes;
  size_t foff = footer_off;
  uint32_t block_crc[kNumBlocks];
  for (size_t i = 0; i < kNumBlocks; ++i) block_crc[i] = ReadU32(data, foff);
  const uint32_t header_crc = ReadU32(data, foff);
  const uint32_t footer_magic = ReadU32(data, foff);
  const uint32_t footer_version = ReadU32(data, foff);
  if (footer_magic != kFooterMagic || footer_version != kFormatVersion) {
    return Status::Corruption("bad sstable footer");
  }
  if (Crc32(data.data(), kPreludeBytes) != header_crc) {
    return Status::Corruption("header crc mismatch");
  }

  BlockDesc descs[kNumBlocks];
  size_t doff = kHeaderBytes;
  for (size_t i = 0; i < kNumBlocks; ++i) {
    descs[i].offset = ReadU64(data, doff);
    descs[i].on_disk_len = ReadU32(data, doff);
    descs[i].compression = ReadU8(data, doff);
    ReadU8(data, doff);
    ReadU16(data, doff);
    if (descs[i].on_disk_len > 0) {
      if (descs[i].offset + descs[i].on_disk_len > footer_off) {
        return Status::Corruption("block overruns footer");
      }
      if (Crc32(data.data() + descs[i].offset, descs[i].on_disk_len) !=
          block_crc[i]) {
        return Status::Corruption("block crc mismatch");
      }
    }
  }

  auto unwrap = [&](size_t bi) -> Result<std::string> {
    if (descs[bi].on_disk_len == 0) return std::string();
    const CompressionCodec codec =
        static_cast<CompressionCodec>(descs[bi].compression);
    auto image = DecompressBlockImage(data.data() + descs[bi].offset,
                                      descs[bi].on_disk_len, codec);
    if (!image.ok()) return image.status();
    const std::string& uncompressed = image.value();
    if (uncompressed.size() < 4) {
      return Status::Corruption("block image too small");
    }
    size_t bo = 0;
    const uint32_t plen = ReadU32(uncompressed, bo);
    if (bo + plen > uncompressed.size()) {
      return Status::Corruption("block length overrun");
    }
    return uncompressed.substr(bo, plen);
  };

  auto bloom_payload = unwrap(0);
  if (!bloom_payload.ok()) return bloom_payload.status();
  auto sparse_payload = unwrap(1);
  if (!sparse_payload.ok()) return sparse_payload.status();
  auto id_payload = unwrap(2);
  if (!id_payload.ok()) return id_payload.status();
  auto vector_payload = unwrap(3);
  if (!vector_payload.ok()) return vector_payload.status();
  auto meta_payload = unwrap(4);
  if (!meta_payload.ok()) return meta_payload.status();
  auto tags_payload = unwrap(5);
  if (!tags_payload.ok()) return tags_payload.status();

  auto reader = std::unique_ptr<SstableReader>(new SstableReader());
  reader->data_ = std::move(data);
  reader->segment_id_ = segment_id;
  reader->row_count_ = row_count;
  reader->live_row_count_ = live_row_count;
  reader->dimension_ = dimension;
  reader->metric_ = static_cast<Metric>(metric_u8);
  reader->sparse_stride_ = sparse_stride;

  if (bloom_bits > 0 && bloom_payload.ok()) {
    std::vector<uint8_t> bits(bloom_payload.value().begin(),
                              bloom_payload.value().end());
    reader->bloom_ =
        BloomFilter::FromBits(bloom_bits, bloom_hashes, bloom_seed,
                              std::move(bits));
  }

  // Parse ID index.
  {
    const std::string& payload = id_payload.value();
    size_t po = 0;
    reader->id_entries_.reserve(static_cast<size_t>(row_count));
    for (uint64_t i = 0; i < row_count; ++i) {
      IdEntry e;
      e.record_begin = po;
      const uint16_t id_len = ReadU16(payload, po);
      e.id = payload.substr(po, id_len);
      po += id_len;
      e.flags = ReadU8(payload, po);
      ReadU8(payload, po);
      e.timestamp = ReadU64(payload, po);
      e.version = ReadU64(payload, po);
      e.vector_slot = ReadU32(payload, po);
      e.metadata_offset = ReadU32(payload, po);
      e.metadata_len = ReadU32(payload, po);
      ReadU32(payload, po);  // tag ordinal
      reader->id_entries_.push_back(std::move(e));
    }
    reader->id_payload_ = payload;
  }

  // Vectors.
  if (!vector_payload.value().empty() && live_row_count > 0 && dimension > 0) {
    const std::string& vp = vector_payload.value();
    size_t vo = 0;
    const uint32_t pad = ReadU32(vp, vo);
    vo += pad;
    reader->vectors_.resize(static_cast<size_t>(live_row_count) * dimension);
    for (size_t i = 0; i < reader->vectors_.size(); ++i) {
      reader->vectors_[i] = ReadF32(vp, vo);
    }
  }

  reader->metadata_blob_ = meta_payload.value();

  if (auto st = ParseTagsPayload(tags_payload.value(),
                                 static_cast<size_t>(row_count),
                                 &reader->row_tags_);
      !st.ok()) {
    return st;
  }

  // Sparse (optional acceleration; Get uses binary search on id_entries_).
  if (!sparse_payload.value().empty()) {
    const std::string& sp = sparse_payload.value();
    size_t so = 0;
    while (so + 10 <= sp.size()) {
      ReadU32(sp, so);  // ordinal
      ReadU32(sp, so);  // id_index_offset
      const uint16_t id_len = ReadU16(sp, so);
      if (so + id_len > sp.size()) break;
      std::string id = sp.substr(so, id_len);
      so += id_len;
      reader->sparse_.emplace_back(std::move(id), 0);
    }
  }

  return reader;
}

std::optional<Row> SstableReader::Get(const RowId& id) const {
  if (!MayContain(id)) return std::nullopt;
  auto it = std::lower_bound(
      id_entries_.begin(), id_entries_.end(), id,
      [](const IdEntry& e, const RowId& key) { return e.id < key; });
  if (it == id_entries_.end() || it->id != id) return std::nullopt;

  Row row;
  row.id = it->id;
  row.timestamp = it->timestamp;
  row.version = it->version;
  row.tombstone = (it->flags & 0x01) != 0;
  if (!row.tombstone && it->vector_slot != kNoVector && dimension_ > 0) {
    const size_t base = static_cast<size_t>(it->vector_slot) * dimension_;
    row.vector.assign(vectors_.begin() + static_cast<std::ptrdiff_t>(base),
                      vectors_.begin() + static_cast<std::ptrdiff_t>(base + dimension_));
  }
  if (it->metadata_len > 0 &&
      it->metadata_offset + it->metadata_len <= metadata_blob_.size()) {
    row.metadata = metadata_blob_.substr(it->metadata_offset, it->metadata_len);
  }
  const size_t ordinal =
      static_cast<size_t>(std::distance(id_entries_.begin(), it));
  if (ordinal < row_tags_.size()) {
    row.tags = row_tags_[ordinal];
  }
  return row;
}

std::vector<Row> SstableReader::LoadAll() const {
  std::vector<Row> out;
  out.reserve(id_entries_.size());
  for (const auto& e : id_entries_) {
    if (auto row = Get(e.id)) out.push_back(std::move(*row));
  }
  return out;
}

std::vector<Row> SstableReader::TakeAll() {
  std::vector<Row> out;
  out.reserve(id_entries_.size());
  for (size_t i = 0; i < id_entries_.size(); ++i) {
    IdEntry& e = id_entries_[i];
    Row row;
    row.id = std::move(e.id);
    row.timestamp = e.timestamp;
    row.version = e.version;
    row.tombstone = (e.flags & 0x01) != 0;
    if (!row.tombstone && e.vector_slot != kNoVector && dimension_ > 0) {
      const size_t base = static_cast<size_t>(e.vector_slot) * dimension_;
      const size_t end = base + dimension_;
      if (end <= vectors_.size()) {
        row.vector.resize(dimension_);
        std::move(vectors_.begin() + static_cast<std::ptrdiff_t>(base),
                  vectors_.begin() + static_cast<std::ptrdiff_t>(end),
                  row.vector.begin());
      }
    }
    if (e.metadata_len > 0 &&
        e.metadata_offset + e.metadata_len <= metadata_blob_.size()) {
      row.metadata =
          metadata_blob_.substr(e.metadata_offset, e.metadata_len);
    }
    if (i < row_tags_.size()) {
      row.tags = std::move(row_tags_[i]);
    }
    out.push_back(std::move(row));
  }

  // Drop file + index scratch; caller owns the rows now.
  data_.clear();
  data_.shrink_to_fit();
  id_entries_.clear();
  id_entries_.shrink_to_fit();
  id_payload_.clear();
  id_payload_.shrink_to_fit();
  vectors_.clear();
  vectors_.shrink_to_fit();
  metadata_blob_.clear();
  metadata_blob_.shrink_to_fit();
  row_tags_.clear();
  row_tags_.shrink_to_fit();
  sparse_.clear();
  sparse_.shrink_to_fit();
  bloom_ = BloomFilter();
  row_count_ = 0;
  live_row_count_ = 0;
  return out;
}

}  // namespace aster
