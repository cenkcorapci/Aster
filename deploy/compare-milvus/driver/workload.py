"""Deterministic shared workload for Aster vs Milvus comparison."""

from __future__ import annotations

import hashlib
import math
import random
import struct
from typing import Iterator, List, Sequence, Tuple


def _seed_for(base_seed: int, index: int) -> int:
    h = hashlib.sha256(struct.pack("<QQ", base_seed & 0xFFFFFFFFFFFFFFFF, index)).digest()
    return int.from_bytes(h[:8], "little")


def make_vector(dim: int, index: int, base_seed: int = 42) -> List[float]:
    """Unit-norm float32 vector for cosine / normalized-IP search."""
    rng = random.Random(_seed_for(base_seed, index))
    v = [rng.gauss(0.0, 1.0) for _ in range(dim)]
    n = math.sqrt(sum(x * x for x in v))
    if n < 1e-12:
        v[0] = 1.0
        n = 1.0
    return [x / n for x in v]


def make_vectors_batch(
    dim: int, start: int, count: int, base_seed: int = 42
) -> Tuple[List[str], List[List[float]]]:
    ids = [f"doc-{start + i}" for i in range(count)]
    mat = [make_vector(dim, start + i, base_seed) for i in range(count)]
    return ids, mat


def iter_batches(
    total: int, dim: int, batch_size: int, base_seed: int = 42
) -> Iterator[Tuple[List[str], List[List[float]]]]:
    start = 0
    while start < total:
        n = min(batch_size, total - start)
        yield make_vectors_batch(dim, start, n, base_seed)
        start += n


def make_queries(
    dim: int, count: int, base_seed: int = 42, query_seed: int = 9001
) -> List[List[float]]:
    """Queries drawn from a different seed so they are not exact corpus rows."""
    _ = base_seed  # reserved for future correlated queries
    return [make_vector(dim, i, query_seed) for i in range(count)]


def shard_of(doc_index: int, shards: int) -> int:
    return doc_index % shards


def merge_topk(
    shard_hits: List[List[Tuple[str, float]]], k: int
) -> List[Tuple[str, float]]:
    """Merge per-shard (id, score) lists; higher score is better (cosine)."""
    merged: dict[str, float] = {}
    for hits in shard_hits:
        for doc_id, score in hits:
            prev = merged.get(doc_id)
            if prev is None or score > prev:
                merged[doc_id] = score
    ranked = sorted(merged.items(), key=lambda x: (-x[1], x[0]))
    return ranked[:k]
