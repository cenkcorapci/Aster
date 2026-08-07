# Aster client libraries

One client per language, all living in this monorepo, all speaking the same
Thrift protocol defined in [`//aster/rpc:aster.thrift`](../aster/rpc/aster.thrift).
The IDL is the single source of truth: request/response types are generated,
never hand-written, so clients cannot drift from the server.

| Directory     | Package name        | Registry        | Bazel ruleset        |
| ------------- | ------------------- | --------------- | -------------------- |
| `cpp/`        | `aster-client`      | (vendored/conan)| `rules_cc` (active)  |
| `python/`     | `aster-client`      | PyPI            | `rules_python`       |
| `go/`         | `github.com/aster-db/aster/clients/go` | Go module proxy | `rules_go` |
| `rust/`       | `aster-client`      | crates.io       | `rules_rust`         |
| `java/`       | `io.aster:aster-client` | Maven Central | `rules_jvm_external` |
| `scala/`      | `io.aster:aster-client-scala` | Maven Central | `rules_scala` |
| `javascript/` | `@aster-db/client`  | npm             | `aspect_rules_js`    |

## Design contract (all languages)

Every client implements the same layered API:

1. **Transport** — framed TCP + optional TLS, connection pooling, retries
   with exponential backoff, coordinator failover across seed nodes.
2. **Generated protocol** — Thrift-generated types and service stubs.
3. **Idiomatic facade** — the thin hand-written layer users actually touch:
   `connect`, `collection`, `upsert`, `get`, `delete`, `search`. Async-first
   where the language has a native async story (Python asyncio, JS promises,
   Rust tokio, Scala Future), sync elsewhere.

Facade rules:

- Vectors are accepted in the language's native form (`std::span<float>`,
  numpy array / list, `[]float32`, `Vec<f32>`, `float[]`, `Array[Float]`,
  `Float32Array`) and encoded to the wire format internally.
- `ef_search`, `top_k`, tags and consistency level are per-call options with
  collection-level defaults, mirroring `docs/client-api.md`.
- Errors map to idiomatic mechanisms (exceptions, `error`, `Result`) but
  always carry the server's `AsterError.code`.

## Build & release pipeline

All packages are built by Bazel and released from CI in this repo
(milestones M5–M6 in `docs/development-plan.md`):

```
tag vX.Y.Z
  └─ CI: bazel test //...
       ├─ bazel build //clients/python:wheel        → twine upload (PyPI)
       ├─ bazel build //clients/rust:crate          → cargo publish
       ├─ bazel build //clients/java:deploy         → Maven Central (Sonatype)
       ├─ bazel build //clients/scala:deploy        → Maven Central
       ├─ bazel build //clients/javascript:npm_pkg  → npm publish
       └─ go: module proxy picks up the git tag directly
```

All clients share one version number, tagged at the repo root. The language
rulesets (`rules_python`, `rules_go`, `rules_rust`, `rules_jvm_external`,
`rules_scala`, `aspect_rules_js`) are added to `MODULE.bazel` when M6
starts; until then each directory carries an idiomatic native build file so
the packages are developable standalone, and the Bazel `BUILD.bazel` files
export sources via `filegroup` placeholders.
