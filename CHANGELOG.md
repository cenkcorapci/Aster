# Changelog

All notable changes to Aster (server and clients) are documented in this file.
The project follows a single-version policy; see [docs/versioning.md](docs/versioning.md).

## [Unreleased]

### Added
- Python client HTTP transport (`clients/python`, `aster-client` 0.2.0):
  `create_collection`, `upsert`, `search(top_k, ef_search, tags)`, `get`,
  `delete`, `health`, `stats`, with live-server tests.
- `GET /v1/collections/{name}/stats` (vectors, segments, memtable rows,
  write memory, compaction backlog); `GET /v1/collections/{name}/docs/{id}`
  now returns `metadata`.
- `/metrics` is Prometheus-conformant (`promtool check metrics` clean):
  `aster_` prefix, counters `*_total`, latency histograms in seconds,
  `aster_build_info{version}`. **Breaking:** old unprefixed names
  (`read_latency_ms`, `searches_total`, `segment_count`, ...) are gone from
  `/metrics`; `deploy/sim-grafana` dashboard updated.
- `compaction_backlog` gauge is now populated (segments above the tier
  threshold); `gets_total` is now incremented.
- GHCR multi-arch image workflow (`.github/workflows/publish-image.yml`).

- M2-T10: HNSW recall@10 CI gate (`//aster/qa:recall_gate_test`, nightly
  `//aster/qa:recall_gate_nightly_test`) — SIFT/GloVe-like subsets must
  reach ≥ 0.95 @ ef=128 vs exact; workflows `.github/workflows/recall-gate.yml`
  and `recall-nightly.yml`.
- Installable embedded library target `//aster:embedded_lib` — single
  `cc_library` for downstream in-process deps (re-exports `//aster/db`).
  See [docs/code-structure.md](docs/code-structure.md).

### Fixed
- HTTP JSON reader now unescapes `\n \r \t \uXXXX` (previously `\n` became `n`);
  `json::Escape` escapes control characters.
- HTTP API percent-decodes path segments (document ids with spaces, `%`, UTF-8).

### Stabilized
- Embedded `aster::Db` public API (`aster/db/db.h`) is the in-process contract.
  Version macros live in `aster/core/version.h` (`ASTER_VERSION_*`, synced with
  `VERSION`). Breaking changes to that surface require a MAJOR bump and a note
  here; see [docs/versioning.md](docs/versioning.md) § Embedded C++ API.

## [0.1.0] - 2026-08-07
- M1-T14: kill-9 crash-recovery fuzz (`//aster/qa:kill9_fuzz_test`) + laptop write bench (`//aster/qa:write_bench`, `docs/bench-write.md`).
- Claim wave-1 parallel tasks: M1-T01, M3-T05, M4-T06, M6-T07.
- Complete M0 foundation: Bazel C++ engine, clients, TLA+, task board.
- Design docs.
- Initial commit

