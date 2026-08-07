# Aster

Peer-to-peer vector database: LSM storage, per-segment indexes, CPU-only
search. Runs from MCUs to multi-node clusters. C++20, Bazel, AGPL-3.0.

**Status:** pre-alpha — single-node engine works; HNSW/RPC/clustering still WIP.
See the [development plan](docs/development-plan.md).

## Quick start

Needs [Bazel](https://bazel.build) (`.bazelversion`) and a C++20 compiler.

```bash
bazel test //aster/...          # tests
bazel run //aster/cli:aster     # local demo
./scripts/docker-build.sh       # BusyBox image → aster:local
```

## Layout

| Path | What |
| --- | --- |
| `aster/` | Engine (`db`, `embedded`, storage, index, …) |
| `clients/` | Client SDKs ([README](clients/README.md)) |
| `docs/` | Design, indexing, roadmap |
| `tla/` | Formal specs ([README](tla/README.md)) |

## More

```bash
./scripts/run-coverage.sh              # ≥90% gate
./scripts/build-matrix.sh              # Tiny / Edge / Server / Arduino / musl
make bench-local                       # kind soak (local disk)
make bench-minio                       # kind soak + MinIO
bazel build --config=arduino //aster/embedded   # MCU static lib
```

CI runs `bazel test //aster/...` on every push/PR to `main`.

License: [AGPL-3.0](LICENSE).
