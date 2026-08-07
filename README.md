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
```

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
