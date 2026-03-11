#!/usr/bin/env python3
"""
compare_results.py — JSON benchmark result comparison tool.

Usage:
    python compare_results.py <before.json> <after.json>

Output:
    Formatted comparison table to stdout showing deltas and percent changes.
"""

import json
import sys
from pathlib import Path


def load_json(path: str) -> dict:
    try:
        with open(path, "r") as f:
            return json.load(f)
    except json.JSONDecodeError as e:
        print(f"Error: invalid JSON in {path}: {e}", file=sys.stderr)
        sys.exit(1)


def fmt_delta(before: float, after: float, lower_is_better: bool = True) -> str:
    """Format delta with ASCII indicator."""
    if before == 0:
        return f"{after:.1f} (new)"
    delta = after - before
    pct = (delta / before) * 100
    sign = "+" if delta > 0 else ""
    if lower_is_better:
        indicator = "[OK]" if delta < 0 else "[!!]" if delta > 0 else "[==]"
    else:
        indicator = "[OK]" if delta > 0 else "[!!]" if delta < 0 else "[==]"
    return f"{after:.1f} ({sign}{pct:.1f}%) {indicator}"


def compare(before: dict, after: dict) -> None:
    print("=" * 70)
    print(f"{'Metric':<25} {'Before':>15} {'After':>25}")
    print("=" * 70)

    # Throughput metrics (higher is better)
    for key, label in [
        ("msg_per_sec", "Throughput (msg/s)"),
        ("mb_per_sec", "Throughput (MB/s)"),
    ]:
        b = before.get(key, 0)
        a = after.get(key, 0)
        print(f"{label:<25} {b:>15.1f} {fmt_delta(b, a, lower_is_better=False):>25}")

    # Latency metrics (lower is better)
    b_lat = before.get("latency_us", {})
    a_lat = after.get("latency_us", {})
    for key, label in [
        ("avg", "Latency avg (us)"),
        ("p50", "Latency p50 (us)"),
        ("p90", "Latency p90 (us)"),
        ("p99", "Latency p99 (us)"),
        ("p999", "Latency p99.9 (us)"),
    ]:
        b = b_lat.get(key, 0)
        a = a_lat.get(key, 0)
        print(f"{label:<25} {b:>15.1f} {fmt_delta(b, a, lower_is_better=True):>25}")

    # Config
    print("-" * 70)
    print(f"{'Connections':<25} {before.get('connections', '?'):>15} {after.get('connections', '?'):>25}")
    print(f"{'Payload (bytes)':<25} {before.get('payload_size', '?'):>15} {after.get('payload_size', '?'):>25}")
    print(f"{'Duration (s)':<25} {before.get('duration_secs', '?'):>15} {after.get('duration_secs', '?'):>25}")
    print("=" * 70)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <before.json> <after.json>", file=sys.stderr)
        sys.exit(1)

    before_path, after_path = sys.argv[1], sys.argv[2]
    for p in [before_path, after_path]:
        if not Path(p).exists():
            print(f"Error: {p} not found", file=sys.stderr)
            sys.exit(1)

    before = load_json(before_path)
    after = load_json(after_path)
    compare(before, after)


if __name__ == "__main__":
    main()
