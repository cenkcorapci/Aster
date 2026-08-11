// Host-side smoke of the same Tiny harness (no MCU). Used when QEMU is skipped.

#include <cstdio>

#include "smoke.h"

int main() {
  auto ok = aster::sim::RunSmoke([](const char* s) { std::fputs(s, stdout); });
  return ok ? 0 : 1;
}
