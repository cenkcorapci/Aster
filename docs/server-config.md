# Server TOML config (M4-T05)

`aster/cli:aster` supports an optional `--config PATH` flag for both:

- `serve`
- `serve-rpc`

When `--config` is provided, TOML fields override defaults and are used to
configure the catalog (WAL sync + compaction knobs) and the server bind
address.

## File format

TOML is parsed as a strict subset:

- Supported tables: `[server]`, `[catalog]`
- Supported keys:
  - `[server]`
    - `host` (string)
    - `port` (integer, 0..65535)
  - `[catalog]`
    - `data_dir` (string, required for durable mode)
    - `wal_sync` (string: `"always"`, `"every_ms"`, `"never"`)
    - `memtable_flush_bytes` (integer, > 0)
    - `compaction_tier_threshold` (integer, > 0)
    - `max_segments_before_compact` (integer, > 0)

Unknown tables/keys or malformed TOML produce a line-numbered error like:
`path/to/config.toml:12: unknown key '...' in section [server]`.

## Precedence rules

For each knob, precedence is:

1) explicit CLI flags (e.g. `--host`, `--port`, `--data-dir`)
2) `--config` TOML
3) environment defaults (`ASTER_DATA_DIR`, etc.)
4) built-in defaults (host/port defaults differ per command)

## Example

```toml
[server]
host = "127.0.0.1"
port = 9090

[catalog]
data_dir = "/data"
wal_sync = "every_ms"
memtable_flush_bytes = 67108864
compaction_tier_threshold = 4
max_segments_before_compact = 8
```

