"""Accuracy / agreement metrics between two ranked result lists."""

from __future__ import annotations

from typing import Dict, List, Sequence, Tuple

Hit = Tuple[str, float]


def _ids(hits: Sequence[Hit]) -> List[str]:
    return [h[0] for h in hits]


def jaccard_at_k(a: Sequence[Hit], b: Sequence[Hit], k: int) -> float:
    sa = set(_ids(a)[:k])
    sb = set(_ids(b)[:k])
    if not sa and not sb:
        return 1.0
    return len(sa & sb) / float(len(sa | sb))


def recall_at_k(candidate: Sequence[Hit], truth: Sequence[Hit], k: int) -> float:
    """Fraction of ground-truth top-k IDs recovered by candidate."""
    gt = set(_ids(truth)[:k])
    if not gt:
        return 1.0
    got = set(_ids(candidate)[:k])
    return len(gt & got) / float(len(gt))


def overlap_count(a: Sequence[Hit], b: Sequence[Hit], k: int) -> int:
    return len(set(_ids(a)[:k]) & set(_ids(b)[:k]))


def rank_disagreements(a: Sequence[Hit], b: Sequence[Hit], k: int) -> Dict[str, float]:
    """How often overlapping IDs appear at different ranks."""
    ra = {doc: i for i, doc in enumerate(_ids(a)[:k])}
    rb = {doc: i for i, doc in enumerate(_ids(b)[:k])}
    common = set(ra) & set(rb)
    if not common:
        return {
            "common_ids": 0,
            "exact_rank_matches": 0,
            "mean_abs_rank_delta": 0.0,
            "max_abs_rank_delta": 0,
        }
    deltas = [abs(ra[d] - rb[d]) for d in common]
    exact = sum(1 for d in deltas if d == 0)
    return {
        "common_ids": len(common),
        "exact_rank_matches": exact,
        "mean_abs_rank_delta": sum(deltas) / float(len(deltas)),
        "max_abs_rank_delta": max(deltas),
    }


def score_mae_on_overlap(a: Sequence[Hit], b: Sequence[Hit], k: int) -> float:
    sa = {doc: score for doc, score in a[:k]}
    sb = {doc: score for doc, score in b[:k]}
    common = set(sa) & set(sb)
    if not common:
        return 0.0
    return sum(abs(sa[d] - sb[d]) for d in common) / float(len(common))


def same_top1(a: Sequence[Hit], b: Sequence[Hit]) -> bool:
    if not a or not b:
        return not a and not b
    return a[0][0] == b[0][0]


def summarize_query_pair(
    aster_hits: Sequence[Hit], milvus_hits: Sequence[Hit], k: int
) -> Dict[str, float | int | bool]:
    return {
        "overlap_at_k": overlap_count(aster_hits, milvus_hits, k),
        "jaccard_at_k": jaccard_at_k(aster_hits, milvus_hits, k),
        "milvus_recall_vs_aster": recall_at_k(milvus_hits, aster_hits, k),
        "aster_recall_vs_milvus": recall_at_k(aster_hits, milvus_hits, k),
        "score_mae_overlap": score_mae_on_overlap(aster_hits, milvus_hits, k),
        "same_top1": same_top1(aster_hits, milvus_hits),
        **rank_disagreements(aster_hits, milvus_hits, k),
    }


def aggregate(per_query: List[Dict]) -> Dict[str, float]:
    if not per_query:
        return {}
    keys = [
        "overlap_at_k",
        "jaccard_at_k",
        "milvus_recall_vs_aster",
        "aster_recall_vs_milvus",
        "score_mae_overlap",
        "mean_abs_rank_delta",
        "max_abs_rank_delta",
        "exact_rank_matches",
        "common_ids",
    ]
    out: Dict[str, float] = {}
    for key in keys:
        vals = [float(q[key]) for q in per_query if key in q]
        out[f"mean_{key}"] = sum(vals) / len(vals) if vals else 0.0
        out[f"min_{key}"] = min(vals) if vals else 0.0
        out[f"max_{key}"] = max(vals) if vals else 0.0
    same = sum(1 for q in per_query if q.get("same_top1"))
    out["top1_agreement_rate"] = same / float(len(per_query))
    out["queries"] = float(len(per_query))
    # Perfect agreement: jaccard==1 and same ranking of common set
    perfect = sum(
        1
        for q in per_query
        if q.get("jaccard_at_k", 0) >= 0.999
        and q.get("mean_abs_rank_delta", 1) == 0
        and q.get("same_top1")
    )
    out["perfect_agreement_rate"] = perfect / float(len(per_query))
    return out


def percentile(samples: List[float], p: float) -> float:
    if not samples:
        return 0.0
    s = sorted(samples)
    if len(s) == 1:
        return s[0]
    k = (len(s) - 1) * (p / 100.0)
    f = int(k)
    c = min(f + 1, len(s) - 1)
    if f == c:
        return s[f]
    return s[f] * (c - k) + s[c] * (k - f)


def latency_stats(samples_ms: List[float]) -> Dict[str, float]:
    if not samples_ms:
        return {"count": 0, "avg_ms": 0, "p50_ms": 0, "p95_ms": 0, "p99_ms": 0, "max_ms": 0}
    return {
        "count": float(len(samples_ms)),
        "avg_ms": sum(samples_ms) / len(samples_ms),
        "p50_ms": percentile(samples_ms, 50),
        "p95_ms": percentile(samples_ms, 95),
        "p99_ms": percentile(samples_ms, 99),
        "max_ms": max(samples_ms),
    }
