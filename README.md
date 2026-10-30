# Aster

[![CI](https://github.com/cenkcorapci/Aster/actions/workflows/ci.yml/badge.svg)](https://github.com/cenkcorapci/Aster/actions/workflows/ci.yml)

Peer-to-peer vector database: LSM storage, per-segment indexes, CPU-only
search. Runs from MCUs to multi-node clusters. C++20, Bazel, AGPL-3.0.

**Status:** 1.0 — stable single-node engine with segmented HNSW, HTTP +
Thrift APIs, and catalog quotas. Cluster replication and AWS S3 (SigV4)
are on the [roadmap](docs/development-plan.md) and not part of 1.0.

## Install

### Docker (GHCR image)

Install the CLI from source into `~/.local/bin`:

```bash
curl -fsSL https://raw.githubusercontent.com/cenkcorapci/Aster/main/scripts/install.sh | sh
```

```bash
docker run -d -p 8080:8080 -v aster-data:/data \
  -e ASTER_DATA_DIR=/data ghcr.io/cenkcorapci/aster:latest \
  serve --host 0.0.0.0 --port 8080
curl http://localhost:8080/health
```

### From source

Needs [Bazel](https://bazel.build) (version in `.bazelversion`; Bazelisk
recommended) and a C++20 compiler.

```bash
git clone https://github.com/cenkcorapci/Aster && cd Aster
bazel build -c opt //aster/cli:aster
./bazel-bin/aster/cli/aster --help
```

Use `-c opt` for production and benchmarks; debug builds are much slower.

## Usage

Start the HTTP server (set `--api-key` or `ASTER_API_KEY` to require auth):

```bash
./bazel-bin/aster/cli/aster serve --data-dir /var/lib/aster --port 8080
```

Create a collection, upsert, and search:

```bash
curl -X PUT localhost:8080/v1/collections/docs \
  -H 'Content-Type: application/json' -d '{"dimension":3,"metric":"cosine"}'
curl -X PUT localhost:8080/v1/collections/docs/docs/a \
  -H 'Content-Type: application/json' -d '{"vector":[0.1,0.2,0.3]}'
curl -X POST localhost:8080/v1/collections/docs/search \
  -H 'Content-Type: application/json' -d '{"vector":[0.1,0.2,0.3],"top_k":5}'
```

Full API: [HTTP API](docs/tutorials/http-api.md). Config file:
[server-config](docs/server-config.md). Thrift: `aster serve-rpc`.
Embedding in C++: [database management](docs/tutorials/database-management.md).
Python client: [`clients/python`](clients/python).

## Development

```bash
bazel test //aster/...          # unit + integration tests
./scripts/run-coverage.sh       # ≥90% line coverage gate on core aster libs
bazel run //aster/cli:aster -- serve --data-dir /tmp/aster --port 8080
bazel run //aster/cli:aster     # local demo
./scripts/docker-build.sh       # BusyBox image → aster:local
```

## Tutorials

- [Database management](docs/tutorials/database-management.md) — open, upsert, search, flush, compact, Docker
- [HTTP API](docs/tutorials/http-api.md) — multi-collection JSON server (`aster serve`)
- [Client libraries](docs/tutorials/client-libraries.md) — seven-language facades (transport = M5)

Full docs index: [docs/README.md](docs/README.md).

## Layout

| Path | What |
| --- | --- |
| `aster/` | Engine (`db`, `embedded`, storage, index, …) |
| `clients/` | Client SDKs ([README](clients/README.md)) |
| `docs/` | Design, tutorials, roadmap |
| `tla/` | Formal specs ([README](tla/README.md)) |

## More

```bash
./scripts/run-coverage.sh              # ≥90% gate on core aster libs (excl. cli, gen-cpp, see script)
./scripts/build-matrix.sh              # Tiny / Edge / Server / Arduino / musl
make bench-local                       # kind soak (local disk)
make bench-minio                       # kind soak + MinIO
make bench-multitenant-smoke           # multi-tenant Catalog (mixed dims/rows)
make bench-scale                       # elastic scale 15↔50 (local+minio)
make sim-grafana                       # 50-node MinIO sim + Grafana dashboards
make sim-grafana-stop
make sim-arduino                       # ESP32 Tiny firmware + Espressif QEMU
make sim-arduino-native                # same harness on host (no MCU)
make bench-vs-milvus-smoke             # Aster vs Milvus (tiny)
make bench-vs-milvus                   # Aster vs Milvus (100M×2048 target, auto-scaled)
make bench-vs-qdrant-smoke             # Aster vs Qdrant (tiny)
make bench-vs-qdrant                   # Aster vs Qdrant (100M×2048 target, auto-scaled)
bazel build --config=arduino //aster/embedded   # MCU static lib
```

Multi-tenant: [deploy/bench-multitenant/README.md](deploy/bench-multitenant/README.md).  
Scale: [deploy/bench-scale/README.md](deploy/bench-scale/README.md).  
Live Grafana sim: [deploy/sim-grafana/README.md](deploy/sim-grafana/README.md).  
Arduino / ESP32 QEMU sim: [deploy/sim-arduino/README.md](deploy/sim-arduino/README.md).  
Aster vs Milvus: [deploy/compare-milvus/README.md](deploy/compare-milvus/README.md) ·
[measured numbers](docs/bench-vs-milvus.md).
Aster vs Qdrant: [docs/bench-vs-qdrant.md](docs/bench-vs-qdrant.md).

### Aster vs Milvus (Apple M4, 2026-09-03)

Kind + shared MinIO. HNSW `M=16`, `ef=128`, dim 2048, seed 42.

| Profile | Corpus | Aster search p50 | Milvus search p50 | Jaccard@10 | Milvus recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Smoke (2 shards) | 2 000 | 17.8 ms | 4.5 ms | 0.91 | 0.95 |
| Laptop full (4 shards) | 40 000 | 41.5 ms | 4.6 ms | 0.20 | 0.32 |

Aster load is HTTP upsert + per-segment index build; Milvus batch-insert is
much faster on this harness. Full write-up: [docs/bench-vs-milvus.md](docs/bench-vs-milvus.md).

CI runs `bazel test //aster/...` on every push/PR to `main`, plus an
`ubuntu-24.04-arm` Edge job (`--config=raspberry_pi`). Local matrix:
`./scripts/build-matrix.sh --full`.

License: [AGPL-3.0](LICENSE).
