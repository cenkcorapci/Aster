"""Pretty + JSON report for Aster vs Milvus comparison."""

from __future__ import annotations

import json
import sys
from typing import Any, Dict


def _fmt(v: Any) -> str:
    if isinstance(v, float):
        if abs(v) >= 100:
            return f"{v:.2f}"
        if abs(v) >= 1:
            return f"{v:.4f}"
        return f"{v:.6f}"
    return str(v)


def render_text(report: Dict[str, Any]) -> str:
    lines = []
    a = report.get("scale", {})
    lines.append("=" * 72)
    lines.append("Aster vs Milvus — distributed MinIO comparison")
    lines.append("=" * 72)
    lines.append(
        f"target_vectors={a.get('target_vectors')}  actual_vectors={a.get('actual_vectors')}  "
        f"dim={a.get('dimension')}  shards={a.get('aster_shards')}  top_k={a.get('top_k')}"
    )
    lines.append(
        f"host_ram_gb={a.get('host_ram_gb')}  usable_gb={a.get('usable_gb')}  "
        f"scaled={a.get('scaled')}  seed={a.get('seed')}"
    )
    lines.append("")
    lines.append("--- Load / index ---")
    al = report.get("aster_load", {})
    ml = report.get("milvus_load", {})
    lines.append(
        f"Aster  load={_fmt(al.get('load_seconds', 0))}s  "
        f"flush={_fmt(al.get('flush_seconds', 0))}s  "
        f"vps={_fmt(al.get('vectors_per_sec', 0))}"
    )
    lines.append(
        f"Milvus insert={_fmt(ml.get('insert_seconds', 0))}s  "
        f"flush={_fmt(ml.get('flush_seconds', 0))}s  "
        f"index={_fmt(ml.get('index_seconds', 0))}s  "
        f"load={_fmt(ml.get('load_seconds', 0))}s  "
        f"vps={_fmt(ml.get('vectors_per_sec', 0))}"
    )
    lines.append("")
    lines.append("--- Search latency (ms) ---")
    for name in ("aster_search", "milvus_search"):
        s = report.get(name, {})
        lines.append(
            f"{name:14s}  avg={_fmt(s.get('avg_ms', 0))}  "
            f"p50={_fmt(s.get('p50_ms', 0))}  p95={_fmt(s.get('p95_ms', 0))}  "
            f"p99={_fmt(s.get('p99_ms', 0))}  max={_fmt(s.get('max_ms', 0))}"
        )
    lines.append("")
    lines.append("--- Result agreement (Aster exact = ground truth) ---")
    acc = report.get("accuracy", {})
    lines.append(
        f"mean Jaccard@{a.get('top_k')}: {_fmt(acc.get('mean_jaccard_at_k', 0))}  "
        f"(min={_fmt(acc.get('min_jaccard_at_k', 0))} max={_fmt(acc.get('max_jaccard_at_k', 0))})"
    )
    lines.append(
        f"mean Milvus recall@k vs Aster: {_fmt(acc.get('mean_milvus_recall_vs_aster', 0))}"
    )
    lines.append(
        f"top-1 agreement: {_fmt(acc.get('top1_agreement_rate', 0))}  "
        f"perfect agreement: {_fmt(acc.get('perfect_agreement_rate', 0))}"
    )
    lines.append(
        f"mean score MAE on overlap: {_fmt(acc.get('mean_score_mae_overlap', 0))}  "
        f"mean |rank Δ|: {_fmt(acc.get('mean_mean_abs_rank_delta', 0))}"
    )
    lines.append(
        f"mean overlap count @k: {_fmt(acc.get('mean_overlap_at_k', 0))} / {a.get('top_k')}"
    )
    verdict = report.get("verdict", {})
    lines.append("")
    lines.append("--- Verdict ---")
    lines.append(verdict.get("summary", ""))
    if verdict.get("notes"):
        for n in verdict["notes"]:
            lines.append(f"  • {n}")
    lines.append("=" * 72)
    return "\n".join(lines) + "\n"


def write_report(report: Dict[str, Any], out_dir: str) -> None:
    import os

    os.makedirs(out_dir, exist_ok=True)
    json_path = os.path.join(out_dir, "report.json")
    txt_path = os.path.join(out_dir, "report.txt")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, sort_keys=True)
    text = render_text(report)
    with open(txt_path, "w", encoding="utf-8") as f:
        f.write(text)
    sys.stdout.write(text)
    sys.stdout.write(f"\nWrote {json_path}\nWrote {txt_path}\n")


def build_verdict(report: Dict[str, Any]) -> Dict[str, Any]:
    acc = report.get("accuracy", {})
    recall = float(acc.get("mean_milvus_recall_vs_aster", 0))
    jaccard = float(acc.get("mean_jaccard_at_k", 0))
    top1 = float(acc.get("top1_agreement_rate", 0))
    notes = []
    notes.append(
        "Aster search is exact (scatter-gather); Milvus uses HNSW ANN — "
        "differences are expected unless ef is very high / corpus tiny."
    )
    al = report.get("aster_search", {}).get("avg_ms", 0)
    ml = report.get("milvus_search", {}).get("avg_ms", 0)
    if al and ml:
        if ml < al:
            notes.append(
                f"Milvus search avg latency lower ({ml:.2f}ms vs Aster {al:.2f}ms)."
            )
        else:
            notes.append(
                f"Aster search avg latency lower ({al:.2f}ms vs Milvus {ml:.2f}ms)."
            )
    if recall >= 0.99 and jaccard >= 0.99:
        summary = "Results essentially match (Milvus recall≥0.99 vs Aster exact)."
    elif recall >= 0.90:
        summary = (
            f"Close but not identical: mean Milvus recall@k={recall:.4f}, "
            f"Jaccard={jaccard:.4f}, top1={top1:.4f}."
        )
    else:
        summary = (
            f"Material disagreement: mean Milvus recall@k={recall:.4f}, "
            f"Jaccard={jaccard:.4f}, top1={top1:.4f}."
        )
    scale = report.get("scale", {})
    if scale.get("scaled"):
        notes.append(
            f"Corpus auto-scaled from target {scale.get('target_vectors')} to "
            f"actual {scale.get('actual_vectors')} for host RAM."
        )
    return {"summary": summary, "notes": notes}
