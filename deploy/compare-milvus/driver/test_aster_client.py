#!/usr/bin/env python3
"""Offline checks for Aster HTTP request construction and shard routing."""

import json

from aster_client import AsterCluster, AsterShardClient


class Response:
    def __init__(self, status_code: int, body: object = None, text: str = "") -> None:
        self.status_code = status_code
        self._body = body if body is not None else {}
        self.text = text

    def json(self) -> object:
        return self._body


class Session:
    def __init__(self, responses: list[Response]) -> None:
        self.responses = responses
        self.calls: list[tuple[str, str, dict]] = []

    def _call(self, method: str, url: str, **kwargs: object) -> Response:
        self.calls.append((method, url, kwargs))
        return self.responses.pop(0)

    def get(self, url: str, **kwargs: object) -> Response:
        return self._call("GET", url, **kwargs)

    def put(self, url: str, **kwargs: object) -> Response:
        return self._call("PUT", url, **kwargs)

    def post(self, url: str, **kwargs: object) -> Response:
        return self._call("POST", url, **kwargs)


class Shard:
    def __init__(self) -> None:
        self.upserts: list[tuple[list[str], list[list[float]]]] = []
        self.searches = 0

    def upsert_batch(self, ids: list[str], vectors: list[list[float]]) -> None:
        self.upserts.append((ids, vectors))

    def flush(self) -> dict[str, float]:
        return {"memtable_flush_seconds": 1.0, "index_build_seconds": 2.0}

    def search(self, *_: object, **__: object) -> list[tuple[str, float]]:
        self.searches += 1
        return [("doc-1", 0.8), ("doc-2", 0.4)]


def test_collection_and_search_requests() -> None:
    client = AsterShardClient("http://aster/", collection="docs")
    session = Session(
        [
            Response(201),
            Response(200, {"hits": [{"id": "doc-1", "score": 0.75}]}),
            Response(200, {"memtable_flush_seconds": 1.5, "index_build_seconds": 2.5}),
        ]
    )
    client.session = session
    client.ensure_collection(3, hnsw_m=16, hnsw_ef_construction=200, hnsw_ef_search=128)
    assert session.calls[0][1] == "http://aster/v1/collections/docs"
    assert json.loads(session.calls[0][2]["data"]) == {
        "dimension": 3,
        "metric": "cosine",
        "hnsw_m": 16,
        "hnsw_ef_construction": 200,
        "hnsw_ef_search": 128,
    }
    assert client.search([1, 0, 0], 5, ef_search=64) == [("doc-1", 0.75)]
    assert json.loads(session.calls[1][2]["data"])["ef_search"] == 64
    assert client.flush() == {"memtable_flush_seconds": 1.5, "index_build_seconds": 2.5}


def test_existing_collection_and_cluster_routing() -> None:
    client = AsterShardClient("http://aster")
    client.session = Session([Response(400, text="exists"), Response(200)])
    client.ensure_collection(2)

    cluster = AsterCluster(["http://one", "http://two"])
    first, second = Shard(), Shard()
    cluster.shards = [first, second]
    cluster.n = 2
    assert cluster.upsert_routed(
        ["doc-0", "doc-1", "doc-2"],
        [[0.0], [1.0], [2.0]],
        [0, 1, 2],
    ) == 3
    assert first.upserts == [(["doc-0", "doc-2"], [[0.0], [2.0]])]
    assert second.upserts == [(["doc-1"], [[1.0]])]
    assert cluster.flush_all() == {
        "memtable_flush_seconds": 2.0,
        "index_build_seconds": 4.0,
    }
    assert cluster.search([1.0], 1)[0] == [("doc-1", 0.8)]


if __name__ == "__main__":
    test_collection_and_search_requests()
    test_existing_collection_and_cluster_routing()
    print("ok")
