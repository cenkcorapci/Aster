#!/usr/bin/env python3
"""Aster vs vector database comparison driver: load shared corpus and metricize."""

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
from qdrant_backend import QdrantCluster
from report import build_verdict, write_report
from workload import iter_batches, make_queries


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Aster vector database comparison driver")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--aster-urls", required=True, help="comma-separated shard base URLs")
    p.add_argument("--backend", choices=("milvus", "qdrant"), required=True)
    p.add_argument("--competitor-uri", required=True)
    p.add_argument("--vectors", type=int, required=True)
    p.add_argument("--dimension", type=int, default=2048)
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--queries", type=int, default=50)
    p.add_argument("--top-k", type=int, default=10)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--ef", type=int, default=128, help="HNSW ef search (both sides)")
    p.add_argument("--hnsw-m", type=int, default=16, help="HNSW M (both sides)")
    p.add_argument(
        "--hnsw-ef-construction",
        type=int,
        default=200,
        help="HNSW efConstruction (both sides)",
    )
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

    hnsw_index = {
        "type": "HNSW",
        "metric": "IP",
        "M": args.hnsw_m,
        "efConstruction": args.hnsw_ef_construction,
        "ef": args.ef,
    }

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
        "hnsw": hnsw_index,
    }
    with open(os.path.join(args.out_dir, "scale.json"), "w", encoding="utf-8") as f:
        json.dump(scale, f, indent=2)

    print(f"==> connecting Aster shards ({len(shard_urls)})", flush=True)
    aster = AsterCluster(shard_urls)
    aster.wait_all()
    aster.ensure_collections(
        args.dimension,
        hnsw_m=args.hnsw_m,
        hnsw_ef_construction=args.hnsw_ef_construction,
        hnsw_ef_search=args.ef,
    )

    print(f"==> connecting {args.backend.title()} at {args.competitor_uri}", flush=True)
    if args.backend == "milvus":
        competitor = MilvusCluster(
            uri=args.competitor_uri,
            dim=args.dimension,
            m=args.hnsw_m,
            ef_construction=args.hnsw_ef_construction,
            ef=args.ef,
        )
    else:
        competitor = QdrantCluster(
            url=args.competitor_uri,
            dim=args.dimension,
            m=args.hnsw_m,
            ef_construction=args.hnsw_ef_construction,
            ef=args.ef,
        )
    competitor.connect()
    competitor.ensure_collection()

    print(f"==> loading {args.vectors} × {args.dimension}-d vectors", flush=True)
    competitor_ids: List[str] = []
    competitor_rows: List[List[List[float]]] = []

    t_aster_load = 0.0
    n_loaded = 0
    # Periodic flush only on large corpora; small loads finish before memtables dominate.
    flush_every = 0
    if args.vectors >= 50000:
        flush_every = max(args.batch_size * 400, 25000)
    for ids, mat in iter_batches(args.vectors, args.dimension, args.batch_size, args.seed):
        indices = [int(x.split("-", 1)[1]) for x in ids]
        t0 = time.perf_counter()
        aster.upsert_routed(ids, mat, indices)
        t_aster_load += time.perf_counter() - t0
        competitor_ids.extend(ids)
        competitor_rows.append(mat)
        n_loaded += len(ids)
        if n_loaded % max(args.batch_size * 10, 1) == 0 or n_loaded == args.vectors:
            print(f"    aster upserted {n_loaded}/{args.vectors}", flush=True)
        # Periodic flush keeps memtables under pod memory limits on kind.
        if flush_every and n_loaded % flush_every == 0 and n_loaded < args.vectors:
            aster.flush_all()
            print(f"    aster mid-load flush at {n_loaded}", flush=True)

    t_flush0 = time.perf_counter()
    flush_stats = aster.flush_all()
    aster_flush = time.perf_counter() - t_flush0
    aster_load = {
        "vectors": n_loaded,
        "load_seconds": t_aster_load,
        "flush_seconds": aster_flush,
        "memtable_flush_seconds": flush_stats["memtable_flush_seconds"],
        "index_build_seconds": flush_stats["index_build_seconds"],
        "vectors_per_sec": n_loaded / t_aster_load if t_aster_load > 0 else 0.0,
    }

    flat: List[List[float]] = [row for batch in competitor_rows for row in batch]
    all_mat = (
        np.asarray(flat, dtype=np.float32)
        if flat
        else np.zeros((0, args.dimension), dtype=np.float32)
    )
    print(f"==> {args.backend.title()} insert + index", flush=True)
    chunk = max(args.batch_size, 256)
    competitor_stats = competitor.load(competitor_ids, all_mat, batch_size=chunk)

    with open(os.path.join(args.out_dir, "aster_load.json"), "w", encoding="utf-8") as f:
        json.dump(aster_load, f, indent=2)
    with open(os.path.join(args.out_dir, f"{args.backend}_load.json"), "w", encoding="utf-8") as f:
        json.dump(competitor_stats, f, indent=2)

    print(f"==> running {args.queries} queries top_k={args.top_k}", flush=True)
    queries = make_queries(args.dimension, args.queries, base_seed=args.seed)
    aster_lat: List[float] = []
    competitor_lat: List[float] = []
    per_query = []
    detailed = []

    for qi in range(args.queries):
        q = queries[qi]
        a_hits, a_ms = aster.search(q, args.top_k, ef_search=args.ef)
        m_hits, m_ms = competitor.search(q, args.top_k)
        aster_lat.append(a_ms)
        competitor_lat.append(m_ms)
        summary = summarize_query_pair(a_hits, m_hits, args.top_k)
        per_query.append(summary)
        detailed.append(
            {
                "query": qi,
                "aster_ms": a_ms,
                "competitor_ms": m_ms,
                "aster_hits": [{"id": i, "score": s} for i, s in a_hits],
                "competitor_hits": [{"id": i, "score": s} for i, s in m_hits],
                "metrics": summary,
            }
        )
        if (qi + 1) % 10 == 0 or qi == 0:
            print(
                f"    q{qi}: jaccard={summary['jaccard_at_k']:.3f} "
                f"recall_{args.backend}={summary['milvus_recall_vs_aster']:.3f} "
                f"aster={a_ms:.1f}ms {args.backend}={m_ms:.1f}ms",
                flush=True,
            )

    accuracy = aggregate(per_query)
    accuracy["mean_competitor_recall_vs_aster"] = accuracy[
        "mean_milvus_recall_vs_aster"
    ]
    report = {
        "scale": scale,
        "aster_load": aster_load,
        "competitor": args.backend,
        "competitor_load": competitor_stats,
        "aster_search": latency_stats(aster_lat),
        "competitor_search": latency_stats(competitor_lat),
        "accuracy": accuracy,
        "per_query": detailed,
        "competitor_index": hnsw_index,
        "aster_index": hnsw_index,
        "aster_search_mode": "hnsw_scatter_gather",
        "aster_ef_search": args.ef,
    }
    if args.backend == "milvus":
        report["milvus_load"] = competitor_stats
        report["milvus_search"] = report["competitor_search"]
        report["milvus_index"] = hnsw_index
        for query in detailed:
            query["milvus_ms"] = query["competitor_ms"]
            query["milvus_hits"] = query["competitor_hits"]
    report["verdict"] = build_verdict(report)
    write_report(report, args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
