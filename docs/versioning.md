# Versioning policy

Aster uses a **single version** for the server binary, Bazel module, and every
client package in this monorepo. There is no independent per-language
semver: a release is one `vX.Y.Z` that moves all artifacts together.

## Source of truth

| Artifact | Location |
| --- | --- |
| Canonical version | [`VERSION`](../VERSION) at the repo root (`X.Y.Z`, no `v` prefix) |
| C++ macros | `ASTER_VERSION_MAJOR` / `MINOR` / `PATCH` / `STRING` in [`aster/core/version.h`](../aster/core/version.h) |
| Bazel module | `module(version = …)` in [`MODULE.bazel`](../MODULE.bazel) |
| Python | `version` in [`clients/python/pyproject.toml`](../clients/python/pyproject.toml) |
| Rust | `version` in [`clients/rust/Cargo.toml`](../clients/rust/Cargo.toml) |
| JavaScript | `version` in [`clients/javascript/package.json`](../clients/javascript/package.json) |
| Go | module version is the git tag (`vX.Y.Z`); no separate file bump |
| Java / Scala / C++ client | follow the same release tag; packaging metadata lands with M6 publish jobs |

`scripts/bump-version.sh` keeps `VERSION`, `aster/core/version.h`, and the known
package fields above in sync. Do not edit those fields by hand unless you also
update `VERSION`.

## Tag format

- Annotated tags only: `vX.Y.Z` (leading `v`, semver, no other suffixes on
  stable releases).
- Pre-releases: `vX.Y.Z-rc.N` or `vX.Y.Z-rcN` (same string in `VERSION`
  without the leading `v`, e.g. `0.1.0-rc.1`).
- Do not retag; ship a new patch or rc instead.

## Semver (product-level)

- **MAJOR** — breaking wire protocol, on-disk format, client API, or
  embedded `aster::Db` public surface (see below).
- **MINOR** — backward-compatible features.
- **PATCH** — bug fixes and non-breaking docs/tooling.

Clients and server share the major: a `1.x` client talks to a `1.x` server.
Cross-major compatibility is not promised.

## Embedded C++ API (`aster::Db`)

[`aster/db/db.h`](../aster/db/db.h) is the **public contract** for in-process /
embedded use (options, `Open`, `Upsert` / `Delete` / `Get` / `Search`,
`Flush` / `Compact`, and the documented inspectors). Private members and
`.cc` internals are not part of the contract.

| Concern | Rule |
| --- | --- |
| Where the version lives | `VERSION` + `ASTER_VERSION_*` macros in `aster/core/version.h` (included by `db.h`) |
| Additive API | Prefer new overloads / optional fields / defaulted options; ship as **MINOR** |
| Breaking API | Rename/remove symbols, change defaults that alter semantics, or tighten preconditions → **MAJOR** + note |
| How breaks are announced | Entry under the next version in [`CHANGELOG.md`](../CHANGELOG.md), and a short callout in this file when the major bumps |

MCU / Tiny embeds use `aster::embedded::Db` (`aster/embedded/db.h`); that
surface follows the same semver product version but is a separate, smaller
API (no WAL / SSTable).

## Release steps

1. Ensure `main` is green (`bazel test //aster/...` and any client checks
   required for the milestone).
2. Decide the next version (`X.Y.Z` or pre-release).
3. Bump all package fields from the repo root:

   ```bash
   ./scripts/bump-version.sh X.Y.Z
   ```

4. Generate / refresh the changelog section for that version:

   ```bash
   ./scripts/generate-changelog.sh X.Y.Z
   ```

   Or omit the argument to draft under `## [Unreleased]` from commits since
   the latest `v*` tag.

5. Review `CHANGELOG.md`, commit the bump + changelog (message should name
   the version), and open a PR if not releasing from `main` directly.
6. After merge, create an annotated tag and push it:

   ```bash
   git tag -a "vX.Y.Z" -m "Aster vX.Y.Z"
   git push origin "vX.Y.Z"
   ```

7. CI (M6 pipeline) publishes artifacts from that tag. Go modules are
   consumed via the same `vX.Y.Z` tag on the default branch history.

## Scripts

| Script | Purpose |
| --- | --- |
| [`scripts/bump-version.sh`](../scripts/bump-version.sh) | Write `VERSION`, `aster/core/version.h`, and sync known package version fields |
| [`scripts/generate-changelog.sh`](../scripts/generate-changelog.sh) | Append or refresh a `CHANGELOG.md` section from `git log` |

Both scripts are idempotent: re-running with the same version refreshes the
same targets without duplicating changelog sections. Use
`./scripts/bump-version.sh --dry-run X.Y.Z` to print planned edits without
writing files.
