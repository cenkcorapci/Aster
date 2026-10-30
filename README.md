# Aster

[![CI](https://github.com/cenkcorapci/Aster/actions/workflows/ci.yml/badge.svg)](https://github.com/cenkcorapci/Aster/actions/workflows/ci.yml)
[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)

**Aster is a CPU-first vector database for embedded, local, and server
deployments.** It combines an LSM storage engine with per-segment HNSW indexes,
durable WAL/SSTables, an HTTP API, and a C++ embedding API. The same codebase
supports memory-constrained targets through multi-node benchmark harnesses.

This README is the fast path to running Aster. See the [documentation
index](docs/README.md) for design documents, API references, tutorials, and
canonical [milestone status and gates](docs/development-plan.md) /
[claimable work](docs/tasks.md).

## Contents

- [Choose an installation](#choose-an-installation)
- [Run your first collection](#run-your-first-collection)
- [Operate a server](#operate-a-server)
- [Embed Aster in C++](#embed-aster-in-c)
- [Run benchmarks](#run-benchmarks)
- [Develop and test](#develop-and-test)
- [Documentation map](#documentation-map)

## Choose an installation

### One-line source install

The installer downloads a source snapshot, uses your installed Bazel or
bootstraps the pinned Bazel version through Bazelisk, builds an optimized CLI,
and installs it into `~/.local/bin`.

```bash
curl -fsSL https://raw.githubusercontent.com/cenkcorapci/Aster/main/scripts/install.sh | sh
```

It requires `curl`, `tar`, and a C++20 compiler. The script supports macOS
Apple Silicon/Intel and Linux x86_64/arm64. Add `~/.local/bin` to `PATH` if the
installer reports that it is absent:

```bash
export PATH="$HOME/.local/bin:$PATH"
aster --help
```

Set `PREFIX` to select another destination or `ASTER_REF` to build a different
repository branch:

```bash
curl -fsSL https://raw.githubusercontent.com/cenkcorapci/Aster/main/scripts/install.sh | \
  PREFIX="$HOME/bin" ASTER_REF=main sh
```

### Docker

Run the published image with a named volume for durable data:

```bash
docker run -d --name aster -p 8080:8080 -v aster-data:/data \
  -e ASTER_DATA_DIR=/data ghcr.io/cenkcorapci/aster:latest \
  serve --host 0.0.0.0 --port 8080

curl http://localhost:8080/health
```

The container exposes the server only on the Docker host. Use a reverse proxy,
network policy, and an API key before exposing it beyond a trusted network.

### Build from source

For development, install [Bazelisk](https://github.com/bazelbuild/bazelisk)
(recommended) or the version recorded in `.bazelversion`, plus a C++20
compiler:

```bash
git clone https://github.com/cenkcorapci/Aster.git
cd Aster
bazel build -c opt //aster/cli:aster
./bazel-bin/aster/cli/aster --help
```

Use `-c opt` for production and benchmark runs. Debug builds intentionally
trade speed for diagnostics.

## Run your first collection

Start a local HTTP server. `--data-dir` enables durable WAL and SSTable
storage; use a writable directory that is backed up according to your
operational policy.

```bash
aster serve --data-dir "$HOME/.local/share/aster" --port 8080
```

Create a three-dimensional cosine collection, write one document, and search
it:

```bash
curl -X PUT http://localhost:8080/v1/collections/docs \
  -H 'Content-Type: application/json' \
  -d '{"dimension":3,"metric":"cosine"}'

curl -X PUT http://localhost:8080/v1/collections/docs/docs/getting-started \
  -H 'Content-Type: application/json' \
  -d '{"vector":[0.1,0.2,0.3],"metadata":"hello"}'

curl -X POST http://localhost:8080/v1/collections/docs/search \
  -H 'Content-Type: application/json' \
  -d '{"vector":[0.1,0.2,0.3],"top_k":5}'
```

The HTTP API supports collection management, upserts, reads, deletes, search,
flush/compaction operations, health, statistics, and Prometheus metrics. Read
the [HTTP API tutorial](docs/tutorials/http-api.md) before integrating a
service.

## Operate a server

### Durability and lifecycle

A collection accepts writes into a memtable, persists them to the WAL when a
data directory is configured, then flushes immutable SSTable segments with
their vector indexes. Compaction merges segments and removes obsolete
tombstones. The [database management tutorial](docs/tutorials/database-management.md)
explains the durability, flush, and compaction model.

Useful operational endpoints and commands:

| Goal | Command or endpoint |
| --- | --- |
| Check liveness | `GET /health` |
| Inspect collection size and backlog | `GET /v1/collections/<name>/stats` |
| Export Prometheus metrics | `GET /metrics` |
| Run with a configuration file | [server configuration](docs/server-config.md) |
| Start the Thrift server | `aster serve-rpc` |

### Security

Require authentication with `--api-key` or `ASTER_API_KEY`; clients then send
the corresponding API key with requests. Keep Aster behind TLS termination and
limit network access to trusted clients. The server does not make an insecure
deployment safe by itself.

### Search and storage choices

Use cosine metric for normalized embeddings. Aster supports exact search and
segmented HNSW approximate search; `top_k` and `ef_search` are request-level
controls. Storage can use compact vector encodings where their precision and
recall trade-offs fit the workload. See [vector encodings](docs/vector-encoding.md)
and the [API tutorial](docs/tutorials/http-api.md) for the available options.

## Embed Aster in C++

The stable in-process interface is `aster::Db` from `aster/db/db.h`. Downstream
Bazel projects depend on `//aster:embedded_lib`.

```cpp
#include "aster/db/db.h"

aster::Db::Options options;
options.dimension = 384;
options.metric = aster::Metric::kCosine;
options.data_dir = "/var/lib/my-app/aster";

auto opened = aster::Db::Open(options);
if (!opened.ok()) {
  // Handle opened.status().
}
std::unique_ptr<aster::Db> db = std::move(opened.value());
```

The [database management tutorial](docs/tutorials/database-management.md)
documents writes, reads, filters, flushes, compaction, error handling, and
thread-safety. The [versioning policy](docs/versioning.md) defines compatibility
rules for this public API.

## Clients and protocol

The repository includes C++, Python, Go, Rust, Java, Scala, and JavaScript
client facades. Their source-of-truth wire protocol is
[`aster/rpc/aster.thrift`](aster/rpc/aster.thrift). Transport and generated package publishing are planned delivery work. See the
[development plan](docs/development-plan.md), [task board](docs/tasks.md), and
[client libraries](clients/README.md) for the compatibility contract.

## Run benchmarks

All benchmark commands are reproducible harnesses, not production capacity
claims. Record host details, dimensions, corpus size, index parameters, and
result artifacts when comparing runs.

| Workflow | Command | Purpose |
| --- | --- | --- |
| Local kind soak | `make bench-local` | Mixed write/update/search on local disk |
| MinIO soak | `make bench-minio` | Same workload with MinIO object storage |
| Multi-tenant | `make bench-multitenant-smoke` | Catalog quotas across dimensions and sizes |
| Elastic scale | `make bench-scale` | Scale/rebalance workload with local and MinIO backends |
| Milvus comparison | `make bench-vs-milvus-smoke` | Aster vs Milvus, deterministic 2k smoke corpus |
| Qdrant comparison | `make bench-vs-qdrant-smoke` | Aster vs Qdrant, deterministic 2k smoke corpus |
| Full comparison target | `make bench-vs-qdrant` | 100M × 2048 target, automatically scaled to host RAM |

Milvus and Qdrant comparisons share deterministic seed `42`, the same vectors
and queries, HNSW settings, and agreement metrics. Their full target is
100M × 2048 vectors, but the harness reduces the actual corpus and Aster shard
count to fit host and pod memory. Results are saved under `bench-results/`.

- [Comparison harness](deploy/compare-milvus/README.md)
- [Aster vs Milvus measured results](docs/bench-vs-milvus.md)
- [Aster vs Qdrant methodology](docs/bench-vs-qdrant.md)
- [Multi-tenant benchmark](deploy/bench-multitenant/README.md)
- [Elastic-scale benchmark](deploy/bench-scale/README.md)
- [Grafana simulation](deploy/sim-grafana/README.md)

## Develop and test

### Core engine

```bash
bazel test //aster/...             # unit and integration suite
./scripts/run-coverage.sh          # core library coverage gate (>=90%)
./scripts/build-matrix.sh --quick  # host, profiles, and Arduino smoke matrix
```

CI runs the core suite on every push and pull request, plus an ARM64 Edge
profile. Use `./scripts/build-matrix.sh --full` to add cross-platform builds
when the required toolchains are available.

### Comparison harness

The comparison driver is separately testable without Docker, kind, or a live
vector database. Create a Python environment, install its dependencies, then
run the offline suite:

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r deploy/compare-milvus/driver/requirements.txt
make bench-vs-test
make install-test
```

`bench-vs-test` covers deterministic workloads, agreement metrics, Aster HTTP
request construction and shard routing, report rendering, the Milvus/Qdrant
driver paths, and Qdrant request mapping. `install-test` simulates the
one-line installer, including its Bazelisk fallback, without downloading or
building a real release.

### Repository layout

| Path | Contents |
| --- | --- |
| `aster/` | Core database, storage, index, RPC, CLI, and embedded targets |
| `clients/` | Language client facades and wire conformance corpus |
| `deploy/` | kind, MinIO, comparison, scale, and simulation harnesses |
| `docs/` | Tutorials, operational guides, architecture, and roadmap |
| `tla/` | TLA+ specifications |
| `scripts/` | Build, coverage, image, release, and installer tooling |

## Documentation map

| Need | Start here |
| --- | --- |
| HTTP endpoints and examples | [HTTP API tutorial](docs/tutorials/http-api.md) |
| C++ embedding, durability, and compaction | [Database management](docs/tutorials/database-management.md) |
| Server environment and configuration | [Server configuration](docs/server-config.md) |
| System architecture | [Code structure](docs/code-structure.md) |
| Vector precision/storage trade-offs | [Vector encodings](docs/vector-encoding.md) |
| Client support and protocol compatibility | [Client libraries](clients/README.md) |
| Compatibility and release rules | [Versioning](docs/versioning.md) |
| Milestone status and gates | [Development plan](docs/development-plan.md) |
| Claimable engineering work | [Task board](docs/tasks.md) |

## Contributing and license

Read the [development plan](docs/development-plan.md) and
[contributor protocol](docs/start-the-tasks.md), keep changes covered by the
relevant test target, and run the narrowest applicable checks before opening a
pull request. Aster is licensed under [AGPL-3.0](LICENSE).
