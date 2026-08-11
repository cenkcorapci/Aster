# Aster live simulation + Grafana

**M4-T07 deliverable:** importable Grafana dashboard JSON at
[`grafana/dashboards/aster-sim.json`](grafana/dashboards/aster-sim.json).

Boots a **kind** cluster with:

- Up to **50 Aster nodes** (`aster serve`) + **MinIO** durable sync
- Continuous **sawtooth scale** between `MIN_NODES` and `MAX_NODES` (default 15↔50)
- **50 indexes** of mixed dimensions; row budget targets **1B** but auto-scales to host RAM
- Continuous **insert / update / delete / search**
- **Prometheus** scrapes per-pod `/metrics` + cAdvisor CPU/memory
- **Grafana** with a preloaded dashboard

## Commands

```bash
make sim-grafana          # build image, start everything, print URLs
make sim-grafana-stop     # delete kind cluster aster-sim
```

Grafana: http://127.0.0.1:3000 (user `admin` / pass `aster`, anonymous viewer enabled)  
Prometheus: http://127.0.0.1:9090

## Overrides

```bash
make sim-grafana MAX_NODES=50 MIN_NODES=15 START_NODES=25 \
  INDEXES=50 TARGET_TOTAL_ROWS=1000000000 SCALE_EVERY_SEC=45 WORK_RPS=20
```

## Dashboard panels

Core Aster metrics (from `aster/metrics`, scraped at `GET /metrics`):

- Upsert / delete / search / get rates
- Write, delete, flush latency p50/p95
- Read + **hnsw_search_latency** p50/p95
- Collections, vectors, segments, memtable rows, compaction backlog
- Drain / gossip / replication lag

Sim / cluster extras (empty if unused):

- Cluster replica count / pods up
- CPU and memory **per node** and **cluster total** (cAdvisor)
- Controller scale events + row budget gauges

## Import against local Prometheus (no kind)

Use this when Prometheus scrapes a local `aster serve` instead of the sim cluster.

1. Run Aster and confirm metrics:

   ```bash
   # example: aster listening on :8080
   curl -sS http://127.0.0.1:8080/metrics | head
   ```

2. Scrape it from local Prometheus (`prometheus.yml`):

   ```yaml
   scrape_configs:
     - job_name: aster
       metrics_path: /metrics
       static_configs:
         - targets: ["127.0.0.1:8080"]
   ```

   Reload Prometheus and check **Status → Targets** that `aster` is `UP`.

3. In Grafana, add a Prometheus datasource with **UID** `prometheus`
   (Connections → Data sources → Add → Prometheus → set UID to `prometheus`,
   URL e.g. `http://127.0.0.1:9090`). The shipped JSON binds panels to that UID.

4. Import the dashboard:

   - **Dashboards → New → Import → Upload JSON file**
   - Choose `deploy/sim-grafana/grafana/dashboards/aster-sim.json`
   - Or paste the file contents / use **Import via panel json**

   Sim-only panels (cAdvisor, `sim_*`) stay empty; ops and latency panels
   light up from Aster `/metrics`.

Validate the JSON anytime with:

```bash
python3 -m json.tool deploy/sim-grafana/grafana/dashboards/aster-sim.json >/dev/null
```

## Honesty note

One billion float32 vectors at mixed high dimensions needs multi‑TB. The
controller always reports `sim_target_total_rows` vs `sim_actual_total_rows`
so Grafana shows both the aspirational target and the scaled budget.
