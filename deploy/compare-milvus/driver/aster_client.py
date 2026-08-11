"""HTTP client for sharded Aster `serve` pods."""

from __future__ import annotations

import json
import time
from typing import Any, List, Optional, Sequence, Tuple

import requests

from workload import merge_topk, shard_of


class AsterShardClient:
    def __init__(self, base_url: str, collection: str = "bench", timeout: float = 120.0):
        self.base_url = base_url.rstrip("/")
        self.collection = collection
        self.timeout = timeout
        self.session = requests.Session()

    def _url(self, path: str) -> str:
        return f"{self.base_url}{path}"

    def wait_healthy(self, attempts: int = 60) -> None:
        last: Optional[Exception] = None
        for _ in range(attempts):
            try:
                r = self.session.get(self._url("/health"), timeout=5)
                if r.status_code == 200:
                    return
            except Exception as e:  # noqa: BLE001
                last = e
            time.sleep(2)
        raise RuntimeError(f"aster not healthy at {self.base_url}: {last}")

    def ensure_collection(self, dimension: int, metric: str = "cosine") -> None:
        r = self.session.put(
            self._url(f"/v1/collections/{self.collection}"),
            headers={"Content-Type": "application/json"},
            data=json.dumps({"dimension": dimension, "metric": metric}),
            timeout=self.timeout,
        )
        if r.status_code in (200, 201):
            return
        # Already exists → 400 InvalidArgument from Catalog
        g = self.session.get(
            self._url(f"/v1/collections/{self.collection}"), timeout=self.timeout
        )
        if g.status_code == 200:
            return
        raise RuntimeError(
            f"create collection failed: {r.status_code} {r.text}; get={g.status_code} {g.text}"
        )

    def upsert(self, doc_id: str, vector: Sequence[float]) -> None:
        body = {"vector": list(map(float, vector)), "timestamp": 1}
        r = self.session.put(
            self._url(f"/v1/collections/{self.collection}/docs/{doc_id}"),
            headers={"Content-Type": "application/json"},
            data=json.dumps(body),
            timeout=self.timeout,
        )
        if r.status_code not in (200, 201):
            raise RuntimeError(f"upsert {doc_id}: {r.status_code} {r.text}")

    def upsert_batch(self, ids: Sequence[str], vectors: Sequence[Sequence[float]]) -> None:
        from concurrent.futures import ThreadPoolExecutor, as_completed

        def one(doc_id: str, vec: Sequence[float]) -> None:
            self.upsert(doc_id, vec)

        # Bound concurrency — HTTP/1 keep-alive to one shard.
        workers = min(8, max(1, len(ids)))
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futs = [pool.submit(one, i, v) for i, v in zip(ids, vectors)]
            for f in as_completed(futs):
                f.result()

    def flush(self) -> None:
        r = self.session.post(
            self._url(f"/v1/collections/{self.collection}/flush"),
            timeout=self.timeout,
        )
        if r.status_code != 200:
            raise RuntimeError(f"flush: {r.status_code} {r.text}")

    def search(self, vector: Sequence[float], top_k: int) -> List[Tuple[str, float]]:
        body = {"vector": list(map(float, vector)), "top_k": top_k}
        t0 = time.perf_counter()
        r = self.session.post(
            self._url(f"/v1/collections/{self.collection}/search"),
            headers={"Content-Type": "application/json"},
            data=json.dumps(body),
            timeout=self.timeout,
        )
        latency_ms = (time.perf_counter() - t0) * 1000.0
        if r.status_code != 200:
            raise RuntimeError(f"search: {r.status_code} {r.text}")
        hits = r.json().get("hits", [])
        out = [(h["id"], float(h["score"])) for h in hits]
        # attach latency on the instance for callers that want last
        self.last_search_ms = latency_ms
        return out


class AsterCluster:
    """Client-side scatter-gather over Aster shards (distributed exact search)."""

    def __init__(self, shard_urls: List[str], collection: str = "bench"):
        self.shards = [AsterShardClient(u, collection=collection) for u in shard_urls]
        self.n = len(self.shards)

    def wait_all(self) -> None:
        for s in self.shards:
            s.wait_healthy()

    def ensure_collections(self, dimension: int) -> None:
        for s in self.shards:
            s.ensure_collection(dimension)

    def upsert_routed(
        self,
        ids: Sequence[str],
        vectors,
        id_indices: Sequence[int],
    ) -> int:
        """Route each vector to shard by doc index % N (no flush)."""
        buckets: List[List[Tuple[str, Any]]] = [[] for _ in range(self.n)]
        for doc_id, vec, idx in zip(ids, vectors, id_indices):
            buckets[shard_of(int(idx), self.n)].append((doc_id, vec))

        total = 0
        for si, bucket in enumerate(buckets):
            if not bucket:
                continue
            batch_ids = [x[0] for x in bucket]
            batch_vecs = [x[1] for x in bucket]
            self.shards[si].upsert_batch(batch_ids, batch_vecs)
            total += len(batch_ids)
        return total

    def flush_all(self) -> None:
        for s in self.shards:
            s.flush()

    def search(self, vector: Sequence[float], top_k: int) -> Tuple[List[Tuple[str, float]], float]:
        t0 = time.perf_counter()
        # Over-fetch per shard so merge can reconstruct global top-k.
        per = top_k
        shard_hits = []
        for s in self.shards:
            shard_hits.append(s.search(vector, per))
        merged = merge_topk(shard_hits, top_k)
        ms = (time.perf_counter() - t0) * 1000.0
        return merged, ms
