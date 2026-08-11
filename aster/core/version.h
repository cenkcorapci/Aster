#pragma once

// Aster product / embedded API version.
// Canonical source: VERSION at the repo root. Keep these macros in sync via
// scripts/bump-version.sh (see docs/versioning.md).

#define ASTER_VERSION_MAJOR 0
#define ASTER_VERSION_MINOR 1
#define ASTER_VERSION_PATCH 0

#define ASTER_VERSION_STRING "0.1.0"

// True when (major, minor, patch) is at least (maj, min, pat).
#define ASTER_VERSION_AT_LEAST(maj, min, pat)                     \
  ((ASTER_VERSION_MAJOR > (maj)) ||                               \
   (ASTER_VERSION_MAJOR == (maj) && ASTER_VERSION_MINOR > (min)) || \
   (ASTER_VERSION_MAJOR == (maj) && ASTER_VERSION_MINOR == (min) && \
    ASTER_VERSION_PATCH >= (pat)))
