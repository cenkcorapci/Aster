"""Tests for the Aster Python client.

* ``StubServerTests`` run against an in-process fake HTTP server (no binary).
* ``LiveServerTests`` spawn a real ``aster serve`` and are skipped unless a
  binary is found via ``$ASTER_BIN`` or ``bazel-bin/aster/cli/aster``.

Run with ``python -m unittest discover -s clients/python -t clients/python``
(or pytest).
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from aster import (
    AsterConnectionError,
    AsterError,
    Client,
    NotFoundError,
)


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class _Stub(BaseHTTPRequestHandler):
    seen: list = []

    def log_message(self, *a):  # silence
        pass

    def _reply(self, code, obj, ctype="application/json"):
        data = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _handle(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode() if n else ""
        _Stub.seen.append(
            (self.command, self.path, body, self.headers.get("X-API-Key"))
        )
        if self.path == "/health":
            return self._reply(200, {"ok": True})
        if self.path == "/boom":
            return self._reply(500, {"error": "kaput"})
        if self.path.endswith("/docs/missing"):
            return self._reply(404, {"error": "document not found"})
        if self.path.startswith("/v1/collections/nocol"):
            return self._reply(404, {"error": "collection not found"})
        if self.path == "/slow":
            time.sleep(1.0)
            return self._reply(200, {})
        return self._reply(200, {"status": "ok", "hits": []})

    do_GET = do_PUT = do_POST = do_DELETE = _handle


class StubServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.httpd = ThreadingHTTPServer(("127.0.0.1", 0), _Stub)
        cls.port = cls.httpd.server_address[1]
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()

    def setUp(self):
        _Stub.seen.clear()
        self.client = Client(
            base_url=f"http://127.0.0.1:{self.port}", api_key="k", timeout_ms=300
        )

    def test_requires_target(self):
        with self.assertRaises(ValueError):
            Client()

    def test_health_true_and_false(self):
        self.assertTrue(self.client.health())
        dead = Client(seeds=[f"127.0.0.1:{_free_port()}"], timeout_ms=200)
        self.assertFalse(dead.health())

    def test_error_mapping(self):
        with self.assertRaises(AsterError) as cm:
            self.client._request("GET", "/boom")
        self.assertEqual(cm.exception.status, 500)
        self.assertIn("kaput", str(cm.exception))
        with self.assertRaises(NotFoundError):
            self.client.collection("nocol").stats()

    def test_get_missing_doc_is_none_but_missing_collection_raises(self):
        self.assertIsNone(self.client.collection("c").get("missing"))
        with self.assertRaises(NotFoundError):
            self.client.collection("nocol").get("x")

    def test_connection_error_and_timeout(self):
        dead = Client(seeds=[f"127.0.0.1:{_free_port()}"], timeout_ms=200)
        with self.assertRaises(AsterConnectionError):
            dead.usage()
        with self.assertRaises(AsterConnectionError):
            self.client._request("GET", "/slow")

    def test_seed_failover(self):
        c = Client(
            seeds=[f"127.0.0.1:{_free_port()}", f"127.0.0.1:{self.port}"],
            timeout_ms=300,
        )
        self.assertTrue(c.health())

    def test_request_shape(self):
        col = self.client.create_collection("my col", 3, metric="l2", hnsw_m=8)
        method, path, body, key = _Stub.seen[-1]
        self.assertEqual((method, path), ("PUT", "/v1/collections/my%20col"))
        self.assertEqual(
            json.loads(body), {"dimension": 3, "metric": "l2", "hnsw_m": 8}
        )
        self.assertEqual(key, "k")

        col.upsert("a/b c", [1, 2, 3], tags=["x"], metadata=b"bytes", timestamp=7)
        method, path, body, _ = _Stub.seen[-1]
        self.assertEqual(path, "/v1/collections/my%20col/docs/a%2Fb%20c")
        parsed = json.loads(body)
        self.assertEqual(parsed["vector"], [1.0, 2.0, 3.0])
        self.assertEqual(parsed["metadata"], "bytes")
        self.assertEqual(list(json.loads(body))[-1], "metadata")  # emitted last

        col.search([0.5, 0.5, 0.0], top_k=3, ef_search=64, tags=["x"])
        _, path, body, _ = _Stub.seen[-1]
        self.assertEqual(path, "/v1/collections/my%20col/search")
        self.assertEqual(
            json.loads(body),
            {"vector": [0.5, 0.5, 0.0], "top_k": 3, "ef_search": 64, "tags": ["x"]},
        )
        col.delete("d", timestamp=9)
        self.assertEqual(
            _Stub.seen[-1][:2],
            ("DELETE", "/v1/collections/my%20col/docs/d?timestamp=9"),
        )

    def test_rejects_nan(self):
        with self.assertRaises(ValueError):
            self.client.collection("c").upsert("a", [float("nan")])


def _find_binary():
    env = os.environ.get("ASTER_BIN")
    if env and Path(env).exists():
        return env
    root = Path(__file__).resolve().parents[3]
    cand = root / "bazel-bin" / "aster" / "cli" / "aster"
    return str(cand) if cand.exists() else None


@unittest.skipUnless(_find_binary(), "aster binary not found (set ASTER_BIN)")
class LiveServerTests(unittest.TestCase):
    API_KEY = "test-key"

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.port = _free_port()
        cls.proc = subprocess.Popen(
            [
                _find_binary(),
                "serve",
                "--data-dir",
                cls.tmp.name,
                "--port",
                str(cls.port),
                "--api-key",
                cls.API_KEY,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        cls.client = Client(
            seeds=[f"127.0.0.1:{cls.port}"], api_key=cls.API_KEY, timeout_ms=3000
        )
        deadline = time.time() + 10
        while time.time() < deadline and not cls.client.health():
            time.sleep(0.1)
        if not cls.client.health():
            cls.tearDownClass()
            raise RuntimeError("aster serve did not become healthy")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        try:
            cls.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            cls.proc.kill()
        cls.tmp.cleanup()

    def test_auth_required(self):
        with self.assertRaises(AsterError) as cm:
            Client(seeds=[f"127.0.0.1:{self.port}"]).usage()
        self.assertEqual(cm.exception.status, 401)
        # /health is unauthenticated
        self.assertTrue(Client(seeds=[f"127.0.0.1:{self.port}"]).health())

    def test_crud_search_tags(self):
        col = self.client.create_collection("crud", 3, metric="cosine")
        # idempotent
        self.client.create_collection("crud", 3, metric="cosine")
        with self.assertRaises(AsterError):
            self.client.create_collection("crud", 3, exist_ok=False)
        self.assertIn("crud", [c["name"] for c in self.client.list_collections()])

        col.upsert("a", [1, 0, 0], tags=["red"], metadata="alpha\nline2 \"q\" é 😀")
        col.upsert("b", [0.9, 0.1, 0], tags=["blue"], metadata="beta")
        col.upsert("c", [0, 1, 0], tags=["red", "blue"])

        doc = col.get("a")
        self.assertEqual(doc.id, "a")
        self.assertEqual(doc.vector, [1.0, 0.0, 0.0])
        self.assertEqual(doc.tags, ["red"])
        self.assertEqual(doc.metadata, "alpha\nline2 \"q\" é 😀")
        self.assertIsNone(col.get("nope"))

        hits = col.search([1, 0, 0], top_k=3)
        self.assertEqual(hits[0].id, "a")
        self.assertEqual({h.id for h in hits}, {"a", "b", "c"})

        red = col.search([1, 0, 0], top_k=3, tags=["red"])
        self.assertEqual({h.id for h in red}, {"a", "c"})
        blue_only = col.search([1, 0, 0], top_k=3, tags=["blue"])
        self.assertEqual({h.id for h in blue_only}, {"b", "c"})

        with_meta = col.search([1, 0, 0], top_k=1, include_metadata=True)
        self.assertEqual(with_meta[0].metadata, "alpha\nline2 \"q\" é 😀")

        col.upsert("a", [0, 0, 1], tags=["red"])  # replace
        self.assertEqual(col.get("a").vector, [0.0, 0.0, 1.0])

        col.delete("b")
        self.assertIsNone(col.get("b"))
        self.assertNotIn("b", {h.id for h in col.search([1, 0, 0], top_k=5)})

        col.flush()
        self.assertEqual(set(col.get("c").tags), {"red", "blue"})  # survives flush
        col.drop()
        with self.assertRaises(NotFoundError):
            col.stats()

    def test_special_ids(self):
        col = self.client.create_collection("ids", 2)
        for doc_id in ["with space", "pct%25", "ünï", "a+b", "x?y#z"]:
            col.upsert(doc_id, [1, 0], metadata=doc_id)
            got = col.get(doc_id)
            self.assertIsNotNone(got, doc_id)
            self.assertEqual(got.id, doc_id)
        with self.assertRaises(AsterError):
            col.get("a/b")  # '/' is not a valid doc id

    def test_stats_usage_and_metrics(self):
        col = self.client.create_collection("stats", 2)
        for i in range(5):
            col.upsert(f"d{i}", [1, i])
        col.search([1, 0], top_k=2)
        col.get("d0")
        st = col.stats()
        self.assertEqual(st["name"], "stats")
        self.assertEqual(st["dimension"], 2)
        self.assertGreaterEqual(st["vectors_estimate"], 5)
        for key in ("segment_count", "memtable_rows", "compaction_backlog"):
            self.assertIn(key, st)
        self.assertGreaterEqual(self.client.usage()["upserts"], 5)

        text = self.client.metrics()
        self.assertIn("# TYPE aster_searches_total counter", text)
        self.assertIn("# TYPE aster_hnsw_search_latency_seconds histogram", text)
        self.assertIn("# TYPE aster_build_info gauge", text)
        self.assertRegex(text, r'aster_build_info\{version="[^"]+"\} 1')
        self.assertRegex(text, r"(?m)^aster_gets_total [1-9]")
        col.drop()

    def test_validation_errors(self):
        col = self.client.create_collection("val", 2)
        with self.assertRaises(AsterError) as cm:
            col.upsert("x", [1, 2, 3])  # wrong dimension
        self.assertEqual(cm.exception.status, 400)
        with self.assertRaises(AsterError):
            col.search([1, 0], top_k=0)
        with self.assertRaises(NotFoundError):
            self.client.collection("missing_col").upsert("x", [1, 0])
        col.drop()


if __name__ == "__main__":
    unittest.main()
