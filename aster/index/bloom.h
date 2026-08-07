#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aster {

// Bloom filter for SSTable negative lookups (docs/sstable-format.md §7.1).
class BloomFilter {
 public:
  BloomFilter() = default;
  BloomFilter(uint32_t num_bits, uint32_t num_hashes, uint64_t seed = 0);

  static BloomFilter Build(const std::vector<std::string>& keys,
                           double fpp = 0.01);

  void Add(std::string_view key);
  bool MayContain(std::string_view key) const;

  uint32_t num_bits() const { return num_bits_; }
  uint32_t num_hashes() const { return num_hashes_; }
  uint64_t seed() const { return seed_; }
  const std::vector<uint8_t>& bits() const { return bits_; }

  // Reconstruct from stored bitset.
  static BloomFilter FromBits(uint32_t num_bits, uint32_t num_hashes,
                              uint64_t seed, std::vector<uint8_t> bits);

 private:
  uint32_t num_bits_ = 0;
  uint32_t num_hashes_ = 0;
  uint64_t seed_ = 0;
  std::vector<uint8_t> bits_;
};

}  // namespace aster
