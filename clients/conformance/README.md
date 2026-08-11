# Conformance corpus (M5-T03)

Language-agnostic YAML cases that every Aster client must pass against a
live server. Cases live in `corpus/` and map ops directly to
[`aster/rpc/aster.thrift`](../../aster/rpc/aster.thrift).

## Run

```bash
bazel test //clients/conformance:yaml_corpus_test
```

## Fixture

Until `//clients/cpp` grows a real transport (M5-T04), the Bazel test:

1. Opens an ephemeral-port Thrift server (`//aster/rpc:thrift_server`, M4-T02)
2. Speaks framed binary Thrift via generated `AsterClient`
3. Executes every `corpus/*.yaml` file

Later language clients (M5-T05…T10) should reuse the same YAML corpus
against their own transports; CI aggregation is M5-T11.

## Corpus schema

Top-level keys: `name`, optional `description`, and `steps` (sequence).

Each step has `op` plus op-specific fields. Supported ops:

| `op` | Fields |
| --- | --- |
| `createCollection` | `name` |
| `configureCollection` | `name`, `dimension`, `metric` (`L2`/`DOT`/`COSINE`) |
| `dropCollection` | `name` |
| `upsert` | `collection`, `id`, `vector`, optional `tags`, `consistency`, `timestampMicros` |
| `get` | `collection`, `id`, optional `consistency`, `expect` / `expect_error` |
| `remove` | `collection`, `id`, optional `consistency` |
| `search` | `collection`, `vector`, optional `topK`, `efSearch`, `tags`, `consistency`, `expect` |

Expectations:

- `expect_error: true` — step must throw `AsterError`
- `expect.id` / `expect.tags` — for `get`
- `expect.first_id` / `expect.min_score` — for `search`
