# Aster live simulation + Grafana

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

- Cluster replica count / pods up
- Upsert / delete / search rates
- Write & search latency p50/p95 (and delete/flush)
- Collections, vectors, segments, memtable rows **per node**
- CPU and memory **per node** and **cluster total** (cAdvisor)
- Controller scale events + row budget gauges

## Honesty note

One billion float32 vectors at mixed high dimensions needs multi‑TB. The
controller always reports `sim_target_total_rows` vs `sim_actual_total_rows`
so Grafana shows both the aspirational target and the scaled budget.
