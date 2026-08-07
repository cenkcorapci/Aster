#include "aster/core/hash.h"

namespace aster {

namespace {
// splitmix64 finalizer for good avalanche behavior.
uint64_t Mix(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}
}  // namespace

uint64_t Hash64(std::string_view data, uint64_t seed) {
  uint64_t h = 0xcbf29ce484222325ULL ^ Mix(seed);
  for (unsigned char c : data) {
    h ^= c;
    h *= 0x100000001b3ULL;  // FNV prime
  }
  return Mix(h);
}

}  // namespace aster
