#!/usr/bin/env python3
"""Aggregate Aster bench pod logs into summary.json + REPORT.md + color CLI."""

from __future__ import annotations

import argparse
import json
import math
import os
import statistics
import sys
from pathlib import Path


def color_enabled() -> bool:
    return sys.stdout.isatty() or os.environ.get("FORCE_COLOR") == "1"


class C:
    if color_enabled():
        RESET = "\033[0m"
        BOLD = "\033[1m"
        DIM = "\033[2m"
        RED = "\033[31m"
        GREEN = "\033[32m"
        YELLOW = "\033[33m"
        BLUE = "\033[34m"
        MAGENTA = "\033[35m"
        CYAN = "\033[36m"
    else:
        RESET = BOLD = DIM = RED = GREEN = YELLOW = BLUE = MAGENTA = CYAN = ""


def parse_json_lines(text: str) -> list[dict]:
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out


def pct(xs: list[float], p: float) -> float:
    if not xs:
        return 0.0
    s = sorted(xs)
    if len(s) == 1:
        return s[0]
    k = (len(s) - 1) * (p / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return s[int(k)]
    return s[f] * (c - k) + s[c] * (k - f)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--mode", required=True)
    ap.add_argument("--config", required=True)
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    cfg = json.loads(Path(args.config).read_text())
    logs_dir = out_dir / "logs"

    finals: list[dict] = []
    progresses: list[dict] = []
    minio_objects: list[int] = []
    errors_total = 0
    pods_with_final = 0

    for log_path in sorted(logs_dir.glob("*.log")):
        if log_path.name.endswith("-minio-sync.log"):
            for obj in parse_json_lines(log_path.read_text(errors="ignore")):
                if obj.get("phase") == "minio_sync":
                    minio_objects.append(int(obj.get("objects") or 0))
            continue
        rows = parse_json_lines(log_path.read_text(errors="ignore"))
        finals_here = [r for r in rows if r.get("phase") == "final"]
        if not finals_here:
            # Fall back to last progress snapshot if process was killed early.
            finals_here = [r for r in rows if r.get("phase") == "progress"][-1:]
        progresses.extend([r for r in rows if r.get("phase") == "progress"])
        if finals_here:
            pods_with_final += 1
            finals.append(finals_here[-1])
            errors_total += int(finals_here[-1].get("errors") or 0)

    def collect(key: str) -> list[float]:
        return [float(f.get(key) or 0) for f in finals]

    write_ops = collect("write_ops_sec")
    update_ops = collect("update_ops_sec")
    search_ops = collect("search_ops_sec")
    write_ms = collect("write_avg_ms")
    update_ms = collect("update_avg_ms")
    search_ms = collect("search_avg_ms")
    elapsed = collect("elapsed_sec")

    cluster_write = sum(write_ops)
    cluster_update = sum(update_ops)
    cluster_search = sum(search_ops)

    summary = {
        "mode": args.mode,
        "config": cfg,
        "pods_reporting": pods_with_final,
        "pods_expected": cfg.get("nodes"),
        "errors_total": errors_total,
        "cluster": {
            "write_ops_sec": cluster_write,
            "update_ops_sec": cluster_update,
            "search_ops_sec": cluster_search,
            "total_ops_sec": cluster_write + cluster_update + cluster_search,
        },
        "per_node": {
            "write_ops_sec": {
                "avg": statistics.mean(write_ops) if write_ops else 0,
                "p50": pct(write_ops, 50),
                "p95": pct(write_ops, 95),
            },
            "update_ops_sec": {
                "avg": statistics.mean(update_ops) if update_ops else 0,
                "p50": pct(update_ops, 50),
                "p95": pct(update_ops, 95),
            },
            "search_ops_sec": {
                "avg": statistics.mean(search_ops) if search_ops else 0,
                "p50": pct(search_ops, 50),
                "p95": pct(search_ops, 95),
            },
            "write_avg_ms": {
                "avg": statistics.mean(write_ms) if write_ms else 0,
                "p95": pct(write_ms, 95),
            },
            "update_avg_ms": {
                "avg": statistics.mean(update_ms) if update_ms else 0,
                "p95": pct(update_ms, 95),
            },
            "search_avg_ms": {
                "avg": statistics.mean(search_ms) if search_ms else 0,
                "p95": pct(search_ms, 95),
            },
            "elapsed_sec": {
                "avg": statistics.mean(elapsed) if elapsed else 0,
                "max": max(elapsed) if elapsed else 0,
            },
        },
        "minio": {
            "sync_samples": len(minio_objects),
            "objects_last_max": max(minio_objects) if minio_objects else 0,
        },
        "survival": {
            "all_nodes_reported": pods_with_final == cfg.get("nodes"),
            "zero_errors": errors_total == 0,
        },
    }

    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    # ---- Markdown report -------------------------------------------------
    md = []
    md.append(f"# Aster Bench Report — `{args.mode}`\n")
    md.append(f"**Stamp:** `{cfg.get('stamp')}`  ")
    md.append(f"**Cluster:** `{cfg.get('cluster')}`  ")
    md.append(f"**Image:** `{cfg.get('image')}`\n")
    md.append("## Scenario\n")
    md.append("| Knob | Value |")
    md.append("| --- | --- |")
    md.append(f"| Storage mode | `{args.mode}` |")
    md.append(f"| Target vectors (cluster) | {cfg.get('target_vectors'):,} |")
    md.append(f"| Actual vectors (cluster) | {cfg.get('actual_vectors'):,} |")
    md.append(f"| Scale vs 100M target | {cfg.get('scale_factor')} |")
    md.append(f"| Nodes | {cfg.get('nodes')} |")
    md.append(f"| Vectors / node | {cfg.get('vectors_per_node'):,} |")
    md.append(f"| Dimension | {cfg.get('dimension')} |")
    md.append(f"| Duration | {cfg.get('duration_sec')}s |")
    md.append(f"| Host | {cfg.get('host_cpus')} CPUs / {cfg.get('host_mem_gb')} GiB |")
    md.append("")
    md.append("## Survival\n")
    md.append(f"- Pods reporting final metrics: **{pods_with_final}/{cfg.get('nodes')}**")
    md.append(f"- Total engine errors: **{errors_total}**")
    md.append(
        f"- Verdict: **{'PASS' if summary['survival']['all_nodes_reported'] and summary['survival']['zero_errors'] else 'DEGRADED'}**"
    )
    md.append("")
    md.append("## Cluster throughput\n")
    md.append("| Op | ops/sec (sum across nodes) |")
    md.append("| --- | ---: |")
    md.append(f"| Write | {cluster_write:,.2f} |")
    md.append(f"| Update | {cluster_update:,.2f} |")
    md.append(f"| Search | {cluster_search:,.2f} |")
    md.append(
        f"| **Total** | **{cluster_write + cluster_update + cluster_search:,.2f}** |"
    )
    md.append("")
    md.append("## Per-node latency / throughput\n")
    md.append("| Metric | avg | p50 | p95 |")
    md.append("| --- | ---: | ---: | ---: |")
    pn = summary["per_node"]
    md.append(
        f"| write ops/s | {pn['write_ops_sec']['avg']:.2f} | {pn['write_ops_sec']['p50']:.2f} | {pn['write_ops_sec']['p95']:.2f} |"
    )
    md.append(
        f"| update ops/s | {pn['update_ops_sec']['avg']:.2f} | {pn['update_ops_sec']['p50']:.2f} | {pn['update_ops_sec']['p95']:.2f} |"
    )
    md.append(
        f"| search ops/s | {pn['search_ops_sec']['avg']:.2f} | {pn['search_ops_sec']['p50']:.2f} | {pn['search_ops_sec']['p95']:.2f} |"
    )
    md.append(
        f"| write avg ms | {pn['write_avg_ms']['avg']:.4f} | — | {pn['write_avg_ms']['p95']:.4f} |"
    )
    md.append(
        f"| update avg ms | {pn['update_avg_ms']['avg']:.4f} | — | {pn['update_avg_ms']['p95']:.4f} |"
    )
    md.append(
        f"| search avg ms | {pn['search_avg_ms']['avg']:.4f} | — | {pn['search_avg_ms']['p95']:.4f} |"
    )
    md.append("")
    if args.mode == "minio":
        md.append("## MinIO (S3 simulation)\n")
        md.append(
            f"- Sync samples observed: {summary['minio']['sync_samples']}"
        )
        md.append(
            f"- Max mirrored objects on a node (last sample): {summary['minio']['objects_last_max']}"
        )
        md.append(
            "- Sidecar `mc mirror` continuously copies `/data` SSTables/WAL/MANIFEST to `s3://aster-bench/nodes/<pod>/`."
        )
        md.append("")
    md.append("## Resource usage\n")
    top_pods = out_dir / "pods-top.txt"
    top_nodes = out_dir / "nodes-top.txt"
    if top_pods.exists() and top_pods.read_text().strip():
        md.append("### `kubectl top pods`\n")
        md.append("```")
        md.append(top_pods.read_text().strip())
        md.append("```\n")
    else:
        md.append("_kubectl top pods unavailable (metrics-server may be missing in kind)._")
        md.append("")
    if top_nodes.exists() and top_nodes.read_text().strip():
        md.append("### `kubectl top nodes`\n")
        md.append("```")
        md.append(top_nodes.read_text().strip())
        md.append("```\n")
    md.append("## Notes\n")
    md.append(
        "- Exact-search engine: working-set size is auto-scaled to host RAM so the "
        "cluster can approach the 100M target without OOMing kind on a laptop."
    )
    md.append(
        "- Local mode uses `emptyDir` (embedded/POSIX disk on each BusyBox node). "
        "MinIO mode adds an `mc` sidecar that mirrors durable files to MinIO."
    )
    md.append(
        "- RPC/gossip are not exercised yet (M4); this measures the single-node "
        "engine under sharded load."
    )
    md.append(
        "- For a quicker smoke: "
        "`make bench-local NODES=8 DURATION=60 TARGET_VECTORS=1000000`."
    )
    md.append("")
    (out_dir / "REPORT.md").write_text("\n".join(md) + "\n")

    # ---- Color CLI banner ------------------------------------------------
    surviving = summary["survival"]["all_nodes_reported"] and summary["survival"]["zero_errors"]
    verdict_color = C.GREEN if surviving else C.YELLOW
    print()
    print(f"{C.BOLD}{C.CYAN}╔══════════════════════════════════════════════════════════╗{C.RESET}")
    print(f"{C.BOLD}{C.CYAN}║           ASTER BENCH RESULTS  ({args.mode:^10})            ║{C.RESET}")
    print(f"{C.BOLD}{C.CYAN}╚══════════════════════════════════════════════════════════╝{C.RESET}")
    print()
    print(f"  {C.BOLD}Nodes{C.RESET}          {pods_with_final}/{cfg.get('nodes')} reporting")
    print(
        f"  {C.BOLD}Vectors{C.RESET}        {cfg.get('actual_vectors'):,} actual  "
        f"{C.DIM}(target {cfg.get('target_vectors'):,}, scale {cfg.get('scale_factor')}){C.RESET}"
    )
    print(f"  {C.BOLD}Dimension{C.RESET}      {cfg.get('dimension')}")
    print(f"  {C.BOLD}Duration{C.RESET}       {cfg.get('duration_sec')}s")
    print()
    print(f"  {C.BOLD}Cluster ops/s{C.RESET}")
    print(f"    {C.GREEN}write {C.RESET} {cluster_write:10.2f}")
    print(f"    {C.BLUE}update{C.RESET} {cluster_update:10.2f}")
    print(f"    {C.MAGENTA}search{C.RESET} {cluster_search:10.2f}")
    print(
        f"    {C.BOLD}total {C.RESET} {cluster_write + cluster_update + cluster_search:10.2f}"
    )
    print()
    print(f"  {C.BOLD}Latency p95 (ms){C.RESET}")
    print(f"    write  {pn['write_avg_ms']['p95']:.4f}")
    print(f"    update {pn['update_avg_ms']['p95']:.4f}")
    print(f"    search {pn['search_avg_ms']['p95']:.4f}")
    print()
    if args.mode == "minio":
        print(
            f"  {C.BOLD}MinIO objects (max sample){C.RESET}  {summary['minio']['objects_last_max']}"
        )
        print()
    print(f"  {C.BOLD}Errors{C.RESET}         {errors_total}")
    print(f"  {C.BOLD}Verdict{C.RESET}        {verdict_color}{'PASS — survived soak' if surviving else 'DEGRADED — inspect logs'}{C.RESET}")
    print()
    print(f"  {C.DIM}Report{C.RESET}   {out_dir / 'REPORT.md'}")
    print(f"  {C.DIM}Summary{C.RESET}  {out_dir / 'summary.json'}")
    print()
    return 0 if surviving else 1


if __name__ == "__main__":
    sys.exit(main())
