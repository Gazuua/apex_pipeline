# Phase 5.5 Tier 2.5: 시각화 + PDF 보고서 생성 — 구현 계획

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** E2E 부하 테스트 JSON 결과를 시각적 차트로 변환하고, Phase 5.5 종합 성능 보고서를 PDF로 생성하는 파이프라인을 구축한다.

**Architecture:** Python 기반 도구 체인 — visualize.py (matplotlib 차트 생성) → generate_report.py (차트 + 텍스트 → PDF). E2E loadtest의 before/after JSON 결과를 시각화.

**Tech Stack:** Python 3.10+, matplotlib, reportlab

**v6 계획서 참조**: `docs/apex_common/plans/20260312_070255_phase5_5_v6.md` § 5.5 Phase B, § 6 Tier 2.5

**선행 조건**: Tier 2 (메모리 아키텍처) 완료, Tier 1.5 E2E 부하 테스터 사용 가능

---

## File Structure

### New Files

| File | Purpose |
|------|---------|
| `apex_tools/benchmark/visualize/visualize.py` | E2E loadtest JSON → matplotlib 차트 생성 (bar chart) |
| `apex_tools/benchmark/visualize/requirements.txt` | Python 의존성 (matplotlib) |
| `apex_tools/benchmark/report/generate_report.py` | 차트 + JSON 결과 → PDF 보고서 (Tier 구조화) |
| `apex_tools/benchmark/report/requirements.txt` | Python 의존성 (reportlab) |

> **제거된 항목**: `template.py`는 별도 파일로 분리하지 않음 — 스타일 정의가 소량이므로 generate_report.py 인라인으로 충분.

---

## Chunk 1: 시각화 도구 (Tasks 1–2)

### Task 1: visualize.py — JSON → 차트 생성

**Files:**
- Create: `apex_tools/benchmark/visualize/visualize.py`
- Create: `apex_tools/benchmark/visualize/requirements.txt`

- [ ] **Step 1: requirements.txt**

```
matplotlib>=3.8
```

- [ ] **Step 2: visualize.py 작성**

```python
#!/usr/bin/env python3
"""
visualize.py — E2E loadtest JSON -> chart generator.

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
matplotlib.use('Agg')  # Non-interactive backend — MUST be before pyplot import

import argparse
import json
import os

import matplotlib.pyplot as plt


def load_json(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


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
```

- [ ] **Step 3: 로컬 테스트 (더미 데이터)**

```bash
pip install matplotlib
echo '{"msg_per_sec":50000,"mb_per_sec":12.5,"latency_us":{"avg":200,"p50":150,"p90":300,"p99":500,"p999":1000},"connections":100,"payload_size":256,"duration_secs":30}' > /tmp/before.json
echo '{"msg_per_sec":75000,"mb_per_sec":18.7,"latency_us":{"avg":130,"p50":100,"p90":200,"p99":350,"p999":700},"connections":100,"payload_size":256,"duration_secs":30}' > /tmp/after.json
python3 apex_tools/benchmark/visualize/visualize.py \
    --before=/tmp/before.json --after=/tmp/after.json --output=/tmp/charts/
```

Expected: `throughput_comparison.png`, `latency_comparison.png` 생성 확인

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/benchmark/visualize/
git commit -m "feat(tools): 벤치마크 시각화 도구 (visualize.py)"
```

---

### Task 2: generate_report.py — PDF 보고서 생성

**Files:**
- Create: `apex_tools/benchmark/report/generate_report.py`
- Create: `apex_tools/benchmark/report/requirements.txt`

- [ ] **Step 1: requirements.txt**

```
reportlab>=4.0
```

> matplotlib 불필요 — report는 PNG 차트를 참조만 하고 직접 생성하지 않음.

- [ ] **Step 2: generate_report.py 작성**

```python
#!/usr/bin/env python3
"""
generate_report.py — Phase 5.5 benchmark PDF report generator.

Usage:
    python generate_report.py \
        --title="Phase 5.5 Performance Report" \
        --results=results/ \
        --charts=charts/ \
        --output=report.pdf

Report Sections:
    1. Title + metadata (date, system info)
    2. Charts (throughput + latency PNG images)
    3. Detailed Results (JSON data as tables, grouped by Tier)
"""

import argparse
import json
import os
from datetime import datetime
from pathlib import Path

from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import inch
from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Spacer, Image, Table, TableStyle,
    PageBreak
)
from reportlab.lib import colors


def build_report(title: str, results_dir: str, charts_dir: str, output: str):
    doc = SimpleDocTemplate(output, pagesize=A4,
                            leftMargin=inch, rightMargin=inch,
                            topMargin=inch, bottomMargin=inch)
    styles = getSampleStyleSheet()

    title_style = ParagraphStyle('CustomTitle', parent=styles['Title'], fontSize=24)
    heading_style = ParagraphStyle('CustomHeading', parent=styles['Heading1'], fontSize=16)
    meta_style = ParagraphStyle('Meta', parent=styles['Normal'], fontSize=10, textColor=colors.grey)

    elements = []

    # --- Section 1: Title + Metadata ---
    elements.append(Paragraph(title, title_style))
    elements.append(Spacer(1, 0.3 * inch))
    elements.append(Paragraph("Apex Pipeline - Phase 5.5 Core Optimization", styles['Normal']))
    elements.append(Spacer(1, 0.2 * inch))
    elements.append(Paragraph(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M')}", meta_style))
    elements.append(Spacer(1, 0.5 * inch))

    # --- Section 2: Charts ---
    chart_files = sorted(Path(charts_dir).glob('*.png'))
    if chart_files:
        elements.append(Paragraph("Performance Charts", heading_style))
        elements.append(Spacer(1, 0.2 * inch))

        for chart_path in chart_files:
            chart_title = chart_path.stem.replace('_', ' ').title()
            elements.append(Paragraph(chart_title, styles['Heading2']))
            elements.append(Image(str(chart_path), width=6 * inch, height=3.5 * inch))
            elements.append(Spacer(1, 0.3 * inch))

    # --- Section 3: Detailed Results ---
    json_files = sorted(Path(results_dir).glob('*.json'))
    if json_files:
        elements.append(PageBreak())
        elements.append(Paragraph("Detailed Results", heading_style))
        elements.append(Spacer(1, 0.2 * inch))

        for jf in json_files:
            with open(jf) as f:
                data = json.load(f)

            # Tier name from filename (e.g., "baseline_tier0.5" or "after_tier1")
            elements.append(Paragraph(jf.stem, styles['Heading2']))
            table_data = [['Metric', 'Value']]
            for k, v in data.items():
                if isinstance(v, dict):
                    for k2, v2 in v.items():
                        table_data.append([f"  {k}.{k2}", f"{v2}"])
                else:
                    table_data.append([k, f"{v}"])

            t = Table(table_data, colWidths=[3 * inch, 3 * inch])
            t.setStyle(TableStyle([
                ('BACKGROUND', (0, 0), (-1, 0), colors.grey),
                ('TEXTCOLOR', (0, 0), (-1, 0), colors.whitesmoke),
                ('GRID', (0, 0), (-1, -1), 0.5, colors.black),
                ('FONTSIZE', (0, 0), (-1, -1), 9),
                ('TOPPADDING', (0, 0), (-1, -1), 4),
                ('BOTTOMPADDING', (0, 0), (-1, -1), 4),
            ]))
            elements.append(t)
            elements.append(Spacer(1, 0.3 * inch))

    doc.build(elements)
    print(f"Report saved to {output}")


def main():
    parser = argparse.ArgumentParser(description='Generate PDF benchmark report')
    parser.add_argument('--title', default='Phase 5.5 Performance Report')
    parser.add_argument('--results', default='results/', help='Results JSON directory')
    parser.add_argument('--charts', default='charts/', help='Charts PNG directory')
    parser.add_argument('--output', default='report.pdf', help='Output PDF path')
    args = parser.parse_args()

    build_report(args.title, args.results, args.charts, args.output)


if __name__ == '__main__':
    main()
```

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/benchmark/report/
git commit -m "feat(tools): PDF 보고서 생성 파이프라인 (generate_report.py)"
```

---

## Chunk 2: 종합 보고서 생성 (Task 3)

### Task 3: Phase 5.5 종합 벤치마크 보고서 생성

**Files:**
- 실행만 (파일 생성 없음)

- [ ] **Step 1: Tier 2 before/after 측정**

각 Tier 커밋 전후로 E2E 부하 테스트 실행, `--json` 출력을 `results/` 디렉토리에 저장.

- [ ] **Step 2: 차트 생성**

```bash
python3 apex_tools/benchmark/visualize/visualize.py \
    --before=results/baseline.json --after=results/final.json --output=charts/
```

- [ ] **Step 3: PDF 생성**

```bash
python3 apex_tools/benchmark/report/generate_report.py \
    --title="Phase 5.5 Performance Report" \
    --results=results/ --charts=charts/ --output=docs/apex_common/phase5_5_report.pdf
```

- [ ] **Step 4: 결과 커밋**

> charts/ PNG 파일은 커밋하지 않음 — PDF에 이미 포함. results/ JSON과 PDF만 커밋.

```bash
git add results/ docs/apex_common/phase5_5_report.pdf
git commit -m "bench: Phase 5.5 종합 벤치마크 보고서"
```

---

## Summary

### 산출물
1. **visualize.py** — E2E loadtest JSON → PNG 차트 (throughput + latency 비교)
2. **generate_report.py** — 차트 + JSON 결과 → 구조화된 PDF 보고서
3. **Phase 5.5 종합 보고서** — 전 Tier before/after 통합 PDF

### Task 별 커밋 (3개)
1. `feat(tools): 벤치마크 시각화 도구 (visualize.py)`
2. `feat(tools): PDF 보고서 생성 파이프라인 (generate_report.py)`
3. `bench: Phase 5.5 종합 벤치마크 보고서`
