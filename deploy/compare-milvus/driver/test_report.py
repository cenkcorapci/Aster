#!/usr/bin/env python3
"""Offline checks for generic and legacy-compatible reports."""

from report import build_verdict, render_text


def report_for(competitor: str) -> dict:
    return {
        "competitor": competitor,
        "scale": {
            "target_vectors": 100,
            "actual_vectors": 100,
            "dimension": 8,
            "aster_shards": 2,
            "top_k": 10,
            "host_ram_gb": 16,
            "usable_gb": 8,
            "scaled": False,
            "seed": 42,
        },
        "aster_load": {"load_seconds": 2.0, "vectors_per_sec": 50.0},
        "competitor_load": {"insert_seconds": 1.0, "vectors_per_sec": 100.0},
        "aster_search": {"avg_ms": 4.0, "p50_ms": 4.0, "p95_ms": 5.0, "p99_ms": 5.0, "max_ms": 5.0},
        "competitor_search": {"avg_ms": 2.0, "p50_ms": 2.0, "p95_ms": 3.0, "p99_ms": 3.0, "max_ms": 3.0},
        "accuracy": {
            "mean_competitor_recall_vs_aster": 1.0,
            "mean_jaccard_at_k": 1.0,
            "top1_agreement_rate": 1.0,
        },
    }


def test_qdrant_report_uses_generic_fields() -> None:
    report = report_for("qdrant")
    report["verdict"] = build_verdict(report)
    text = render_text(report)
    assert "Aster vs Qdrant" in text
    assert "mean Qdrant recall@k vs Aster" in text
    assert "Qdrant search avg latency lower" in text


def test_milvus_legacy_metric_remains_supported() -> None:
    report = report_for("milvus")
    report["accuracy"] = {
        "mean_milvus_recall_vs_aster": 0.5,
        "mean_jaccard_at_k": 0.4,
        "top1_agreement_rate": 0.2,
    }
    report["competitor_search"] = {}
    report["milvus_search"] = {"avg_ms": 1.0}
    verdict = build_verdict(report)
    assert "Material disagreement" in verdict["summary"]
    assert "Milvus search avg latency lower" in verdict["notes"][1]


if __name__ == "__main__":
    test_qdrant_report_uses_generic_fields()
    test_milvus_legacy_metric_remains_supported()
    print("ok")
