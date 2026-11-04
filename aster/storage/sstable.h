#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"
#include "aster/index/bloom.h"
#include "aster/index/vector_encoding.h"
#include "aster/platform/mapped_file.h"
#include "aster/storage/storage_policy.h"

namespace aster {

// On-disk SSTable (.ast) per docs/sstable-format.md.
// Directory compression field: 0=None, 1=LZ4, 2=ZSTD.

enum class CompressionCodec : uint8_t {
  kNone = 0,
  kLz4 = 1,
  kZstd = 2,
};

struct SstableWriteOptions {
  uint32_t sparse_stride = 16;
  // Default None keeps Tiny / uncompressed layouts as the common path.
  // Non-None requires ASTER_ENABLE_COMPRESSION (Edge/Server profiles).
  CompressionCodec compression = CompressionCodec::kNone;
  // Packed vector payload encoding (header byte `vector_encoding`).
  VectorEncoding vector_encoding = VectorEncoding::kFloat32;
};

// Writes an immutable segment file. Rows must already be sorted by id with
// at most one version per id (including tombstones).
Status WriteSstable(const std::string& path, uint64_t segment_id, Metric metric,
                    const std::vector<Row>& rows,
                    const SstableWriteOptions& options = {});

// Memory-mapped SSTable for point lookups and segment reopen.
// Uncompressed vector blocks are viewed in-place (no anonymous payload copy).
// Compressed blocks keep a decompressed heap image for that block only.
class SstableReader {
 public:
  static Result<std::unique_ptr<SstableReader>> Open(
      const std::string& path, const StoragePolicy& policy = {});

  uint64_t segment_id() const { return segment_id_; }
  uint64_t row_count() const { return row_count_; }
  uint32_t dimension() const { return dimension_; }
  Metric metric() const { return metric_; }
  VectorEncoding vector_encoding() const { return vector_encoding_; }
  const BloomFilter& bloom() const { return bloom_; }
  const StoragePolicy& storage_policy() const { return policy_; }

  // Packed live vectors as stored on disk. Empty when no live vectors.
  const CompactVectorStore* packed_vectors() const {
    return packed_vectors_ ? packed_vectors_.get() : nullptr;
  }
  std::shared_ptr<const CompactVectorStore> SharePackedVectors() const {
    return packed_vectors_;
  }
  bool vectors_file_backed() const {
    return packed_vectors_ && packed_vectors_->is_file_backed();
  }

  // False when the bloom filter confidently excludes the id (negative lookup).
  bool MayContain(const RowId& id) const { return bloom_.MayContain(id); }

  std::optional<Row> Get(const RowId& id) const;

  // Materialize all rows (including tombstones) in id order. Decodes vectors.
  std::vector<Row> LoadAll() const;

  // Like LoadAll, but moves metadata out of the reader. Vectors are decoded
  // into each Row for API compatibility; prefer ReleaseForSegment() on reopen.
  std::vector<Row> TakeAll();

  // Lowest-RAM reopen handoff: metadata rows with empty vectors + packed store
  // + live_slots parallel to rows. Clears reader state.
  struct SegmentPayload {
    std::shared_ptr<std::vector<Row>> rows;  // vectors empty for live rows
    std::shared_ptr<const CompactVectorStore> packed;
    std::shared_ptr<const std::vector<uint32_t>> live_slots;
    VectorEncoding encoding = VectorEncoding::kFloat32;
    Metric metric = Metric::kL2;
    uint64_t segment_id = 0;
  };
  Result<SegmentPayload> ReleaseForSegment();

 private:
  SstableReader() = default;

  StoragePolicy policy_{};
  std::shared_ptr<MappedFile> mapped_;
  // Owned copies of decompressed (or small) blocks that are not views.
  std::string bloom_owned_;
  std::string sparse_owned_;
  std::string id_owned_;
  std::string vector_owned_;  // only when compressed or pinned
  std::string meta_owned_;
  std::string tags_owned_;

  uint64_t segment_id_ = 0;
  uint64_t row_count_ = 0;
  uint64_t live_row_count_ = 0;
  uint32_t dimension_ = 0;
  Metric metric_ = Metric::kL2;
  VectorEncoding vector_encoding_ = VectorEncoding::kFloat32;
  uint32_t sparse_stride_ = 16;
  BloomFilter bloom_;
  std::shared_ptr<CompactVectorStore> packed_vectors_;

  struct IdEntry {
    RowId id;
    uint8_t flags = 0;
    Timestamp timestamp = 0;
    Version version = 0;
    uint32_t vector_slot = 0xFFFFFFFFu;
    uint32_t metadata_offset = 0;
    uint32_t metadata_len = 0;
  };

  std::vector<IdEntry> id_entries_;
  std::string_view metadata_blob_;
  std::vector<std::set<std::string>> row_tags_;
  std::vector<std::pair<std::string, uint32_t>> sparse_;
};

}  // namespace aster
