#!/usr/bin/env python3
"""Offline checks for deterministic workload generation and shard merging."""

from workload import iter_batches, make_queries, make_vector, make_vectors_batch, merge_topk, shard_of


def test_vectors_are_deterministic_and_normalized() -> None:
    first = make_vector(16, 7, base_seed=42)
    assert first == make_vector(16, 7, base_seed=42)
    assert first != make_vector(16, 7, base_seed=43)
    assert abs(sum(value * value for value in first) - 1.0) < 1e-6


def test_batches_cover_the_requested_range() -> None:
    ids, vectors = make_vectors_batch(4, 8, 3)
    assert ids == ["doc-8", "doc-9", "doc-10"]
    assert len(vectors) == 3
    batches = list(iter_batches(5, 3, 2))
    assert [doc_id for ids, _ in batches for doc_id in ids] == [
        "doc-0",
        "doc-1",
        "doc-2",
        "doc-3",
        "doc-4",
    ]


def test_queries_are_separate_from_corpus_seed() -> None:
    assert make_queries(8, 3, base_seed=1) == make_queries(8, 3, base_seed=999)
    assert make_queries(8, 3) != [make_vector(8, i, 42) for i in range(3)]


def test_sharding_and_merge_deduplicate_by_best_score() -> None:
    assert [shard_of(index, 3) for index in range(6)] == [0, 1, 2, 0, 1, 2]
    merged = merge_topk(
        [
            [("doc-1", 0.6), ("doc-2", 0.5)],
            [("doc-1", 0.9), ("doc-3", 0.8)],
        ],
        3,
    )
    assert merged == [("doc-1", 0.9), ("doc-3", 0.8), ("doc-2", 0.5)]


if __name__ == "__main__":
    test_vectors_are_deterministic_and_normalized()
    test_batches_cover_the_requested_range()
    test_queries_are_separate_from_corpus_seed()
    test_sharding_and_merge_deduplicate_by_best_score()
    print("ok")
