#!/usr/bin/env python3
"""Offline checks for agreement metrics (no cluster required)."""

from metrics import (
    aggregate,
    jaccard_at_k,
    recall_at_k,
    summarize_query_pair,
)
from workload import make_vector, merge_topk, shard_of


def test_merge_and_shard() -> None:
    assert shard_of(5, 4) == 1
    merged = merge_topk(
        [[("a", 0.9), ("b", 0.5)], [("c", 0.8), ("b", 0.6)]],
        k=2,
    )
    assert [x[0] for x in merged] == ["a", "c"]
    v = make_vector(8, 0)
    assert abs(sum(x * x for x in v) - 1.0) < 1e-5


def test_agreement() -> None:
    truth = [("a", 0.9), ("b", 0.8), ("c", 0.7)]
    same = [("a", 0.9), ("b", 0.8), ("c", 0.7)]
    approx = [("a", 0.88), ("c", 0.7), ("d", 0.6)]
    assert jaccard_at_k(same, truth, 3) == 1.0
    assert recall_at_k(same, truth, 3) == 1.0
    assert 0.0 < jaccard_at_k(approx, truth, 3) < 1.0
    s = summarize_query_pair(truth, approx, 3)
    assert s["overlap_at_k"] == 2
    agg = aggregate([s, summarize_query_pair(truth, same, 3)])
    assert agg["queries"] == 2.0
    assert agg["mean_jaccard_at_k"] > 0.5


if __name__ == "__main__":
    test_merge_and_shard()
    test_agreement()
    print("ok")
