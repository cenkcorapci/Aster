#!/usr/bin/env python3
"""Offline end-to-end checks for both generic comparison driver backends."""

import json
import os
import sys
import tempfile
from contextlib import redirect_stdout
from io import StringIO
from typing import Sequence

import compare


class FakeAster:
    def __init__(self, urls: list[str]) -> None:
        self.urls = urls

    def wait_all(self) -> None:
        pass

    def ensure_collections(self, *_: object, **__: object) -> None:
        pass

    def upsert_routed(self, ids: Sequence[str], *_: object) -> int:
        return len(ids)

    def flush_all(self) -> dict[str, float]:
        return {"memtable_flush_seconds": 0.1, "index_build_seconds": 0.2}

    def search(self, _: Sequence[float], top_k: int, **__: object) -> tuple[list[tuple[str, float]], float]:
        return [(f"doc-{index}", 1.0 - index / 10.0) for index in range(top_k)], 1.0


class FakeCompetitor:
    created: list["FakeCompetitor"] = []

    def __init__(self, **kwargs: object) -> None:
        self.kwargs = kwargs
        self.loaded: list[str] = []
        FakeCompetitor.created.append(self)

    def connect(self) -> None:
        pass

    def ensure_collection(self) -> None:
        pass

    def load(self, ids: Sequence[str], _: object, **__: object) -> dict[str, float | int]:
        self.loaded = list(ids)
        return {
            "vectors": len(ids),
            "insert_seconds": 0.5,
            "flush_seconds": 0.0,
            "index_seconds": 0.0,
            "load_seconds": 0.0,
            "vectors_per_sec": float(len(ids) * 2),
        }

    def search(self, _: Sequence[float], top_k: int) -> tuple[list[tuple[str, float]], float]:
        return [(f"doc-{index}", 1.0 - index / 10.0) for index in range(top_k)], 0.5


def run_backend(backend: str) -> dict:
    original_argv = sys.argv
    original_aster = compare.AsterCluster
    original_milvus = compare.MilvusCluster
    original_qdrant = compare.QdrantCluster
    try:
        with tempfile.TemporaryDirectory() as out_dir:
            sys.argv = [
                "compare.py",
                "--out-dir",
                out_dir,
                "--aster-urls",
                "http://one,http://two",
                "--backend",
                backend,
                "--competitor-uri",
                "http://competitor",
                "--vectors",
                "4",
                "--dimension",
                "3",
                "--batch-size",
                "2",
                "--queries",
                "2",
                "--top-k",
                "2",
            ]
            compare.AsterCluster = FakeAster
            compare.MilvusCluster = FakeCompetitor
            compare.QdrantCluster = FakeCompetitor
            FakeCompetitor.created.clear()
            with redirect_stdout(StringIO()):
                assert compare.main() == 0
            with open(os.path.join(out_dir, "report.json"), encoding="utf-8") as report_file:
                report = json.load(report_file)
            assert os.path.exists(os.path.join(out_dir, f"{backend}_load.json"))
    finally:
        sys.argv = original_argv
        compare.AsterCluster = original_aster
        compare.MilvusCluster = original_milvus
        compare.QdrantCluster = original_qdrant
    assert FakeCompetitor.created[-1].loaded == ["doc-0", "doc-1", "doc-2", "doc-3"]
    return report


def test_qdrant_driver_report_is_generic() -> None:
    report = run_backend("qdrant")
    assert report["competitor"] == "qdrant"
    assert "competitor_load" in report
    assert "milvus_load" not in report
    assert report["accuracy"]["mean_competitor_recall_vs_aster"] == 1.0
    assert "competitor_hits" in report["per_query"][0]
    assert "milvus_hits" not in report["per_query"][0]


def test_milvus_driver_preserves_legacy_fields() -> None:
    report = run_backend("milvus")
    assert report["competitor"] == "milvus"
    assert report["milvus_load"] == report["competitor_load"]
    assert report["per_query"][0]["milvus_hits"] == report["per_query"][0]["competitor_hits"]


if __name__ == "__main__":
    test_qdrant_driver_report_is_generic()
    test_milvus_driver_preserves_legacy_fields()
    print("ok")
