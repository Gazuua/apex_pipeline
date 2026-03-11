# Phase 5.5 Tier 2.5: 시각화 + PDF 보고서 생성 — 구현 계획

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 벤치마크 JSON 결과를 시각적 차트로 변환하고, Phase 5.5 종합 성능 보고서를 PDF로 생성하는 파이프라인을 구축한다.

**Architecture:** Python 기반 도구 체인 — visualize.py (matplotlib 차트 생성) → report.py (차트 + 텍스트 → PDF). 모든 Tier의 before/after 벤치마크 결과를 통합.

**Tech Stack:** Python 3.10+, matplotlib, reportlab (또는 weasyprint)

**v6 계획서 참조**: `docs/apex_common/plans/20260311_204613_phase5_5_v6.md` § 5.5 Phase B, § 6 Tier 2.5

**선행 조건**: Tier 2 (메모리 아키텍처) 완료, Tier 1.5 E2E 부하 테스터 사용 가능

---

## File Structure

### New Files

| File | Purpose |
|------|---------|
| `apex_tools/benchmark/visualize/visualize.py` | JSON → matplotlib 차트 생성 (bar/line) |
| `apex_tools/benchmark/visualize/requirements.txt` | Python 의존성 (matplotlib) |
| `apex_tools/benchmark/report/generate_report.py` | 차트 + 텍스트 → PDF 보고서 |
| `apex_tools/benchmark/report/requirements.txt` | Python 의존성 (reportlab) |
| `apex_tools/benchmark/report/template.py` | 보고서 레이아웃/스타일 정의 |

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
visualize.py — Benchmark JSON → chart generator.

Usage:
    python visualize.py --before=results/before.json --after=results/after.json --output=charts/
    python visualize.py --micro=results/micro_*.json --output=charts/

Generates:
    - throughput_comparison.png (bar chart)
    - latency_comparison.png (bar chart, percentiles)
    - latency_cdf.png (line chart, cumulative distribution)
"""

import argparse
import json
import os
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib
matplotlib.use('Agg')  # Non-interactive backend


def load_json(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def plot_throughput_comparison(before: dict, after: dict, output_dir: str):
    """Bar chart comparing throughput (msg/s, MB/s)."""
    labels = ['msg/s', 'MB/s']
    before_vals = [before.get('msg_per_sec', 0), before.get('mb_per_sec', 0)]
    after_vals = [after.get('msg_per_sec', 0), after.get('mb_per_sec', 0)]

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))
    for i, (label, bv, av) in enumerate(zip(labels, before_vals, after_vals)):
        bars = axes[i].bar(['Before', 'After'], [bv, av],
                           color=['#E57373', '#81C784'])
        axes[i].set_title(f'Throughput ({label})')
        axes[i].set_ylabel(label)
        for bar in bars:
            axes[i].text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                         f'{bar.get_height():.1f}', ha='center', va='bottom')

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'throughput_comparison.png'), dpi=150)
    plt.close()


def plot_latency_comparison(before: dict, after: dict, output_dir: str):
    """Bar chart comparing latency percentiles."""
    b_lat = before.get('latency_us', {})
    a_lat = after.get('latency_us', {})

    percentiles = ['avg', 'p50', 'p90', 'p99', 'p999']
    labels = ['avg', 'p50', 'p90', 'p99', 'p99.9']

    before_vals = [b_lat.get(p, 0) for p in percentiles]
    after_vals = [a_lat.get(p, 0) for p in percentiles]

    x = range(len(labels))
    width = 0.35

    fig, ax = plt.subplots(figsize=(10, 6))
    ax.bar([i - width / 2 for i in x], before_vals, width, label='Before', color='#E57373')
    ax.bar([i + width / 2 for i in x], after_vals, width, label='After', color='#81C784')
    ax.set_xlabel('Percentile')
    ax.set_ylabel('Latency (μs)')
    ax.set_title('Latency Comparison')
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels)
    ax.legend()

    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'latency_comparison.png'), dpi=150)
    plt.close()


def main():
    parser = argparse.ArgumentParser(description='Benchmark visualization')
    parser.add_argument('--before', required=True, help='Before JSON result')
    parser.add_argument('--after', required=True, help='After JSON result')
    parser.add_argument('--output', default='charts/', help='Output directory')
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
python3 apex_tools/benchmark/visualize/visualize.py \
    --before=/tmp/before.json --after=/tmp/after.json --output=/tmp/charts/
```

Expected: `throughput_comparison.png`, `latency_comparison.png` 생성

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
matplotlib>=3.8
```

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

Sections:
    1. Executive Summary
    2. System Configuration
    3. Tier-by-Tier Results (before/after charts + tables)
    4. Conclusions
"""

import argparse
import json
import os
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

    # Custom styles
    title_style = ParagraphStyle('CustomTitle', parent=styles['Title'], fontSize=24)
    heading_style = ParagraphStyle('CustomHeading', parent=styles['Heading1'], fontSize=16)

    elements = []

    # Title
    elements.append(Paragraph(title, title_style))
    elements.append(Spacer(1, 0.5 * inch))
    elements.append(Paragraph("Apex Pipeline — Phase 5.5 Core Optimization", styles['Normal']))
    elements.append(Spacer(1, 0.3 * inch))

    # Charts
    chart_files = sorted(Path(charts_dir).glob('*.png'))
    for chart_path in chart_files:
        elements.append(Paragraph(chart_path.stem.replace('_', ' ').title(), heading_style))
        elements.append(Image(str(chart_path), width=6 * inch, height=3.5 * inch))
        elements.append(Spacer(1, 0.3 * inch))

    # Results tables
    json_files = sorted(Path(results_dir).glob('*.json'))
    if json_files:
        elements.append(PageBreak())
        elements.append(Paragraph("Detailed Results", heading_style))

        for jf in json_files:
            with open(jf) as f:
                data = json.load(f)

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
            ]))
            elements.append(t)
            elements.append(Spacer(1, 0.3 * inch))

    doc.build(elements)
    print(f"Report saved to {output}")


def main():
    parser = argparse.ArgumentParser(description='Generate PDF benchmark report')
    parser.add_argument('--title', default='Phase 5.5 Performance Report')
    parser.add_argument('--results', default='results/', help='Results JSON directory')
    parser.add_argument('--charts', default='charts/', help='Charts directory')
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

각 Tier 커밋 전후로 E2E + 마이크로 벤치마크 실행, JSON 결과 저장.

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

```bash
git add results/ charts/ docs/apex_common/phase5_5_report.pdf
git commit -m "bench: Phase 5.5 종합 벤치마크 보고서"
```

---

## Summary

### 산출물
1. **visualize.py** — JSON → PNG 차트 (throughput + latency 비교)
2. **generate_report.py** — 차트 + 결과 → PDF 보고서
3. **Phase 5.5 종합 보고서** — 전 Tier before/after 통합 PDF

### Task 별 커밋 (3개)
1. `feat(tools): 벤치마크 시각화 도구 (visualize.py)`
2. `feat(tools): PDF 보고서 생성 파이프라인 (generate_report.py)`
3. `bench: Phase 5.5 종합 벤치마크 보고서`
