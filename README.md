# Aster

A peer-to-peer vector database designed to be easy to use, easy to tune,
reliable, fast, and frugal: Cassandra-style decentralized clustering,
LSM-tree storage with immutable per-segment HNSW indexes, CPU-only search,
and deployment targets from microcontrollers to S3-backed cloud clusters.

## Repository layout (Bazel monorepo)

| Path | Contents |
| --- | --- |
| `aster/` | C++20 core engine: `core`, `storage` (WAL/memtable/segments), `index` (distance kernels, vector indexes), `distributed` (consistent-hash ring), `query`, `db` (single-node engine), `rpc` (Thrift IDL), `platform`, `metrics`, `cli` |
| `clients/` | Client libraries for C++, Python, Go, Rust, Java, Scala, JavaScript — one protocol, one release train ([clients/README.md](clients/README.md)) |
| `docs/` | Design & architecture docs, incl. the [development plan](docs/development-plan.md), [task board](docs/tasks.md), [parallel-agent playbook](docs/start-the-tasks.md), and [indexing reference](docs/indexing.md) |
| `tla/` | TLA+ formal specifications of the indexing lifecycle and replication protocol, model-checked with TLC ([tla/README.md](tla/README.md)) |

## Quick start

Requires [Bazel](https://bazel.build) (see `.bazelversion`) and a C++20
compiler.

```bash
bazel test //aster/...        # unit + integration tests
bazel run //aster/cli:aster   # single-node demo: insert, search, compact
./scripts/run-coverage.sh     # LLVM LCOV report; gates >=90% on aster libs
./scripts/docker-build.sh     # static musl binary + BusyBox image (aster:local)
docker run --rm -v aster-data:/data aster:local
./scripts/build-matrix.sh     # host + Tiny/Edge/Server + Arduino + musl
./scripts/build-matrix.sh --full   # also zig-cross Linux/Darwin
```

## Target platforms

Profiles (`--config=tiny|edge|server`) and CPU/OS platforms are orthogonal.

| Target | Config / command |
| --- | --- |
| Apple Silicon Mac | default, or `--config=apple_silicon` |
| Intel Mac | `--config=apple_intel` or `--config=zig_macos_amd64` (cross) |
| Linux x86_64 / arm64 | `--config=linux_amd64` / `linux_arm64`, or `zig_linux_*` |
| Raspberry Pi (Edge) | `--config=raspberry_pi` |
| BusyBox / scratch | `--config=linux_musl_arm64` (see `./scripts/docker-build.sh`) |
| Arduino / ESP32 / MCU | `--config=arduino //aster/embedded` → `libembedded.a` |

Arduino builds the Tiny in-memory engine only (no POSIX disk/WAL). Link
`bazel-bin/aster/embedded/libembedded.a` from PlatformIO / ESP-IDF.

Model-check the formal specs (requires Java 11+):

```bash
cd tla
curl -LO https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar
java -jar tla2tools.jar -workers auto AsterLsmIndex.tla
java -jar tla2tools.jar -workers auto AsterReplication.tla
```

## Status

Pre-alpha. Milestone M0 (project skeleton, single-node in-memory engine,
formal specs) is complete; see the
[development plan](docs/development-plan.md) for the roadmap.

## License

AGPL-3.0 — see [LICENSE](LICENSE).
