"""Aster Python client over the HTTP JSON API (``aster serve``).

Zero third-party dependencies: uses :mod:`http.client` from the stdlib. The
server closes the connection after each response, so every call opens a short
lived connection; a :class:`Client` is therefore safe to share across threads.

Endpoints used (see ``aster/server/http_api.cc``)::

    GET    /health
    GET    /metrics
    GET    /v1/usage
    GET    /v1/collections
    PUT    /v1/collections/{name}
    GET    /v1/collections/{name}
    GET    /v1/collections/{name}/stats
    DELETE /v1/collections/{name}
    POST   /v1/collections/{name}/search
    POST   /v1/collections/{name}/flush
    POST   /v1/collections/{name}/compact
    PUT    /v1/collections/{name}/docs/{id}
    GET    /v1/collections/{name}/docs/{id}
    DELETE /v1/collections/{name}/docs/{id}
"""

from __future__ import annotations

import http.client
import json
import math
import socket
import urllib.parse
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Sequence, Union

__all__ = [
    "AsterConnectionError",
    "AsterError",
    "Client",
    "Collection",
    "Document",
    "Hit",
    "NotFoundError",
]


class AsterError(Exception):
    """Server returned a non-2xx response."""

    def __init__(self, status: int, message: str) -> None:
        super().__init__(f"aster http {status}: {message}")
        self.status = status
        self.message = message


class NotFoundError(AsterError):
    """HTTP 404 (collection or document does not exist)."""


class AsterConnectionError(AsterError):
    """Transport failure (refused, reset, timeout, malformed response)."""

    def __init__(self, message: str) -> None:
        super().__init__(0, message)


@dataclass(frozen=True)
class Hit:
    id: str
    score: float
    metadata: Optional[str] = None


@dataclass(frozen=True)
class Document:
    id: str
    vector: List[float]
    tags: List[str] = field(default_factory=list)
    timestamp: int = 0
    metadata: str = ""


def _vector(values: Sequence[float]) -> List[float]:
    out = [float(v) for v in values]  # accepts numpy arrays / iterables
    if any(math.isnan(v) or math.isinf(v) for v in out):
        raise ValueError("vector contains NaN or Inf")
    return out


def _metadata(value: Union[str, bytes, None]) -> Optional[str]:
    if value is None:
        return None
    if isinstance(value, (bytes, bytearray, memoryview)):
        return bytes(value).decode("utf-8")
    return value


def _quote(segment: str) -> str:
    return urllib.parse.quote(segment, safe="")


@dataclass
class Collection:
    """Handle to a single collection; cheap to create, safe to cache."""

    name: str
    _client: Optional["Client"] = field(default=None, repr=False, compare=False)

    def _c(self) -> "Client":
        if self._client is None:
            raise RuntimeError("Collection is not bound to a Client")
        return self._client

    def _path(self, *tail: str) -> str:
        return "/".join(["/v1/collections", _quote(self.name), *tail]).replace(
            "//", "/"
        )

    def upsert(
        self,
        id: str,
        vector: Sequence[float],
        *,
        tags: Iterable[str] = (),
        metadata: Union[str, bytes, None] = None,
        timestamp: Optional[int] = None,
        consistency: str = "ONE",
    ) -> None:
        """Insert or replace a document. ``consistency`` is accepted for
        forward compatibility; the single-node HTTP server ignores it."""
        body: Dict[str, Any] = {"vector": _vector(vector), "tags": list(tags)}
        if timestamp is not None:
            body["timestamp"] = int(timestamp)
        meta = _metadata(metadata)
        if meta is not None:
            # Emitted last: the server's minimal JSON reader resolves keys by
            # first textual occurrence, so free-form text must not precede
            # structural keys.
            body["metadata"] = meta
        self._c()._request("PUT", self._path("docs", _quote(id)), body)

    def get(self, id: str, *, consistency: str = "ONE") -> Optional[Document]:
        """Fetch a document; ``None`` when it does not exist."""
        try:
            data = self._c()._request("GET", self._path("docs", _quote(id)))
        except NotFoundError as e:
            if "collection" in e.message:
                raise
            return None
        return Document(
            id=data["id"],
            vector=[float(x) for x in data.get("vector", [])],
            tags=list(data.get("tags", [])),
            timestamp=int(data.get("timestamp", 0)),
            metadata=data.get("metadata", ""),
        )

    def delete(
        self, id: str, *, timestamp: Optional[int] = None, consistency: str = "ONE"
    ) -> None:
        path = self._path("docs", _quote(id))
        if timestamp is not None:
            path += "?timestamp=%d" % int(timestamp)
        self._c()._request("DELETE", path)

    def search(
        self,
        vector: Sequence[float],
        *,
        top_k: int = 10,
        ef_search: Optional[int] = None,
        tags: Iterable[str] = (),
        include_metadata: bool = False,
        consistency: str = "ONE",
    ) -> List[Hit]:
        """ANN search. ``tags`` restricts to documents carrying the tags.

        With ``include_metadata`` each hit costs one extra ``get`` round-trip
        (the server's search response carries only id + score)."""
        body: Dict[str, Any] = {"vector": _vector(vector), "top_k": int(top_k)}
        if ef_search is not None:
            body["ef_search"] = int(ef_search)
        tag_list = list(tags)
        if tag_list:
            body["tags"] = tag_list
        data = self._c()._request("POST", self._path("search"), body)
        hits = [
            Hit(id=h["id"], score=float(h["score"])) for h in data.get("hits", [])
        ]
        if include_metadata:
            enriched = []
            for h in hits:
                doc = self.get(h.id)
                enriched.append(
                    Hit(h.id, h.score, doc.metadata if doc is not None else None)
                )
            hits = enriched
        return hits

    def stats(self) -> Dict[str, Any]:
        """Per-collection stats (vectors_estimate, segment_count, ...)."""
        return self._c()._request("GET", self._path("stats"))

    def info(self) -> Dict[str, Any]:
        return self._c()._request("GET", self._path())

    def flush(self) -> Dict[str, Any]:
        return self._c()._request("POST", self._path("flush"))

    def compact(self) -> None:
        self._c()._request("POST", self._path("compact"))

    def drop(self) -> None:
        self._c()._request("DELETE", self._path())


@dataclass
class Client:
    """Entry point for a single ``aster serve`` node.

    ``seeds`` is a list of ``host:port`` strings; the first entry is used and
    the rest are tried in order on connection failure. Alternatively pass
    ``base_url="http://host:port"``.
    """

    seeds: List[str] = field(default_factory=list)
    tls: bool = False
    timeout_ms: int = 5000
    base_url: Optional[str] = None
    api_key: Optional[str] = None

    def __post_init__(self) -> None:
        self._targets: List[tuple] = []
        if self.base_url:
            u = urllib.parse.urlparse(self.base_url)
            if not u.hostname:
                raise ValueError(f"bad base_url: {self.base_url!r}")
            tls = u.scheme == "https"
            self._targets.append((u.hostname, u.port or (443 if tls else 80), tls))
        for seed in self.seeds:
            host, _, port = seed.rpartition(":")
            if not host:
                host, port = seed, "8080"
            self._targets.append((host, int(port), self.tls))
        if not self._targets:
            raise ValueError("Client needs seeds=[...] or base_url=...")

    # -- collections --------------------------------------------------------

    def collection(self, name: str) -> Collection:
        return Collection(name=name, _client=self)

    def create_collection(
        self,
        name: str,
        dimension: int,
        *,
        metric: str = "cosine",
        exist_ok: bool = True,
        **options: Any,
    ) -> Collection:
        """Create a collection (idempotent by default).

        Extra ``options`` are passed through: ``hnsw_m``,
        ``hnsw_ef_construction``, ``hnsw_ef_search``, ``storage_mode``,
        ``max_vectors``, ``max_qps``, ... With ``exist_ok=False`` an existing
        collection raises :class:`AsterError`.
        """
        body: Dict[str, Any] = {"dimension": int(dimension), "metric": metric}
        body.update(options)
        data = self._request("PUT", f"/v1/collections/{_quote(name)}", body)
        if data.get("status") == "exists" and not exist_ok:
            raise AsterError(409, f"collection {name!r} already exists")
        return self.collection(name)

    def drop_collection(self, name: str) -> None:
        self._request("DELETE", f"/v1/collections/{_quote(name)}")

    def list_collections(self) -> List[Dict[str, Any]]:
        return self._request("GET", "/v1/collections").get("collections", [])

    # -- server -------------------------------------------------------------

    def health(self) -> bool:
        """True when ``/health`` answers ``ok``; never raises."""
        try:
            return bool(self._request("GET", "/health").get("ok"))
        except AsterError:
            return False

    def usage(self) -> Dict[str, Any]:
        return self._request("GET", "/v1/usage")

    def metrics(self) -> str:
        """Raw Prometheus text exposition from ``/metrics``."""
        return self._request("GET", "/metrics", raw=True)

    # -- transport ----------------------------------------------------------

    def _request(
        self,
        method: str,
        path: str,
        body: Optional[Dict[str, Any]] = None,
        *,
        raw: bool = False,
    ) -> Any:
        payload: Optional[bytes] = None
        headers = {"Accept": "application/json"}
        if body is not None:
            payload = json.dumps(
                body, ensure_ascii=False, allow_nan=False, separators=(",", ":")
            ).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if self.api_key:
            headers["X-API-Key"] = self.api_key

        last_err: Optional[Exception] = None
        for host, port, tls in self._targets:
            cls = http.client.HTTPSConnection if tls else http.client.HTTPConnection
            conn = cls(host, port, timeout=self.timeout_ms / 1000.0)
            try:
                conn.request(method, path, body=payload, headers=headers)
                resp = conn.getresponse()
                data = resp.read()
                status = resp.status
            except (OSError, http.client.HTTPException, socket.timeout) as e:
                last_err = e
                continue
            finally:
                conn.close()
            return self._decode(status, data, raw)
        raise AsterConnectionError(f"{type(last_err).__name__}: {last_err}")

    @staticmethod
    def _decode(status: int, data: bytes, raw: bool) -> Any:
        text = data.decode("utf-8", errors="replace")
        if 200 <= status < 300:
            if raw:
                return text
            try:
                return json.loads(text) if text else {}
            except ValueError as e:
                raise AsterConnectionError(f"invalid JSON from server: {e}")
        try:
            message = json.loads(text).get("error", text)
        except (ValueError, AttributeError):
            message = text
        exc = NotFoundError if status == 404 else AsterError
        raise exc(status, str(message))
