"""Compile Aster Tiny sources from the monorepo into this PlatformIO firmware."""

from pathlib import Path

Import("env")  # noqa: F821 — provided by PlatformIO

ROOT = Path(env["PROJECT_DIR"]).resolve().parents[1]
BUILD = Path(env.subst("$BUILD_DIR")) / "aster_tiny"
BUILD.mkdir(parents=True, exist_ok=True)

ASTER_SOURCES = [
    "aster/embedded/db.cc",
    "aster/core/hash.cc",
    "aster/index/bloom.cc",
    "aster/index/distance.cc",
    "aster/index/exact_index.cc",
    "aster/query/topk.cc",
    "aster/storage/memtable.cc",
    "aster/storage/segment.cc",
    "aster/platform/memory_storage.cc",
]

objs = []
for rel in ASTER_SOURCES:
    src = ROOT / rel
    if not src.is_file():
        raise SystemExit(f"sim-arduino: missing Aster source: {src}")
    target = BUILD / (rel.replace("/", "_") + ".o")
    objs.append(env.Object(target=str(target), source=str(src)))

# Link the compiled Aster objects into the firmware.
env.Append(LIBS=objs)
