#!/usr/bin/env python3
"""
visualize.py -- E2E loadtest JSON -> chart generator.

Usage:
    python visualize.py --before=results/before.json --after=results/after.json --output=charts/

Generates:
    - throughput_comparison.png (bar chart: msg/s, MB/s)
    - latency_comparison.png (grouped bar chart: avg, p50, p90, p99, p99.9)

Input JSON format (echo_loadtest --json output):
    {
        "msg_per_sec": 50000,
        "mb_per_sec": 12.5,
        "latency_us": {"avg": 200, "p50": 150, "p90": 300, "p99": 500, "p999": 1000},
        "connections": 100,
        "payload_size": 256,
        "duration_secs": 30
    }
"""

import matplotlib
matplotlib.use('Agg')  # Non-interactive backend -- MUST be before pyplot import

import argparse
import json
import os
import sys

import matplotlib.pyplot as plt


def load_json(path: str) -> dict:
    try:
        with open(path) as f:
            return json.load(f)
    except json.JSONDecodeError as e:
        print(f"Error: Invalid JSON in {path}: {e}", file=sys.stderr)
        sys.exit(1)


def plot_throughput_comparison(before: dict, after: dict, output_dir: str):
    """Bar chart comparing throughput (msg/s and MB/s as separate subplots)."""
    metrics = [
        ('msg_per_sec', 'Messages/sec'),
        ('mb_per_sec', 'MB/sec'),
    ]

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))
    for i, (key, label) in enumerate(metrics):
        bv = before.get(key, 0)
        av = after.get(key, 0)
        bars = axes[i].bar(['Before', 'After'], [bv, av],
                           color=['#E57373', '#81C784'])
        axes[i].set_title(f'Throughput: {label}')
        # Value labels on bars
        for bar in bars:
            axes[i].text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                         f'{bar.get_height():.1f}', ha='center', va='bottom')

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'throughput_comparison.png'), dpi=150)
    plt.close()


def plot_latency_comparison(before: dict, after: dict, output_dir: str):
    """Grouped bar chart comparing latency percentiles."""
    b_lat = before.get('latency_us', {})
    a_lat = after.get('latency_us', {})

    keys = ['avg', 'p50', 'p90', 'p99', 'p999']
    labels = ['avg', 'p50', 'p90', 'p99', 'p99.9']

    before_vals = [b_lat.get(k, 0) for k in keys]
    after_vals = [a_lat.get(k, 0) for k in keys]

    x = range(len(labels))
    width = 0.35

    fig, ax = plt.subplots(figsize=(10, 6))
    ax.bar([i - width / 2 for i in x], before_vals, width, label='Before', color='#E57373')
    ax.bar([i + width / 2 for i in x], after_vals, width, label='After', color='#81C784')
    ax.set_xlabel('Percentile')
    ax.set_ylabel('Latency (us)')
    ax.set_title('Latency Comparison')
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels)
    ax.legend()

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'latency_comparison.png'), dpi=150)
    plt.close()


def main():
    parser = argparse.ArgumentParser(description='E2E loadtest result visualization')
    parser.add_argument('--before', required=True, help='Before JSON result (echo_loadtest --json)')
    parser.add_argument('--after', required=True, help='After JSON result (echo_loadtest --json)')
    parser.add_argument('--output', default='charts/', help='Output directory for PNG charts')
    args = parser.parse_args()

    os.makedirs(args.output, exist_ok=True)

    before = load_json(args.before)
    after = load_json(args.after)

    plot_throughput_comparison(before, after, args.output)
    plot_latency_comparison(before, after, args.output)

    print(f"Charts saved to {args.output}")


if __name__ == '__main__':
    main()
