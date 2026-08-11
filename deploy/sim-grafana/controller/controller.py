#!/usr/bin/env python3
"""Continuous Aster simulation controller: 50 indexes, mixed CRUD, scale 15↔50.

Exposes Prometheus metrics on :9101 for Grafana.
"""

from __future__ import annotations

import json
import os
import random
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import Dict, List, Tuple
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from kubernetes import client, config

NS = os.environ.get("NAMESPACE", "aster-sim")
STS = os.environ.get("STATEFULSET", "aster-node")
MIN_NODES = int(os.environ.get("MIN_NODES", "15"))
MAX_NODES = int(os.environ.get("MAX_NODES", "50"))
START_NODES = int(os.environ.get("START_NODES", "25"))
INDEXES = int(os.environ.get("INDEXES", "50"))
TARGET_TOTAL_ROWS = int(os.environ.get("TARGET_TOTAL_ROWS", "1000000000"))
ACTUAL_TOTAL_ROWS = int(os.environ.get("ACTUAL_TOTAL_ROWS", "50000"))
SCALE_EVERY_SEC = float(os.environ.get("SCALE_EVERY_SEC", "45"))
WORK_RPS = float(os.environ.get("WORK_RPS", "25"))
DIMS = [int(x) for x in os.environ.get("DIMS", "64,128,256,384,768,1024,1536,2048").split(",")]
COLLECTION = "bench"  # unused; each index is its own collection

# ---- Prometheus registry (minimal) -----------------------------------------
_lock = threading.Lock()
_metrics: Dict[str, float] = {
    "sim_cluster_replicas": float(START_NODES),
    "sim_scale_events_total": 0.0,
    "sim_indexes_configured": float(INDEXES),
    "sim_target_total_rows": float(TARGET_TOTAL_ROWS),
    "sim_actual_total_rows": float(ACTUAL_TOTAL_ROWS),
    "sim_controller_ops_total": 0.0,
    "sim_controller_errors_total": 0.0,
    "sim_controller_up": 1.0,
}


def metric_inc(name: str, delta: float = 1.0) -> None:
    with _lock:
        _metrics[name] = _metrics.get(name, 0.0) + delta


def metric_set(name: str, value: float) -> None:
    with _lock:
        _metrics[name] = value


class MetricsHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        return

    def do_GET(self) -> None:  # noqa: N802
        if self.path not in ("/metrics", "/"):
            self.send_response(404)
            self.end_headers()
            return
        with _lock:
            snap = dict(_metrics)
        lines = []
        for k, v in sorted(snap.items()):
            lines.append(f"# TYPE {k} gauge")
            lines.append(f"{k} {v}")
        body = ("\n".join(lines) + "\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; version=0.0.4")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def start_metrics_server() -> None:
    httpd = HTTPServer(("0.0.0.0", 9101), MetricsHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()


# ---- Aster HTTP ------------------------------------------------------------
def http_json(method: str, url: str, body: dict | None = None, timeout: float = 30.0):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = Request(url, data=data, headers=headers, method=method)
    with urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode()
        return resp.status, json.loads(raw) if raw else {}


def node_urls(n: int) -> List[str]:
    # Headless DNS: aster-node-N.aster-node.aster-sim.svc.cluster.local
    return [
        f"http://aster-node-{i}.aster-node.{NS}.svc.cluster.local:8080"
        for i in range(n)
    ]


def wait_nodes(n: int, timeout: float = 300.0) -> List[str]:
    urls = node_urls(n)
    deadline = time.time() + timeout
    while time.time() < deadline:
        ok = 0
        for u in urls:
            try:
                st, _ = http_json("GET", f"{u}/health")
                if st == 200:
                    ok += 1
            except Exception:
                pass
        if ok >= n:
            return urls
        time.sleep(2)
    raise RuntimeError(f"only partial nodes ready for n={n}")


def unit_vec(dim: int, rng: random.Random) -> List[float]:
    v = [rng.gauss(0.0, 1.0) for _ in range(dim)]
    n2 = sum(x * x for x in v) ** 0.5
    if n2 < 1e-12:
        v[0] = 1.0
        n2 = 1.0
    return [x / n2 for x in v]


def build_index_plan() -> List[Tuple[str, int, int]]:
    """Return (name, dim, target_rows) for INDEXES collections; rows sum≈ACTUAL."""
    # Zipf-ish weights so a few indexes are huge (toward billion target story).
    weights = []
    for i in range(INDEXES):
        weights.append(1.0 / ((i % 17) + 1) ** 1.3)
    s = sum(weights)
    plan = []
    assigned = 0
    for i in range(INDEXES):
        dim = DIMS[i % len(DIMS)]
        if i < INDEXES - 1:
            rows = max(20, int(ACTUAL_TOTAL_ROWS * weights[i] / s))
            assigned += rows
        else:
            rows = max(20, ACTUAL_TOTAL_ROWS - assigned)
        name = f"idx{i:02d}_d{dim}_n{rows}"
        plan.append((name, dim, rows))
    return plan


def ensure_collections(urls: List[str], plan: List[Tuple[str, int, int]]) -> None:
    # Create every index on every live node (shared workload / shard-local catalogs).
    for u in urls:
        for name, dim, _ in plan:
            try:
                http_json(
                    "PUT",
                    f"{u}/v1/collections/{name}",
                    {"dimension": dim, "metric": "cosine"},
                )
            except HTTPError as e:
                if e.code not in (400, 409):
                    metric_inc("sim_controller_errors_total")
            except Exception:
                metric_inc("sim_controller_errors_total")


def seed_indexes(urls: List[str], plan: List[Tuple[str, int, int]], rng: random.Random) -> Dict[str, List[str]]:
    """Seed a fraction of each index; return live id sets per collection."""
    live: Dict[str, List[str]] = {name: [] for name, _, _ in plan}
    for name, dim, rows in plan:
        seed_n = min(rows, max(10, rows // 20))  # seed 5%
        for i in range(seed_n):
            doc_id = f"{name}-doc-{i}"
            body = {"vector": unit_vec(dim, rng), "timestamp": i + 1}
            u = rng.choice(urls)
            try:
                http_json("PUT", f"{u}/v1/collections/{name}/docs/{doc_id}", body)
                live[name].append(doc_id)
                metric_inc("sim_controller_ops_total")
            except Exception:
                metric_inc("sim_controller_errors_total")
        # Flush one node periodically
        try:
            http_json("POST", f"{rng.choice(urls)}/v1/collections/{name}/flush", {})
        except Exception:
            pass
    return live


def work_loop(
    get_urls,
    plan: List[Tuple[str, int, int]],
    live: Dict[str, List[str]],
    stop: threading.Event,
) -> None:
    rng = random.Random(7)
    seq = 10_000_000
    interval = 1.0 / max(WORK_RPS, 0.1)
    while not stop.is_set():
        urls = get_urls()
        if not urls:
            time.sleep(1)
            continue
        name, dim, rows = rng.choice(plan)
        u = rng.choice(urls)
        op = rng.random()
        try:
            if op < 0.45 or not live[name]:
                # insert
                seq += 1
                doc_id = f"{name}-doc-{seq}"
                http_json(
                    "PUT",
                    f"{u}/v1/collections/{name}/docs/{doc_id}",
                    {"vector": unit_vec(dim, rng), "timestamp": seq},
                )
                live[name].append(doc_id)
                if len(live[name]) > rows:
                    # trim tracking (not physical) to target size
                    live[name] = live[name][-rows:]
            elif op < 0.65:
                # update
                doc_id = rng.choice(live[name])
                http_json(
                    "PUT",
                    f"{u}/v1/collections/{name}/docs/{doc_id}",
                    {"vector": unit_vec(dim, rng), "timestamp": seq},
                )
            elif op < 0.80:
                # delete
                doc_id = live[name].pop(rng.randrange(len(live[name])))
                http_json("DELETE", f"{u}/v1/collections/{name}/docs/{doc_id}?timestamp={seq}")
            else:
                # search
                http_json(
                    "POST",
                    f"{u}/v1/collections/{name}/search",
                    {"vector": unit_vec(dim, rng), "top_k": 10},
                )
            metric_inc("sim_controller_ops_total")
        except Exception:
            metric_inc("sim_controller_errors_total")
        time.sleep(interval)


def scale_loop(stop: threading.Event) -> None:
    config.load_incluster_config()
    apps = client.AppsV1Api()
    climbing = True
    replicas = START_NODES
    while not stop.is_set():
        if replicas <= MIN_NODES:
            climbing = True
        if replicas >= MAX_NODES:
            climbing = False
        replicas = replicas + 1 if climbing else replicas - 1
        replicas = max(MIN_NODES, min(MAX_NODES, replicas))
        try:
            body = {"spec": {"replicas": replicas}}
            apps.patch_namespaced_stateful_set_scale(STS, NS, body)
            metric_set("sim_cluster_replicas", float(replicas))
            metric_inc("sim_scale_events_total")
            print(json.dumps({"phase": "scale", "replicas": replicas}), flush=True)
        except Exception as e:
            metric_inc("sim_controller_errors_total")
            print(json.dumps({"phase": "scale_error", "error": str(e)}), flush=True)
        stop.wait(SCALE_EVERY_SEC)


def main() -> int:
    start_metrics_server()
    print(
        json.dumps(
            {
                "phase": "start",
                "min": MIN_NODES,
                "max": MAX_NODES,
                "start": START_NODES,
                "indexes": INDEXES,
                "target_rows": TARGET_TOTAL_ROWS,
                "actual_rows": ACTUAL_TOTAL_ROWS,
            }
        ),
        flush=True,
    )

    plan = build_index_plan()
    metric_set("sim_indexes_configured", float(len(plan)))
    metric_set("sim_actual_total_rows", float(sum(r for _, _, r in plan)))

    urls_holder: List[str] = []
    urls_lock = threading.Lock()

    def get_urls() -> List[str]:
        with urls_lock:
            return list(urls_holder)

    # Initial wait
    urls = wait_nodes(START_NODES)
    with urls_lock:
        urls_holder[:] = urls
    ensure_collections(urls, plan)
    rng = random.Random(42)
    live = seed_indexes(urls, plan, rng)
    print(json.dumps({"phase": "seeded", "collections": len(plan)}), flush=True)

    stop = threading.Event()
    threading.Thread(target=scale_loop, args=(stop,), daemon=True).start()

    # Refresh URL list as scale changes
    def refresh() -> None:
        while not stop.is_set():
            try:
                n = int(_metrics.get("sim_cluster_replicas", START_NODES))
                ready = []
                for u in node_urls(n):
                    try:
                        st, _ = http_json("GET", f"{u}/health", timeout=3)
                        if st == 200:
                            ready.append(u)
                    except Exception:
                        pass
                if ready:
                    with urls_lock:
                        urls_holder[:] = ready
                    ensure_collections(ready, plan)
            except Exception:
                pass
            stop.wait(10)

    threading.Thread(target=refresh, daemon=True).start()
    work_loop(get_urls, plan, live, stop)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
