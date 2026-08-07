"""Public client facade. See //aster/rpc/aster.thrift for the protocol."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Optional, Sequence


@dataclass(frozen=True)
class Hit:
    id: str
    score: float
    metadata: Optional[bytes] = None


@dataclass
class Collection:
    """Handle to a single collection; cheap to create, safe to cache."""

    name: str

    def upsert(
        self,
        id: str,
        vector: Sequence[float],
        *,
        tags: Iterable[str] = (),
        metadata: Optional[bytes] = None,
        consistency: str = "ONE",
    ) -> None:
        raise NotImplementedError("transport lands in milestone M5")

    def get(self, id: str, *, consistency: str = "ONE"):
        raise NotImplementedError("transport lands in milestone M5")

    def delete(self, id: str, *, consistency: str = "ONE") -> None:
        raise NotImplementedError("transport lands in milestone M5")

    def search(
        self,
        vector: Sequence[float],
        *,
        top_k: int = 10,
        ef_search: Optional[int] = None,
        tags: Iterable[str] = (),
        consistency: str = "ONE",
    ) -> list[Hit]:
        raise NotImplementedError("transport lands in milestone M5")


@dataclass
class Client:
    """Entry point. Connects to any node; every Aster node can coordinate."""

    seeds: list[str] = field(default_factory=list)
    tls: bool = False
    timeout_ms: int = 5000

    def collection(self, name: str) -> Collection:
        return Collection(name=name)
