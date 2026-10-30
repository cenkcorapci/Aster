"""Qdrant backend wrapper for the shared Aster comparison workload."""

from __future__ import annotations

import time
from typing import Any, Dict, List, Sequence, Tuple

import numpy as np


class QdrantCluster:
    def __init__(
        self,
        url: str,
        collection: str = "bench",
        dim: int = 2048,
        m: int = 16,
        ef_construction: int = 200,
        ef: int = 128,
    ):
        self.url = url
        self.collection_name = collection
        self.dim = dim
        self.m = m
        self.ef_construction = ef_construction
        self.ef = ef
        self._client = None

    def connect(self, attempts: int = 60) -> None:
        from qdrant_client import QdrantClient

        self._client = QdrantClient(url=self.url)
        last = None
        for _ in range(attempts):
            try:
                self._client.get_collections()
                return
            except Exception as e:  # noqa: BLE001
                last = e
                time.sleep(5)
        raise RuntimeError(f"qdrant connect failed: {last}")

    def ensure_collection(self) -> None:
        from qdrant_client import models

        assert self._client is not None
        self._client.recreate_collection(
            collection_name=self.collection_name,
            vectors_config=models.VectorParams(
                size=self.dim, distance=models.Distance.COSINE
            ),
            hnsw_config=models.HnswConfigDiff(
                m=self.m, ef_construct=self.ef_construction
            ),
        )

    def load(
        self,
        ids: Sequence[str],
        vectors: np.ndarray,
        batch_size: int = 256,
    ) -> Dict[str, Any]:
        assert self._client is not None
        t0 = time.perf_counter()
        numeric_ids = [int(doc_id.split("-", 1)[1]) for doc_id in ids]
        self._client.upload_collection(
            collection_name=self.collection_name,
            vectors=vectors.tolist(),
            ids=numeric_ids,
            batch_size=batch_size,
            wait=True,
        )
        insert_s = time.perf_counter() - t0
        return {
            "vectors": len(ids),
            "insert_seconds": insert_s,
            "flush_seconds": 0.0,
            "index_seconds": 0.0,
            "load_seconds": 0.0,
            "vectors_per_sec": len(ids) / insert_s if insert_s > 0 else 0.0,
        }

    def search(self, vector: Sequence[float], top_k: int) -> Tuple[List[Tuple[str, float]], float]:
        from qdrant_client import models

        assert self._client is not None
        t0 = time.perf_counter()
        response = self._client.query_points(
            collection_name=self.collection_name,
            query=list(map(float, vector)),
            limit=top_k,
            search_params=models.SearchParams(hnsw_ef=self.ef),
        )
        ms = (time.perf_counter() - t0) * 1000.0
        return [
            (f"doc-{point.id}", float(point.score))
            for point in response.points
        ], ms
