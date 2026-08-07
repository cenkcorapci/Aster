// In-memory Tiny engine for Arduino / MCU / freestanding embeds.
// No WAL, SSTable, or POSIX — only memtable + sealed segments + exact search.
//
//   bazel build --config=arduino //aster/embedded
//
// Link this static library from PlatformIO / Arduino-ESP32 / bare-metal
// toolchains. Feature flags follow ASTER_PROFILE_TINY.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "aster/core/status.h"
#include "aster/core/types.h"
#include "aster/storage/memtable.h"
#include "aster/storage/segment.h"

namespace aster {
namespace embedded {

class Db {
 public:
  struct Options {
    uint32_t dimension = 0;
    Metric metric = Metric::kCosine;
    // Soft cap: Flush() is called automatically when memtable rows exceed this.
    size_t memtable_flush_rows = 256;
  };

  explicit Db(Options options);

  Status Upsert(Row row);
  Status Delete(const RowId& id, Timestamp timestamp);

  std::optional<Row> Get(const RowId& id) const;
  std::vector<SearchHit> Search(const SearchRequest& request) const;

  Status Flush();
  Status Compact();

  size_t segment_count() const { return segments_.size(); }
  size_t memtable_rows() const { return memtable_.row_count(); }

 private:
  Row Reconcile(const RowId& id) const;

  Options options_;
  Memtable memtable_;
  std::vector<std::shared_ptr<const Segment>> segments_;
  uint64_t next_segment_id_ = 1;
};

}  // namespace embedded
}  // namespace aster
