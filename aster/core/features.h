#pragma once

// Compile-time deployment profiles (docs/code-structure.md, docs/tasks.md M3-T05).
// Exactly one of ASTER_PROFILE_TINY / EDGE / SERVER should be set via -D.
// If none is set, SERVER is the default (full feature surface).

#if defined(ASTER_PROFILE_TINY) && defined(ASTER_PROFILE_EDGE)
#error "Aster: only one of ASTER_PROFILE_TINY / EDGE / SERVER may be defined"
#endif
#if defined(ASTER_PROFILE_TINY) && defined(ASTER_PROFILE_SERVER)
#error "Aster: only one of ASTER_PROFILE_TINY / EDGE / SERVER may be defined"
#endif
#if defined(ASTER_PROFILE_EDGE) && defined(ASTER_PROFILE_SERVER)
#error "Aster: only one of ASTER_PROFILE_TINY / EDGE / SERVER may be defined"
#endif

#if !defined(ASTER_PROFILE_TINY) && !defined(ASTER_PROFILE_EDGE) && \
    !defined(ASTER_PROFILE_SERVER)
#define ASTER_PROFILE_SERVER 1
#endif

// Feature defaults per profile. Callers may still -DASTER_ENABLE_*=0/1 to override
// individually after including this header only if they #undef first — prefer
// setting overrides on the compiler command line before the include via -D.

#ifndef ASTER_ENABLE_HNSW
#if defined(ASTER_PROFILE_TINY)
#define ASTER_ENABLE_HNSW 0
#else
#define ASTER_ENABLE_HNSW 1
#endif
#endif

#ifndef ASTER_ENABLE_GOSSIP
#if defined(ASTER_PROFILE_TINY)
#define ASTER_ENABLE_GOSSIP 0
#else
#define ASTER_ENABLE_GOSSIP 1
#endif
#endif

#ifndef ASTER_ENABLE_COMPRESSION
#if defined(ASTER_PROFILE_TINY)
#define ASTER_ENABLE_COMPRESSION 0
#elif defined(ASTER_PROFILE_EDGE)
#define ASTER_ENABLE_COMPRESSION 1  // LZ4
#else
#define ASTER_ENABLE_COMPRESSION 1  // ZSTD on server
#endif
#endif

#ifndef ASTER_ENABLE_PROMETHEUS
#if defined(ASTER_PROFILE_TINY)
#define ASTER_ENABLE_PROMETHEUS 0
#else
#define ASTER_ENABLE_PROMETHEUS 1
#endif
#endif

#ifndef ASTER_ENABLE_REPLICATION
#if defined(ASTER_PROFILE_TINY)
#define ASTER_ENABLE_REPLICATION 0
#else
#define ASTER_ENABLE_REPLICATION 1
#endif
#endif

namespace aster {

enum class Profile {
  kTiny,
  kEdge,
  kServer,
};

inline constexpr Profile ActiveProfile() {
#if defined(ASTER_PROFILE_TINY)
  return Profile::kTiny;
#elif defined(ASTER_PROFILE_EDGE)
  return Profile::kEdge;
#else
  return Profile::kServer;
#endif
}

inline constexpr bool HnswEnabled() { return ASTER_ENABLE_HNSW != 0; }
inline constexpr bool GossipEnabled() { return ASTER_ENABLE_GOSSIP != 0; }
inline constexpr bool CompressionEnabled() {
  return ASTER_ENABLE_COMPRESSION != 0;
}
inline constexpr bool PrometheusEnabled() {
  return ASTER_ENABLE_PROMETHEUS != 0;
}
inline constexpr bool ReplicationEnabled() {
  return ASTER_ENABLE_REPLICATION != 0;
}

}  // namespace aster
