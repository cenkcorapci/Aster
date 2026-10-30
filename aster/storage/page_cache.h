#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace aster {

// Bounded hot-page cache for sealed mmap extents (Milestone B).
// Copies genuinely hot pages into anonymous RAM so cold traversals do not
// thrash the working set; cold misses remain zero-copy mmap views.
class PageCache {
 public:
  explicit PageCache(size_t budget_bytes = 64ull << 20)
      : budget_bytes_(budget_bytes) {}

  struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t inserts = 0;
    uint64_t evictions = 0;
    size_t bytes = 0;
    size_t entries = 0;
  };

  size_t budget_bytes() const { return budget_bytes_; }
  void set_budget_bytes(size_t n) { budget_bytes_ = n; }

  Stats stats() const {
    std::lock_guard lock(mu_);
    return stats_;
  }

  // Look up a cached page. On miss, optionally Insert() a copy later.
  std::optional<std::span<const uint8_t>> Find(uint64_t segment_id,
                                               uint32_t page_id) const {
    std::lock_guard lock(mu_);
    auto it = map_.find(Key{segment_id, page_id});
    if (it == map_.end()) {
      ++stats_.misses;
      return std::nullopt;
    }
    ++stats_.hits;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return std::span<const uint8_t>(it->second.data.data(),
                                    it->second.data.size());
  }

  // Copy `bytes` into the cache under (segment_id, page_id). May evict.
  std::span<const uint8_t> Insert(uint64_t segment_id, uint32_t page_id,
                                  std::span<const uint8_t> bytes) {
    std::lock_guard lock(mu_);
    const Key key{segment_id, page_id};
    auto it = map_.find(key);
    if (it != map_.end()) {
      stats_.bytes -= it->second.data.size();
      lru_.erase(it->second.lru);
      map_.erase(it);
    }
    while (stats_.bytes + bytes.size() > budget_bytes_ && !lru_.empty()) {
      EvictOneLocked();
    }
    if (bytes.size() > budget_bytes_) {
      // Too large for the budget — do not retain.
      ++stats_.misses;
      return {};
    }
    Entry e;
    e.data.assign(bytes.begin(), bytes.end());
    lru_.push_front(key);
    e.lru = lru_.begin();
    stats_.bytes += e.data.size();
    ++stats_.inserts;
    ++stats_.entries;
    auto [ins, _] = map_.emplace(key, std::move(e));
    return std::span<const uint8_t>(ins->second.data.data(),
                                    ins->second.data.size());
  }

  void Clear() {
    std::lock_guard lock(mu_);
    map_.clear();
    lru_.clear();
    stats_ = Stats{};
  }

 private:
  struct Key {
    uint64_t segment = 0;
    uint32_t page = 0;
    bool operator==(const Key& o) const {
      return segment == o.segment && page == o.page;
    }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return std::hash<uint64_t>{}(k.segment) ^
             (std::hash<uint32_t>{}(k.page) << 1);
    }
  };
  struct Entry {
    std::vector<uint8_t> data;
    std::list<Key>::iterator lru;
  };

  void EvictOneLocked() {
    const Key victim = lru_.back();
    lru_.pop_back();
    auto it = map_.find(victim);
    if (it == map_.end()) return;
    stats_.bytes -= it->second.data.size();
    if (stats_.entries > 0) --stats_.entries;
    ++stats_.evictions;
    map_.erase(it);
  }

  size_t budget_bytes_;
  mutable std::mutex mu_;
  mutable std::list<Key> lru_;
  mutable std::unordered_map<Key, Entry, KeyHash> map_;
  mutable Stats stats_;
};

}  // namespace aster
