# Changelog

All notable changes to Aster (server and clients) are documented in this file.
The project follows a single-version policy; see [docs/versioning.md](docs/versioning.md).

## [Unreleased]

### Stabilized
- Embedded `aster::Db` public API (`aster/db/db.h`) is the in-process contract.
  Version macros live in `aster/core/version.h` (`ASTER_VERSION_*`, synced with
  `VERSION`). Breaking changes to that surface require a MAJOR bump and a note
  here; see [docs/versioning.md](docs/versioning.md) § Embedded C++ API.

## [0.1.0] - 2026-08-07
- Claim wave-1 parallel tasks: M1-T01, M3-T05, M4-T06, M6-T07.
- Complete M0 foundation: Bazel C++ engine, clients, TLA+, task board.
- Design docs.
- Initial commit

