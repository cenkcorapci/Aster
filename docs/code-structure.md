# Aster code structure

Layered C++20 monorepo: the kernel has no mandatory OS dependencies;
platform backends plug in storage, clocks, and (later) networking.

## Source tree

```
aster/
├── core/           types, Status/Result, hashing, feature profiles
├── storage/        WAL, memtable, SSTable, manifest, segment
├── index/          distance kernels, bloom, exact index (HNSW = M2)
├── query/          top-k merge across segments / replicas
├── distributed/    consistent-hash ring + vnodes
├── db/             hosted single-node Db (POSIX durable path)
├── server/         multi-collection catalog + HTTP JSON API
├── embedded/       Tiny in-memory Db (Arduino / bare metal)
├── rpc/            aster.thrift (server = M4)
├── platform/       IStorageBackend: memory + POSIX (+ S3 later)
├── metrics/        counters / histograms (Prometheus = M4)
├── cli/            local demo binary
├── bench/          soak / load binary
└── integration/    end-to-end tests
clients/            seven language facades (transport = M5)
docs/               design, tutorials, roadmap
tla/                TLC-checked specs
platforms/          Bazel platform constraints (CPU/OS/musl/baremetal)
```

## Dependency direction

```
cli / bench / embedded
        ↓
       db
        ↓
 storage ← index ← query
        ↓
      core
        ↑
   platform
```

`distributed` is independent of storage today (ring only). Gossip and
replication sit above `db` in M7.

## Profiles (`aster/core/features.h`)

| Config | Intent |
| --- | --- |
| `--config=tiny` | MCU: exact search, no HNSW/gossip |
| `--config=edge` | SBC / Pi: smaller defaults |
| `--config=server` | Full feature set (default) |

Orthogonal to CPU/OS platforms (`--config=apple_silicon`, `linux_musl_arm64`,
`arduino`, zig cross configs — see `.bazelrc`).

## Storage backends

| Backend | Status |
| --- | --- |
| `MemoryStorage` | Done |
| `PosixStorage` | Done (path traversal hardened) |
| S3 | Planned (M8) |

## Tests

```bash
bazel test //aster/...
```

CI runs the same target on every push/PR to `main`. Coverage gate:
`./scripts/run-coverage.sh` (≥90% on product libs).
