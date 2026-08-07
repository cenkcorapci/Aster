#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"
#include "aster/index/bloom.h"

namespace aster {

// On-disk SSTable (.ast) per docs/sstable-format.md.
// Compression is None in this milestone (Tiny-compatible default).

struct SstableWriteOptions {
  uint32_t sparse_stride = 16;
};

// Writes an immutable segment file. Rows must already be sorted by id with
// at most one version per id (including tombstones).
Status WriteSstable(const std::string& path, uint64_t segment_id, Metric metric,
                    const std::vector<Row>& rows,
                    const SstableWriteOptions& options = {});

// Memory-mapped / fully-read SSTable for point lookups and row materialization.
class SstableReader {
 public:
  static Result<std::unique_ptr<SstableReader>> Open(const std::string& path);

  uint64_t segment_id() const { return segment_id_; }
  uint64_t row_count() const { return row_count_; }
  uint32_t dimension() const { return dimension_; }
  Metric metric() const { return metric_; }
  const BloomFilter& bloom() const { return bloom_; }

  // False when the bloom filter confidently excludes the id (negative lookup).
  bool MayContain(const RowId& id) const { return bloom_.MayContain(id); }

  std::optional<Row> Get(const RowId& id) const;

  // Materialize all rows (including tombstones) in id order.
  std::vector<Row> LoadAll() const;

 private:
  SstableReader() = default;

  std::string data_;  // whole file in memory for M1 (mmap later)
  uint64_t segment_id_ = 0;
  uint64_t row_count_ = 0;
  uint64_t live_row_count_ = 0;
  uint32_t dimension_ = 0;
  Metric metric_ = Metric::kL2;
  uint32_t sparse_stride_ = 16;
  BloomFilter bloom_;

  struct IdEntry {
    RowId id;
    uint8_t flags = 0;
    Timestamp timestamp = 0;
    Version version = 0;
    uint32_t vector_slot = 0xFFFFFFFFu;
    uint32_t metadata_offset = 0;
    uint32_t metadata_len = 0;
    size_t record_begin = 0;  // offset into id_payload_
  };

  std::vector<IdEntry> id_entries_;
  std::string id_payload_;
  std::vector<float> vectors_;     // live_row_count * dimension
  std::string metadata_blob_;
  std::vector<std::pair<std::string, uint32_t>> sparse_;  // id -> ordinal
};

}  // namespace aster
