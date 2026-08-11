"""Milvus client wrapper (distributed cluster via proxy)."""

from __future__ import annotations

import time
from typing import Any, Dict, List, Sequence, Tuple

import numpy as np


class MilvusCluster:
    def __init__(
        self,
        uri: str,
        collection: str = "bench",
        dim: int = 2048,
        index_type: str = "HNSW",
        metric_type: str = "IP",  # unit-norm vectors → cosine via IP
        m: int = 16,
        ef_construction: int = 200,
        ef: int = 128,
    ):
        self.uri = uri
        self.collection_name = collection
        self.dim = dim
        self.index_type = index_type
        self.metric_type = metric_type
        self.m = m
        self.ef_construction = ef_construction
        self.ef = ef
        self._col = None

    def connect(self, attempts: int = 60) -> None:
        from pymilvus import connections, utility

        last = None
        for _ in range(attempts):
            try:
                connections.connect(alias="default", uri=self.uri)
                utility.get_server_version()
                return
            except Exception as e:  # noqa: BLE001
                last = e
                time.sleep(5)
        raise RuntimeError(f"milvus connect failed: {last}")

    def ensure_collection(self) -> None:
        from pymilvus import (
            Collection,
            CollectionSchema,
            DataType,
            FieldSchema,
            utility,
        )

        if utility.has_collection(self.collection_name):
            utility.drop_collection(self.collection_name)

        fields = [
            FieldSchema(name="id", dtype=DataType.VARCHAR, is_primary=True, max_length=64),
            FieldSchema(name="vector", dtype=DataType.FLOAT_VECTOR, dim=self.dim),
        ]
        schema = CollectionSchema(fields, description="aster-vs-milvus bench")
        self._col = Collection(self.collection_name, schema)

    def load(
        self,
        ids: Sequence[str],
        vectors: np.ndarray,
    ) -> Dict[str, Any]:
        assert self._col is not None
        t0 = time.perf_counter()
        self._col.insert([list(ids), vectors.tolist()])
        insert_s = time.perf_counter() - t0

        t1 = time.perf_counter()
        self._col.flush()
        flush_s = time.perf_counter() - t1

        index_params = {
            "index_type": self.index_type,
            "metric_type": self.metric_type,
            "params": {"M": self.m, "efConstruction": self.ef_construction},
        }
        t2 = time.perf_counter()
        self._col.create_index(field_name="vector", index_params=index_params)
        index_s = time.perf_counter() - t2

        t3 = time.perf_counter()
        self._col.load()
        load_s = time.perf_counter() - t3

        n = len(ids)
        return {
            "vectors": n,
            "insert_seconds": insert_s,
            "flush_seconds": flush_s,
            "index_seconds": index_s,
            "load_seconds": load_s,
            "vectors_per_sec": n / insert_s if insert_s > 0 else 0.0,
        }

    def search(self, vector: Sequence[float], top_k: int) -> Tuple[List[Tuple[str, float]], float]:
        assert self._col is not None
        t0 = time.perf_counter()
        res = self._col.search(
            data=[list(map(float, vector))],
            anns_field="vector",
            param={"metric_type": self.metric_type, "params": {"ef": self.ef}},
            limit=top_k,
            output_fields=["id"],
        )
        ms = (time.perf_counter() - t0) * 1000.0
        hits: List[Tuple[str, float]] = []
        for hit in res[0]:
            hits.append((str(hit.id), float(hit.score)))
        return hits, ms

    def drop(self) -> None:
        from pymilvus import utility

        if utility.has_collection(self.collection_name):
            utility.drop_collection(self.collection_name)
