#!/usr/bin/env python3
"""Aster vs Milvus compare driver: load shared corpus, search, metricize."""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from typing import List

import numpy as np

from aster_client import AsterCluster
from metrics import aggregate, latency_stats, summarize_query_pair
from milvus_client import MilvusCluster
from report import build_verdict, write_report
from workload import iter_batches, make_queries


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Aster vs Milvus comparison driver")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--aster-urls", required=True, help="comma-separated shard base URLs")
    p.add_argument("--milvus-uri", required=True, help="e.g. http://127.0.0.1:19530")
    p.add_argument("--vectors", type=int, required=True)
    p.add_argument("--dimension", type=int, default=2048)
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--queries", type=int, default=50)
    p.add_argument("--top-k", type=int, default=10)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--ef", type=int, default=128, help="Milvus HNSW ef search")
    p.add_argument("--target-vectors", type=int, default=0)
    p.add_argument("--host-ram-gb", type=float, default=0)
    p.add_argument("--usable-gb", type=float, default=0)
    p.add_argument("--scaled", action="store_true")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    os.makedirs(args.out_dir, exist_ok=True)

    shard_urls = [u.strip() for u in args.aster_urls.split(",") if u.strip()]
    if not shard_urls:
        print("no aster shard urls", file=sys.stderr)
        return 2

    scale = {
        "target_vectors": args.target_vectors or args.vectors,
        "actual_vectors": args.vectors,
        "dimension": args.dimension,
        "aster_shards": len(shard_urls),
        "top_k": args.top_k,
        "queries": args.queries,
        "seed": args.seed,
        "host_ram_gb": args.host_ram_gb,
        "usable_gb": args.usable_gb,
        "scaled": bool(args.scaled),
        "bytes_per_vector": args.dimension * 4,
        "raw_corpus_gb": (args.vectors * args.dimension * 4) / (1024**3),
    }
    with open(os.path.join(args.out_dir, "scale.json"), "w", encoding="utf-8") as f:
        json.dump(scale, f, indent=2)

    print(f"==> connecting Aster shards ({len(shard_urls)})", flush=True)
    aster = AsterCluster(shard_urls)
    aster.wait_all()
    aster.ensure_collections(args.dimension)

    print(f"==> connecting Milvus at {args.milvus_uri}", flush=True)
    milvus = MilvusCluster(
        uri=args.milvus_uri,
        dim=args.dimension,
        ef=args.ef,
    )
    milvus.connect()
    milvus.ensure_collection()

    print(f"==> loading {args.vectors} × {args.dimension}-d vectors", flush=True)
    milvus_ids: List[str] = []
    milvus_rows: List[List[List[float]]] = []

    t_aster_load = 0.0
    n_loaded = 0
    for ids, mat in iter_batches(args.vectors, args.dimension, args.batch_size, args.seed):
        indices = [int(x.split("-", 1)[1]) for x in ids]
        t0 = time.perf_counter()
        aster.upsert_routed(ids, mat, indices)
        t_aster_load += time.perf_counter() - t0
        milvus_ids.extend(ids)
        milvus_rows.append(mat)
        n_loaded += len(ids)
        if n_loaded % max(args.batch_size * 10, 1) == 0 or n_loaded == args.vectors:
            print(f"    aster upserted {n_loaded}/{args.vectors}", flush=True)

    t_flush0 = time.perf_counter()
    aster.flush_all()
    aster_flush = time.perf_counter() - t_flush0
    aster_load = {
        "vectors": n_loaded,
        "load_seconds": t_aster_load,
        "flush_seconds": aster_flush,
        "vectors_per_sec": n_loaded / t_aster_load if t_aster_load > 0 else 0.0,
    }

    flat: List[List[float]] = [row for batch in milvus_rows for row in batch]
    all_mat = (
        np.asarray(flat, dtype=np.float32)
        if flat
        else np.zeros((0, args.dimension), dtype=np.float32)
    )
    print("==> Milvus insert + index", flush=True)
    chunk = max(args.batch_size, 256)
    col = milvus._col
    assert col is not None
    t_ins = 0.0
    for i in range(0, len(milvus_ids), chunk):
        sl_ids = milvus_ids[i : i + chunk]
        sl_vecs = all_mat[i : i + chunk]
        t0 = time.perf_counter()
        col.insert([list(sl_ids), sl_vecs.tolist()])
        t_ins += time.perf_counter() - t0
        if (i // chunk) % 10 == 0:
            print(
                f"    milvus inserted {min(i + chunk, len(milvus_ids))}/{len(milvus_ids)}",
                flush=True,
            )
    t0 = time.perf_counter()
    col.flush()
    flush_s = time.perf_counter() - t0
    index_params = {
        "index_type": "HNSW",
        "metric_type": "IP",
        "params": {"M": 16, "efConstruction": 200},
    }
    t0 = time.perf_counter()
    col.create_index(field_name="vector", index_params=index_params)
    index_s = time.perf_counter() - t0
    t0 = time.perf_counter()
    col.load()
    load_s = time.perf_counter() - t0
    milvus_stats = {
        "vectors": len(milvus_ids),
        "insert_seconds": t_ins,
        "flush_seconds": flush_s,
        "index_seconds": index_s,
        "load_seconds": load_s,
        "vectors_per_sec": len(milvus_ids) / t_ins if t_ins > 0 else 0.0,
    }

    with open(os.path.join(args.out_dir, "aster_load.json"), "w", encoding="utf-8") as f:
        json.dump(aster_load, f, indent=2)
    with open(os.path.join(args.out_dir, "milvus_load.json"), "w", encoding="utf-8") as f:
        json.dump(milvus_stats, f, indent=2)

    print(f"==> running {args.queries} queries top_k={args.top_k}", flush=True)
    queries = make_queries(args.dimension, args.queries, base_seed=args.seed)
    aster_lat: List[float] = []
    milvus_lat: List[float] = []
    per_query = []
    detailed = []

    for qi in range(args.queries):
        q = queries[qi]
        a_hits, a_ms = aster.search(q, args.top_k)
        m_hits, m_ms = milvus.search(q, args.top_k)
        aster_lat.append(a_ms)
        milvus_lat.append(m_ms)
        summary = summarize_query_pair(a_hits, m_hits, args.top_k)
        per_query.append(summary)
        detailed.append(
            {
                "query": qi,
                "aster_ms": a_ms,
                "milvus_ms": m_ms,
                "aster_hits": [{"id": i, "score": s} for i, s in a_hits],
                "milvus_hits": [{"id": i, "score": s} for i, s in m_hits],
                "metrics": summary,
            }
        )
        if (qi + 1) % 10 == 0 or qi == 0:
            print(
                f"    q{qi}: jaccard={summary['jaccard_at_k']:.3f} "
                f"recall_m={summary['milvus_recall_vs_aster']:.3f} "
                f"aster={a_ms:.1f}ms milvus={m_ms:.1f}ms",
                flush=True,
            )

    accuracy = aggregate(per_query)
    report = {
        "scale": scale,
        "aster_load": aster_load,
        "milvus_load": milvus_stats,
        "aster_search": latency_stats(aster_lat),
        "milvus_search": latency_stats(milvus_lat),
        "accuracy": accuracy,
        "per_query": detailed,
        "milvus_index": {"type": "HNSW", "metric": "IP", "ef": args.ef, "M": 16},
        "aster_search_mode": "exact_scatter_gather",
    }
    report["verdict"] = build_verdict(report)
    write_report(report, args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
