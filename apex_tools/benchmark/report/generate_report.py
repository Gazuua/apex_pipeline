#!/usr/bin/env python3
"""
generate_report.py -- Phase 5.5 benchmark PDF report generator.

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
import sys
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
    charts_path = Path(charts_dir)
    chart_files = sorted(charts_path.glob('*.png')) if charts_path.exists() else []
    if chart_files:
        elements.append(Paragraph("Performance Charts", heading_style))
        elements.append(Spacer(1, 0.2 * inch))

        for chart_path in chart_files:
            if not chart_path.is_file() or chart_path.stat().st_size == 0:
                print(f"Warning: Skipping invalid chart: {chart_path}", file=sys.stderr)
                continue
            chart_title = chart_path.stem.replace('_', ' ').title()
            elements.append(Paragraph(chart_title, styles['Heading2']))
            elements.append(Image(str(chart_path), width=6 * inch, height=3.5 * inch))
            elements.append(Spacer(1, 0.3 * inch))

    # --- Section 3: Detailed Results ---
    results_path = Path(results_dir)
    json_files = sorted(results_path.glob('*.json')) if results_path.exists() else []
    if json_files:
        elements.append(PageBreak())
        elements.append(Paragraph("Detailed Results", heading_style))
        elements.append(Spacer(1, 0.2 * inch))

        for jf in json_files:
            try:
                with open(jf) as f:
                    data = json.load(f)
            except json.JSONDecodeError as e:
                print(f"Warning: Skipping invalid JSON {jf}: {e}", file=sys.stderr)
                continue

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

    if not chart_files and not json_files:
        elements.append(Paragraph("No results or charts found.", styles['Normal']))

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
