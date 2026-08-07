#include "aster/index/bloom.h"

#include <algorithm>
#include <cmath>

#include "aster/core/hash.h"

namespace aster {
namespace {
constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ULL;

uint32_t NextPow2(uint32_t n) {
  if (n <= 64) return 64;
  --n;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}
}  // namespace

BloomFilter::BloomFilter(uint32_t num_bits, uint32_t num_hashes, uint64_t seed)
    : num_bits_(num_bits),
      num_hashes_(num_hashes),
      seed_(seed),
      bits_((num_bits + 7) / 8, 0) {}

BloomFilter BloomFilter::Build(const std::vector<std::string>& keys,
                               double fpp) {
  const uint32_t n = static_cast<uint32_t>(std::max<size_t>(keys.size(), 1));
  // m ≈ -n ln(p) / (ln2)^2 ; RFC recommends max(64, next_pow2(n*10)) for M1.
  uint32_t bits = NextPow2(n * 10);
  if (fpp > 0.0 && fpp < 1.0) {
    const double m =
        -static_cast<double>(n) * std::log(fpp) / (std::log(2.0) * std::log(2.0));
    bits = std::max(bits, NextPow2(static_cast<uint32_t>(m)));
  }
  constexpr uint32_t kHashes = 4;
  BloomFilter bloom(bits, kHashes, /*seed=*/0);
  for (const auto& key : keys) bloom.Add(key);
  return bloom;
}

BloomFilter BloomFilter::FromBits(uint32_t num_bits, uint32_t num_hashes,
                                  uint64_t seed, std::vector<uint8_t> bits) {
  BloomFilter bloom(num_bits, num_hashes, seed);
  bloom.bits_ = std::move(bits);
  return bloom;
}

void BloomFilter::Add(std::string_view key) {
  if (num_bits_ == 0 || num_hashes_ == 0) return;
  const uint64_t h = Hash64(key, seed_);
  for (uint32_t i = 0; i < num_hashes_; ++i) {
    const uint64_t bit = (h + i * kGolden) % num_bits_;
    bits_[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
  }
}

bool BloomFilter::MayContain(std::string_view key) const {
  if (num_bits_ == 0 || num_hashes_ == 0) return true;  // no filter
  const uint64_t h = Hash64(key, seed_);
  for (uint32_t i = 0; i < num_hashes_; ++i) {
    const uint64_t bit = (h + i * kGolden) % num_bits_;
    if ((bits_[bit / 8] & static_cast<uint8_t>(1u << (bit % 8))) == 0) {
      return false;
    }
  }
  return true;
}

}  // namespace aster
