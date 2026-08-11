# Aster client libraries

One client per language, all in this monorepo, all speaking
[`//aster/rpc:aster.thrift`](../aster/rpc/aster.thrift). The IDL is the
source of truth for the wire protocol.

**Status:** public facades exist; transport + codegen = milestone **M5**.
Hands-on guide: [docs/tutorials/client-libraries.md](../docs/tutorials/client-libraries.md).
For real data today, embed the engine via Bazel
`//aster:embedded_lib` (`aster::Db` — see
[docs/code-structure.md](../docs/code-structure.md) § Depending on the
embedded library) and
[docs/tutorials/database-management.md](../docs/tutorials/database-management.md).


| Directory | Package | Registry | Bazel ruleset |
| --- | --- | --- | --- |
| `cpp/` | `aster-client` | vendored | `rules_cc` |
| `python/` | `aster-client` | PyPI | `rules_python` (M6) |
| `go/` | `…/clients/go` | Go module proxy | `rules_go` (M6) |
| `rust/` | `aster-client` | crates.io | `rules_rust` (M6) |
| `java/` | `io.aster:aster-client` | Maven Central | `rules_jvm_external` (M6) |
| `scala/` | `io.aster:aster-client-scala` | Maven Central | `rules_scala` (M6) |
| `javascript/` | `@aster-db/client` | npm | `aspect_rules_js` (M6) |

## Design contract

1. **Transport** — framed TCP + optional TLS, pooling, retries, seed failover
2. **Generated protocol** — Thrift types/stubs from the IDL
3. **Idiomatic facade** — `connect` → `collection` → upsert / get / delete / search

Vectors use native language types. `ef_search`, `top_k`, tags, and
consistency are per-call options. Errors carry `AsterError.code`.

## Build & release

Until M6, each directory has a native package file and a Bazel
`filegroup` placeholder. A single `vX.Y.Z` tag publishes all clients —
see [docs/versioning.md](../docs/versioning.md) and
[docs/development-plan.md](../docs/development-plan.md) (M5–M6).
