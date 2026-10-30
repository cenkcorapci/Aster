#!/usr/bin/env python3
"""Offline checks for the Qdrant adapter (no Qdrant server required)."""

from types import SimpleNamespace

import numpy as np

from qdrant_backend import QdrantCluster


class FakeQdrant:
    def __init__(self) -> None:
        self.recreate_kwargs = None
        self.upload_kwargs = None

    def recreate_collection(self, **kwargs: object) -> None:
        self.recreate_kwargs = kwargs

    def upload_collection(self, **kwargs: object) -> None:
        self.upload_kwargs = kwargs

    def query_points(self, **_: object) -> SimpleNamespace:
        return SimpleNamespace(
            points=[
                SimpleNamespace(id=7, score=0.9),
                SimpleNamespace(id=3, score=0.8),
            ]
        )


def test_qdrant_adapter() -> None:
    cluster = QdrantCluster("http://qdrant", dim=3, m=12, ef_construction=99, ef=64)
    fake = FakeQdrant()
    cluster._client = fake

    cluster.ensure_collection()
    assert fake.recreate_kwargs is not None
    assert fake.recreate_kwargs["vectors_config"].size == 3
    assert fake.recreate_kwargs["hnsw_config"].m == 12
    assert fake.recreate_kwargs["hnsw_config"].ef_construct == 99

    stats = cluster.load(["doc-7", "doc-3"], np.ones((2, 3), dtype=np.float32))
    assert stats["vectors"] == 2
    assert fake.upload_kwargs is not None
    assert fake.upload_kwargs["ids"] == [7, 3]
    assert fake.upload_kwargs["wait"] is True

    hits, _ = cluster.search([1.0, 0.0, 0.0], 2)
    assert hits == [("doc-7", 0.9), ("doc-3", 0.8)]


if __name__ == "__main__":
    test_qdrant_adapter()
    print("ok")
