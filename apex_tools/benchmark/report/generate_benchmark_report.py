#!/usr/bin/env python3
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
"""
generate_benchmark_report.py -- Apex Core 벤치마크 시각화 보고서 생성기 v3.

버전 간 성능 비교 + 방법론 비교 체계.
Google Benchmark JSON -> 인터랙티브 HTML 보고서 (Plotly.js).

Usage:
    python generate_benchmark_report.py \\
        --data-dir=docs/apex_core/benchmark \\
        --baseline=v0.5.9.0 \\
        --current=v0.5.10.0 \\
        --analysis=docs/apex_core/benchmark/analysis.json \\
        --output=docs/apex_core/benchmark/report
"""

import argparse
import json
import os
import sys
from datetime import datetime
from pathlib import Path


# ====================================================================
# JSON Helpers
# ====================================================================

def load_analysis(path):
    """분석 텍스트를 JSON 파일에서 로드."""
    if not path or not os.path.exists(path):
        print(f"경고: 분석 파일 없음 ({path}) — 분석 코멘터리 없이 생성", file=sys.stderr)
        return {}
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def load_benchmarks(directory):
    """디렉토리 내 벤치마크 JSON 로드. metadata.json은 제외."""
    results = {}
    dirpath = Path(directory)
    if not dirpath.exists():
        return results
    for jf in sorted(dirpath.glob('*.json')):
        if jf.stem == 'metadata':
            continue
        try:
            with open(jf, encoding='utf-8') as f:
                data = json.load(f)
            results[jf.stem] = {
                'benchmarks': data.get('benchmarks', []),
                'context': data.get('context', {}),
            }
        except (json.JSONDecodeError, KeyError):
            pass
    return results


def load_metadata(directory):
    """metadata.json 로드. 없으면 벤치마크 JSON의 context에서 추출."""
    dirpath = Path(directory)
    meta_path = dirpath / 'metadata.json'
    if meta_path.exists():
        try:
            with open(meta_path, encoding='utf-8') as f:
                return json.load(f)
        except (json.JSONDecodeError, KeyError):
            pass
    # Fallback: extract from any benchmark JSON context
    for jf in sorted(dirpath.glob('*.json')):
        if jf.stem == 'metadata':
            continue
        try:
            with open(jf, encoding='utf-8') as f:
                data = json.load(f)
            ctx = data.get('context', {})
            if ctx:
                caches = ctx.get('caches', [])
                l1d = l2 = l3 = '-'
                for ca in caches:
                    sz = ca.get('size', 0)
                    ss = f"{sz // 1024} KB" if sz < 1048576 else f"{sz // 1048576} MB"
                    if ca.get('level') == 1 and ca.get('type') == 'Data':
                        l1d = ss
                    elif ca.get('level') == 2:
                        l2 = ss
                    elif ca.get('level') == 3:
                        l3 = ss
                return {
                    'host_name': ctx.get('host_name', '-'),
                    'physical_cores': ctx.get('physical_cores', '-'),
                    'logical_cores': ctx.get('logical_cores', ctx.get('num_cpus', '-')),
                    'mhz_per_cpu': ctx.get('mhz_per_cpu', '-'),
                    'caches': {'l1d': l1d, 'l2': l2, 'l3': l3},
                    'total_ram_mb': ctx.get('total_ram_mb', '-'),
                    'library_version': ctx.get('library_version', '-'),
                }
        except (json.JSONDecodeError, KeyError):
            continue
    return {}


def find(data, stem, name):
    """벤치마크 이름으로 항목 찾기."""
    if not data or stem not in data:
        return None
    for b in data[stem]['benchmarks']:
        if b['name'] == name:
            return b
    return None


def find_prefix(data, stem, prefix):
    """벤치마크 이름 접두사로 모든 항목 찾기."""
    if not data or stem not in data:
        return []
    return [b for b in data[stem]['benchmarks'] if b['name'].startswith(prefix)]


def ft(ns):
    """시간 포맷 (ns -> 적절한 단위)."""
    if ns >= 1e6:
        return f"{ns / 1e6:.2f} ms"
    if ns >= 1e3:
        return f"{ns / 1e3:.1f} us"
    return f"{ns:.1f} ns"


def ftp(b):
    """처리량 포맷."""
    bps = b.get('bytes_per_second', 0)
    ips = b.get('items_per_second', 0)
    if bps > 1e9:
        return f"{bps / 1e9:.1f} GB/s"
    if bps > 1e6:
        return f"{bps / 1e6:.1f} MB/s"
    if ips > 1e6:
        return f"{ips / 1e6:.1f}M items/s"
    if ips > 0:
        return f"{ips:,.0f} items/s"
    return "-"


def delta_pct(baseline_val, current_val):
    """변화율 계산. 양수 = 느려짐(퇴보), 음수 = 빨라짐(개선). 시간 기준."""
    if baseline_val == 0:
        return 0.0
    return ((current_val - baseline_val) / baseline_val) * 100


def delta_pct_throughput(baseline_val, current_val):
    """처리량 변화율. 양수 = 빨라짐(개선), 음수 = 느려짐(퇴보)."""
    if baseline_val == 0:
        return 0.0
    return ((current_val - baseline_val) / baseline_val) * 100


def esc(s):
    """HTML 이스케이프 (분석 텍스트 제외용)."""
    return str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


# ====================================================================
# HTML Generation
# ====================================================================

def build_html(cur, base, analysis, metadata, current_ver, baseline_ver):
    """Self-contained HTML 보고서 생성."""

    # ── Metadata extraction ──
    caches = metadata.get('caches', {})
    if isinstance(caches, list):
        l1d = l2 = l3 = '-'
        for ca in caches:
            sz = ca.get('size', 0)
            ss = f"{sz // 1024} KB" if sz < 1048576 else f"{sz // 1048576} MB"
            if ca.get('level') == 1 and ca.get('type') == 'Data':
                l1d = ss
            elif ca.get('level') == 2:
                l2 = ss
            elif ca.get('level') == 3:
                l3 = ss
    else:
        l1d = caches.get('l1d', '-')
        l2 = caches.get('l2', '-')
        l3 = caches.get('l3', '-')

    cpu_name = metadata.get('cpu_name', f"{metadata.get('mhz_per_cpu', '-')} MHz")
    ram_info = f"{metadata.get('total_ram_mb', '-')} MB"
    ram_speed = metadata.get('ram_speed_mhz', '')
    if ram_speed:
        ram_info += f" (DDR4-{ram_speed})"
    cores_info = f"{metadata.get('physical_cores', '-')}C/{metadata.get('logical_cores', '-')}T"
    cache_info = f"L1D {l1d} / L2 {l2} / L3 {l3}"
    version_info = current_ver
    commit_info = metadata.get('commit', '-')
    date_info = metadata.get('date', datetime.now().strftime('%Y-%m-%dT%H:%M'))
    compiler_info = metadata.get('compiler', 'MSVC 19.44')
    build_type = metadata.get('build_type', 'Release')
    has_baseline = base is not None and len(base) > 0

    # ── Prepare benchmark data for embedding ──
    embed_data = _prepare_embed_data(cur, base, has_baseline)

    # ── Analysis texts ──
    an = {k: analysis.get(k, '') for k in [
        'queue', 'allocators', 'frame_codec', 'serialization', 'dispatcher',
        'session_timer', 'ring_buffer', 'integration', 'overview',
        'version_summary', 'methodology_summary'
    ]}

    # ── Build methodology comparison table data ──
    method_table_rows = _build_methodology_table(cur)

    # ── Summary delta data ──
    summary_delta = _build_summary_delta(cur, base) if has_baseline else []

    # ── Integration data ──
    integration_data = _build_integration_data(cur, base, has_baseline)

    # ── Architecture comparison raw data (for JS chart) ──
    embed_data['architecture_comparison'] = cur.get('architecture_comparison', {})

    # ── Section nav items ──
    nav_items = [
        ('section-cover', 'System Info'),
        ('section-queue', 'Queue'),
        ('section-allocators', 'Allocators'),
        ('section-frame', 'Frame Codec'),
        ('section-serialization', 'Serialization'),
        ('section-dispatcher', 'Dispatcher'),
        ('section-session', 'Session & Timer'),
        ('section-buffer', 'RingBuffer'),
        ('section-summary', 'Summary'),
        ('section-integration', 'Integration'),
    ]

    nav_html = '\n'.join(
        f'<a href="#{nid}" class="nav-item">{label}</a>'
        for nid, label in nav_items
    )

    # ── Build the HTML ──
    html = f'''<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Apex Core Benchmark Report — {esc(current_ver)}</title>
<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
<style>
{_get_css()}
</style>
</head>
<body>

<!-- Header Bar -->
<header class="header-bar">
  <div class="header-content">
    <div class="header-left">
      <h1 class="header-title">Apex Core</h1>
      <span class="header-subtitle">Benchmark Report</span>
    </div>
    <div class="header-right">
      <span class="header-version">{esc(current_ver)}</span>
      {f'<span class="header-arrow">&#8592;</span><span class="header-baseline">{esc(baseline_ver)}</span>' if has_baseline else ''}
    </div>
  </div>
</header>

<!-- Sidebar Navigation -->
<nav class="sidebar" id="sidebar">
  <div class="nav-label">SECTIONS</div>
  {nav_html}
</nav>

<!-- Main Content -->
<main class="main-content">

<div class="card" style="border-top: 4px solid var(--accent); margin-bottom: 24px;">
  <p style="font-size: 15px; line-height: 1.8; color: var(--text-sec);">
    이 문서는 <b style="color: var(--text);">Apex Core 프레임워크의 핵심 컴포넌트 성능</b>을 측정하고,
    아키텍처 선택(SPSC vs MPSC, 커스텀 할당기 vs malloc, zero-copy vs memcpy 등)의
    <b style="color: var(--text);">방법론적 성능 차이</b>를 수치로 증명하는 벤치마크 보고서이다.
  </p>
</div>

{_section_cover(cpu_name, ram_info, cores_info, cache_info, version_info,
                commit_info, date_info, compiler_info, build_type,
                baseline_ver, has_baseline, len(cur), an)}

{_section_queue(cur, base, has_baseline, an)}

{_section_allocators(cur, base, has_baseline, an)}

{_section_frame(cur, base, has_baseline, an)}

{_section_serialization(cur, base, has_baseline, an)}

{_section_dispatcher(cur, base, has_baseline, an)}

{_section_session_timer(cur, base, has_baseline, an)}

{_section_buffer(cur, base, has_baseline, an)}

{_section_summary(cur, base, has_baseline, an, method_table_rows, summary_delta)}

{_section_integration(cur, base, has_baseline, an, integration_data)}

</main>

<script>
{_get_js(embed_data, has_baseline, summary_delta, integration_data)}
</script>

</body>
</html>'''

    return html


# ====================================================================
# CSS
# ====================================================================

def _get_css():
    return '''
:root {
  --primary: #1B5E7B;
  --primary-light: rgba(27,94,123,0.1);
  --accent: #D4A843;
  --accent-light: rgba(212,168,67,0.1);
  --bg: #0F172A;
  --card: #1E293B;
  --text: #F1F5F9;
  --text-sec: #94A3B8;
  --improve: #34D399;
  --warn: #F59E0B;
  --danger: #EF4444;
  --border: rgba(255,255,255,0.05);
  --success: #34D399;
  --warning: #F59E0B;
  --purple: #A78BFA;
  --sidebar-w: 200px;
}

*, *::before, *::after { margin: 0; padding: 0; box-sizing: border-box; }

body {
  font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  background: var(--bg);
  color: var(--text);
  line-height: 1.6;
}

/* Header */
.header-bar {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  height: 60px;
  background: linear-gradient(135deg, #0F172A 0%, #1B3A5C 100%);
  z-index: 1000;
  display: flex;
  align-items: center;
  box-shadow: 0 2px 12px rgba(0,0,0,0.4);
  border-bottom: 1px solid rgba(255,255,255,0.05);
}
.header-content {
  width: 100%;
  max-width: 1400px;
  margin: 0 auto;
  padding: 0 24px 0 calc(var(--sidebar-w) + 24px);
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.header-left { display: flex; align-items: baseline; gap: 12px; }
.header-title {
  font-size: 22px;
  font-weight: 900;
  color: #F8FAFC;
  letter-spacing: 0.5px;
}
.header-subtitle {
  font-size: 14px;
  color: var(--accent);
  font-weight: 900;
}
.header-right { display: flex; align-items: center; gap: 8px; }
.header-version {
  font-size: 15px;
  font-weight: 600;
  color: #F8FAFC;
  background: rgba(255,255,255,0.1);
  padding: 4px 14px;
  border-radius: 6px;
}
.header-arrow { color: var(--accent); font-size: 16px; }
.header-baseline {
  font-size: 13px;
  color: rgba(255,255,255,0.6);
}

/* Sidebar */
.sidebar {
  position: fixed;
  top: 60px;
  left: 0;
  width: var(--sidebar-w);
  height: calc(100vh - 60px);
  background: #0F172A;
  border-right: 1px solid var(--border);
  padding: 20px 0;
  overflow-y: auto;
  z-index: 999;
}
.nav-label {
  font-size: 11px;
  font-weight: 900;
  color: var(--text-sec);
  letter-spacing: 1.5px;
  padding: 8px 20px;
  margin-bottom: 4px;
}
.nav-item {
  display: block;
  padding: 8px 20px 8px 24px;
  font-size: 13px;
  color: var(--text-sec);
  text-decoration: none;
  border-left: 3px solid transparent;
  transition: all 0.2s;
}
.nav-item:hover {
  background: rgba(255,255,255,0.05);
  color: var(--text);
  border-left-color: var(--text-sec);
}
.nav-item.active {
  background: rgba(52,211,153,0.05);
  color: var(--improve);
  font-weight: 600;
  border-left-color: var(--improve);
}

/* Main content */
.main-content {
  margin-left: var(--sidebar-w);
  margin-top: 60px;
  padding: 32px 40px 64px;
  max-width: 1100px;
}

/* Cards */
.card {
  background: var(--card);
  border-radius: 12px;
  box-shadow: 0 4px 24px rgba(0,0,0,0.3);
  border: 1px solid var(--border);
  padding: 28px 32px;
  margin-bottom: 24px;
}
.card-header {
  display: flex;
  align-items: center;
  gap: 14px;
  margin-bottom: 20px;
}
.section-number {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  border-radius: 50%;
  background: linear-gradient(135deg, #34D399, #3B82F6);
  color: #FFFFFF;
  font-weight: 900;
  font-size: 16px;
  flex-shrink: 0;
}
.section-title {
  font-size: 20px;
  font-weight: 900;
  color: var(--text);
}

/* Sub-headers */
.sub-header {
  font-size: 15px;
  font-weight: 600;
  color: #38BDF8;
  margin: 18px 0 10px;
  padding-bottom: 4px;
  border-bottom: 2px solid rgba(56,189,248,0.15);
}

/* Tables */
.data-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
  margin: 12px 0;
  border: 1px solid rgba(255,255,255,0.05);
  border-radius: 8px;
  overflow: hidden;
}
.data-table thead th {
  background: rgba(27,94,123,0.3);
  color: var(--text-sec);
  font-weight: 600;
  padding: 10px 12px;
  text-align: left;
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  border-bottom: 1px solid rgba(255,255,255,0.05);
}
.data-table thead th:not(:first-child) { text-align: right; }
.data-table tbody td {
  padding: 8px 12px;
  border-bottom: 1px solid rgba(255,255,255,0.03);
  color: var(--text);
}
.data-table tbody td:not(:first-child) { text-align: right; font-variant-numeric: tabular-nums; }
.data-table tbody tr:nth-child(even) { background: rgba(255,255,255,0.03); }
.data-table tbody tr:hover { background: rgba(255,255,255,0.05); }

/* Analysis box */
.analysis-box {
  border-left: 4px solid rgba(212,168,67,0.3);
  background: rgba(212,168,67,0.1);
  padding: 16px 20px;
  border-radius: 0 8px 8px 0;
  margin: 16px 0;
  font-size: 14px;
  line-height: 1.7;
  color: var(--text-sec);
}
.analysis-box-alt {
  border-left-color: rgba(27,94,123,0.3);
  background: rgba(27,94,123,0.1);
}

/* Chart containers */
.chart-container {
  margin: 24px 0;
  padding-top: 8px;
  border-radius: 8px;
  overflow: hidden;
  background: var(--card);
}
.sub-header + .chart-container {
  margin-top: 8px;
}
.chart-row {
  display: flex;
  gap: 16px;
  flex-wrap: wrap;
}
.chart-half {
  flex: 1;
  min-width: 300px;
}

/* Info grid */
.info-grid {
  display: grid;
  grid-template-columns: 140px 1fr;
  gap: 6px 16px;
  font-size: 14px;
  margin: 12px 0;
}
.info-label {
  color: var(--text-sec);
  font-weight: 900;
}
.info-value {
  font-weight: 600;
  color: var(--text);
}

/* Delta badges */
.delta-improve {
  color: var(--improve);
  font-weight: 900;
}
.delta-regress {
  color: var(--danger);
  font-weight: 900;
}
.delta-neutral {
  color: var(--text-sec);
  font-weight: 600;
}

/* Method table */
.method-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
  margin: 12px 0;
  border: 1px solid rgba(255,255,255,0.05);
  border-radius: 8px;
  overflow: hidden;
}
.method-table thead th {
  background: rgba(27,94,123,0.3);
  color: var(--text-sec);
  font-weight: 600;
  padding: 10px 14px;
  text-align: left;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  border-bottom: 1px solid rgba(255,255,255,0.05);
}
.method-table tbody td {
  padding: 10px 14px;
  border-bottom: 1px solid rgba(255,255,255,0.03);
  color: var(--text);
}
.method-table tbody tr:nth-child(even) { background: rgba(255,255,255,0.03); }
.method-table tbody tr:hover { background: rgba(255,255,255,0.05); }
.ratio-badge {
  display: inline-block;
  background: linear-gradient(135deg, rgba(52,211,153,0.15), rgba(56,189,248,0.15));
  border: 1px solid rgba(52,211,153,0.3);
  color: var(--improve);
  padding: 2px 10px;
  border-radius: 12px;
  font-weight: 900;
  font-size: 12px;
}

/* Error card */
.error-card {
  background: rgba(239,68,68,0.1);
  border: 1px solid rgba(239,68,68,0.2);
  border-left: 4px solid var(--danger);
  padding: 14px 20px;
  border-radius: 0 8px 8px 0;
  margin: 12px 0;
  color: #F87171;
  font-size: 14px;
}

/* Scroll-margin for anchors under fixed header */
[id^="section-"] {
  scroll-margin-top: 80px;
}

/* Responsive */
@media (max-width: 900px) {
  .sidebar { display: none; }
  .main-content { margin-left: 0; }
  .header-content { padding-left: 24px; }
}

@media print {
  .sidebar { display: none; }
  .header-bar { position: relative; }
  .main-content { margin-left: 0; margin-top: 0; }
  .card { break-inside: avoid; box-shadow: none; border: 1px solid var(--border); }
}
'''


# ====================================================================
# Section Builders
# ====================================================================

def _analysis_html(an, key, alt=False):
    """분석 코멘터리 HTML."""
    text = an.get(key, '')
    if not text:
        return ''
    cls = 'analysis-box analysis-box-alt' if alt else 'analysis-box'
    return f'<div class="{cls}">{text}</div>'


def _bm_table_html(benchmarks, title=''):
    """벤치마크 데이터 테이블 HTML."""
    if not benchmarks:
        return ''
    header = f'<h3 class="sub-header">{title}</h3>' if title else ''
    rows = ''
    for b in benchmarks:
        name = b['name'].replace('BM_', '')
        cpu = ft(b['cpu_time'])
        real = ft(b['real_time'])
        iters = f"{b['iterations']:,}"
        tp = ftp(b)
        error = b.get('error_occurred', False)
        if error:
            err_msg = b.get('error_message', 'error')
            rows += f'<tr><td>{esc(name)}</td><td colspan="4" style="color:var(--danger);text-align:center;">{esc(err_msg)}</td></tr>\n'
        else:
            rows += f'<tr><td>{esc(name)}</td><td>{cpu}</td><td>{real}</td><td>{iters}</td><td>{tp}</td></tr>\n'

    return f'''{header}
<table class="data-table">
<thead><tr><th>Benchmark</th><th>CPU Time</th><th>Real Time</th><th>Iterations</th><th>Throughput</th></tr></thead>
<tbody>
{rows}
</tbody>
</table>'''


def _section_cover(cpu_name, ram_info, cores_info, cache_info, version_info,
                   commit_info, date_info, compiler_info, build_type,
                   baseline_ver, has_baseline, bm_count, an):
    """Section 1: Cover / System Info."""
    baseline_row = f'''
    <div class="info-label">Baseline</div>
    <div class="info-value">{esc(baseline_ver)}</div>''' if has_baseline else ''

    return f'''
<div id="section-cover" class="card">
  <div class="card-header">
    <div class="section-number">1</div>
    <div class="section-title">System Information</div>
  </div>

  <div class="info-grid">
    <div class="info-label">CPU</div>
    <div class="info-value">{esc(cpu_name)}</div>
    <div class="info-label">RAM</div>
    <div class="info-value">{esc(ram_info)}</div>
    <div class="info-label">Cores</div>
    <div class="info-value">{esc(cores_info)}</div>
    <div class="info-label">Cache</div>
    <div class="info-value">{esc(cache_info)}</div>
    <div class="info-label">Version</div>
    <div class="info-value">{esc(version_info)}</div>
    <div class="info-label">Commit</div>
    <div class="info-value">{esc(commit_info)}</div>
    <div class="info-label">Date</div>
    <div class="info-value">{esc(date_info)}</div>
    <div class="info-label">Compiler</div>
    <div class="info-value">{esc(compiler_info)}, C++23, {esc(build_type)}</div>
    <div class="info-label">Benchmarks</div>
    <div class="info-value">{bm_count} files</div>
    {baseline_row}
  </div>

  {_analysis_html(an, 'version_summary', alt=True)}
</div>'''


def _section_queue(cur, base, has_baseline, an):
    """Section 2: Queue Performance."""
    spsc_bms = cur.get('spsc_queue', {}).get('benchmarks', [])
    mpsc_bms = cur.get('mpsc_queue', {}).get('benchmarks', [])

    return f'''
<div id="section-queue" class="card">
  <div class="card-header">
    <div class="section-number">2</div>
    <div class="section-title">Queue Performance — SPSC & MPSC</div>
  </div>

  {_bm_table_html(spsc_bms, 'SPSC Queue (Wait-free)')}
  {_bm_table_html(mpsc_bms, 'MPSC Queue (Lock-free)')}

  {_analysis_html(an, 'queue')}

  <h3 class="sub-header">SPSC vs MPSC Methodology</h3>
  <div class="chart-container" id="chart-queue-methodology" style="height:480px;"></div>

  {'<h3 class="sub-header">Version Comparison</h3><div class="chart-container" id="chart-queue-version" style="height:480px;"></div>' if has_baseline else ''}
</div>'''


def _section_allocators(cur, base, has_baseline, an):
    """Section 3: Memory Allocators."""
    alloc_bms = cur.get('allocators', {}).get('benchmarks', [])

    return f'''
<div id="section-allocators" class="card">
  <div class="card-header">
    <div class="section-number">3</div>
    <div class="section-title">Memory Allocators — Slab, Bump, Arena, malloc, make_shared</div>
  </div>

  {_bm_table_html(alloc_bms)}

  {_analysis_html(an, 'allocators')}

  <h3 class="sub-header">5 Allocators Comparison</h3>
  <div class="chart-container" id="chart-allocators-methodology" style="height:500px;"></div>

  {'<h3 class="sub-header">Version Comparison</h3><div class="chart-container" id="chart-allocators-version" style="height:480px;"></div>' if has_baseline else ''}
</div>'''


def _section_frame(cur, base, has_baseline, an):
    """Section 4: Frame Processing."""
    frame_bms = cur.get('frame_codec', {}).get('benchmarks', [])

    return f'''
<div id="section-frame" class="card">
  <div class="card-header">
    <div class="section-number">4</div>
    <div class="section-title">Frame Processing — FrameCodec</div>
  </div>

  {_bm_table_html(frame_bms)}

  {_analysis_html(an, 'frame_codec')}

  <h3 class="sub-header">Encode vs Decode Throughput Scaling</h3>
  <div class="chart-container" id="chart-frame-scaling" style="height:480px;"></div>

  {'<h3 class="sub-header">Version Comparison</h3><div class="chart-container" id="chart-frame-version" style="height:480px;"></div>' if has_baseline else ''}
</div>'''


def _section_serialization(cur, base, has_baseline, an):
    """Section 5: Serialization."""
    ser_bms = cur.get('serialization', {}).get('benchmarks', [])

    return f'''
<div id="section-serialization" class="card">
  <div class="card-header">
    <div class="section-number">5</div>
    <div class="section-title">Serialization — FlatBuffers vs Heap</div>
  </div>

  {_bm_table_html(ser_bms)}

  {_analysis_html(an, 'serialization')}

  <h3 class="sub-header">Build vs Read Comparison</h3>
  <div class="chart-row">
    <div class="chart-half">
      <div class="chart-container" id="chart-ser-build" style="height:480px;"></div>
    </div>
    <div class="chart-half">
      <div class="chart-container" id="chart-ser-read" style="height:480px;"></div>
    </div>
  </div>

  {'<h3 class="sub-header">Version Comparison</h3><div class="chart-container" id="chart-ser-version" style="height:480px;"></div>' if has_baseline else ''}
</div>'''


def _section_dispatcher(cur, base, has_baseline, an):
    """Section 6: Dispatcher."""
    disp_bms = cur.get('dispatcher', {}).get('benchmarks', [])

    return f'''
<div id="section-dispatcher" class="card">
  <div class="card-header">
    <div class="section-number">6</div>
    <div class="section-title">Hash Map — flat_map vs std::unordered_map 대규모 순회 비교</div>
  </div>

  {_bm_table_html(disp_bms)}

  {_analysis_html(an, 'dispatcher')}

  <h3 class="sub-header">flat_map vs std::unordered_map — 세션 순회 (Iteration)</h3>
  <div class="chart-container" id="chart-dispatcher-iteration" style="height:480px;"></div>

  {'<h3 class="sub-header">Version Comparison</h3><div class="chart-container" id="chart-dispatcher-version" style="height:480px;"></div>' if has_baseline else ''}
</div>'''


def _section_session_timer(cur, base, has_baseline, an):
    """Section 7: Session & Timer."""
    sl_bms = cur.get('session_lifecycle', {}).get('benchmarks', [])
    tw_bms = cur.get('timing_wheel', {}).get('benchmarks', [])

    return f'''
<div id="section-session" class="card">
  <div class="card-header">
    <div class="section-number">7</div>
    <div class="section-title">Session & Timer</div>
  </div>

  <h3 class="sub-header">intrusive_ptr vs shared_ptr</h3>
  <div class="chart-container" id="chart-session-ptr" style="height:380px;"></div>

  {_bm_table_html(tw_bms, 'TimingWheel — O(1) Timeout')}
  {_bm_table_html(sl_bms, 'Session Lifecycle')}

  {_analysis_html(an, 'session_timer')}

  {'<h3 class="sub-header">Version Comparison</h3><div class="chart-container" id="chart-session-version" style="height:480px;"></div>' if has_baseline else ''}
</div>'''


def _section_buffer(cur, base, has_baseline, an):
    """Section 8: RingBuffer."""
    rb_bms = cur.get('ring_buffer', {}).get('benchmarks', [])

    return f'''
<div id="section-buffer" class="card">
  <div class="card-header">
    <div class="section-number">8</div>
    <div class="section-title">Buffer — RingBuffer</div>
  </div>

  {_bm_table_html(rb_bms)}

  {_analysis_html(an, 'ring_buffer')}

  <h3 class="sub-header">Zero-copy vs Naive memcpy (Throughput GB/s)</h3>
  <div class="chart-container" id="chart-buffer-methodology" style="height:480px;"></div>

  {'<h3 class="sub-header">Version Comparison</h3><div class="chart-container" id="chart-buffer-version" style="height:480px;"></div>' if has_baseline else ''}
</div>'''


def _section_summary(cur, base, has_baseline, an, method_table_rows, summary_delta):
    """Section 9: Summary."""
    # Build methodology table HTML
    method_html = ''
    if method_table_rows:
        rows_html = ''
        for row in method_table_rows:
            rows_html += f'''<tr>
  <td>{row[0]}</td><td>{row[1]}</td><td>{row[2]}</td>
  <td><span class="ratio-badge">{row[3]}</span></td>
</tr>\n'''
        method_html = f'''
<h3 class="sub-header">Methodology Comparison Highlights</h3>
<table class="method-table">
<thead><tr><th>Comparison</th><th>Approach A</th><th>Approach B</th><th>Ratio</th></tr></thead>
<tbody>
{rows_html}
</tbody>
</table>'''

    delta_chart = ''
    if has_baseline and summary_delta:
        delta_chart = '''
<h3 class="sub-header">All Components Delta %</h3>
<div class="chart-container" id="chart-summary-delta" style="height:500px;"></div>'''

    return f'''
<div id="section-summary" class="card">
  <div class="card-header">
    <div class="section-number">9</div>
    <div class="section-title">Summary</div>
  </div>

  {delta_chart}
  {method_html}

  {_analysis_html(an, 'overview', alt=True)}
  {_analysis_html(an, 'methodology_summary')}
</div>'''


def _section_integration(cur, base, has_baseline, an, integration_data):
    """Section 10: Integration."""
    # Cross-core latency info
    latency_html = ''
    lat_data = integration_data.get('latency', {})
    if lat_data.get('error'):
        latency_html = f'<div class="error-card">Cross-core RTT: {esc(lat_data.get("error_msg", ""))}</div>'
    elif lat_data.get('has_data'):
        val = lat_data.get('value_us', 0)
        latency_html = f'''
<h3 class="sub-header">Cross-core Latency (RTT)</h3>
<div class="chart-container" id="chart-integration-rtt" style="height:350px;"></div>'''
    else:
        latency_html = '<div class="error-card">Cross-core RTT: No data available</div>'

    return f'''
<div id="section-integration" class="card">
  <div class="card-header">
    <div class="section-number">10</div>
    <div class="section-title">Integration — End-to-end Pipeline</div>
  </div>

  {latency_html}

  <h3 class="sub-header">Cross-core Message Throughput</h3>
  <div class="chart-container" id="chart-integration-throughput" style="height:350px;"></div>

  <h3 class="sub-header">Frame Pipeline & Session Echo Throughput</h3>
  <div class="chart-container" id="chart-integration-pipeline" style="height:380px;"></div>

  {_analysis_html(an, 'integration')}

  {_bm_table_html(cur.get('cross_core_latency', {}).get('benchmarks', []), 'Cross-core Latency')}
  {_bm_table_html(cur.get('cross_core_message_passing', {}).get('benchmarks', []), 'Cross-core Message Passing')}
  {_bm_table_html(cur.get('frame_pipeline', {}).get('benchmarks', []), 'Frame Pipeline')}
  {_bm_table_html(cur.get('session_throughput', {}).get('benchmarks', []), 'Session Throughput')}
  {_bm_table_html(cur.get('architecture_comparison', {}).get('benchmarks', []), 'Architecture Comparison')}
</div>'''


# ====================================================================
# Data Preparation for JS Charts
# ====================================================================

def _prepare_embed_data(cur, base, has_baseline):
    """Prepare all chart data for embedding in HTML."""
    data = {}

    # ── Queue methodology ──
    queue_pairs = [
        ('BM_SpscQueue_Throughput/1024', 'BM_MpscQueue_1P1C/1024', '1P1C cap=1K'),
        ('BM_SpscQueue_Throughput/65536', 'BM_MpscQueue_1P1C/65536', '1P1C cap=64K'),
        ('BM_SpscQueue_Backpressure', 'BM_MpscQueue_Backpressure', 'Backpressure'),
    ]
    q_labels, q_spsc, q_mpsc = [], [], []
    for spsc_name, mpsc_name, label in queue_pairs:
        bs = find(cur, 'spsc_queue', spsc_name)
        bm = find(cur, 'mpsc_queue', mpsc_name)
        if bs and bm:
            q_labels.append(label)
            q_spsc.append(round(bs['cpu_time'], 2))
            q_mpsc.append(round(bm['cpu_time'], 2))
    data['queue_methodology'] = {'labels': q_labels, 'spsc': q_spsc, 'mpsc': q_mpsc}

    # ── Queue version comparison ──
    if has_baseline:
        qv_labels, qv_spsc_cur, qv_mpsc_cur = [], [], []
        qv_spsc_base, qv_mpsc_base = [], []
        scenarios = [
            ('1P1C cap=1K',
             ('spsc_queue', 'BM_SpscQueue_Throughput/1024'),
             ('mpsc_queue', 'BM_MpscQueue_1P1C/1024')),
            ('1P1C cap=64K',
             ('spsc_queue', 'BM_SpscQueue_Throughput/65536'),
             ('mpsc_queue', 'BM_MpscQueue_1P1C/65536')),
            ('Backpressure',
             ('spsc_queue', 'BM_SpscQueue_Backpressure'),
             ('mpsc_queue', 'BM_MpscQueue_Backpressure')),
        ]
        for label, (s_stem, s_name), (m_stem, m_name) in scenarios:
            bs = find(cur, s_stem, s_name)
            bm = find(cur, m_stem, m_name)
            if bs or bm:
                qv_labels.append(label)
                qv_spsc_cur.append(round(bs['cpu_time'], 2) if bs else 0)
                qv_mpsc_cur.append(round(bm['cpu_time'], 2) if bm else 0)
                bsb = find(base, s_stem, s_name)
                bmb = find(base, m_stem, m_name)
                qv_spsc_base.append(round(bsb['cpu_time'], 2) if bsb else 0)
                qv_mpsc_base.append(round(bmb['cpu_time'], 2) if bmb else 0)
        data['queue_version'] = {
            'labels': qv_labels,
            'spsc_cur': qv_spsc_cur, 'mpsc_cur': qv_mpsc_cur,
            'spsc_base': qv_spsc_base, 'mpsc_base': qv_mpsc_base,
        }

    # ── Allocators methodology ──
    sizes = [64, 256, 1024]
    size_labels = ['64B', '256B', '1KB']
    slab_v, bump_v, arena_v, malloc_v = [], [], [], []
    for sz in sizes:
        b = find(cur, 'allocators', f'BM_SlabAllocator_AllocDealloc/{sz}')
        slab_v.append(round(b['cpu_time'], 2) if b else 0)
        b = find(cur, 'allocators', f'BM_BumpAllocator_Alloc/{sz}')
        bump_v.append(round(b['cpu_time'], 2) if b else 0)
        b = find(cur, 'allocators', f'BM_ArenaAllocator_Alloc/{sz}')
        arena_v.append(round(b['cpu_time'], 2) if b else 0)
        b = find(cur, 'allocators', f'BM_Malloc_AllocFree/{sz}')
        malloc_v.append(round(b['cpu_time'], 2) if b else 0)
    shared_b = find(cur, 'allocators', 'BM_MakeShared_AllocDealloc')
    shared_v = round(shared_b['cpu_time'], 2) if shared_b else 0
    data['allocators_methodology'] = {
        'labels': size_labels, 'slab': slab_v, 'bump': bump_v,
        'arena': arena_v, 'malloc': malloc_v, 'make_shared': shared_v,
    }

    # ── Allocators version ──
    if has_baseline:
        alloc_names = [
            ('BM_SlabAllocator_AllocDealloc/64', 'Slab 64B'),
            ('BM_SlabAllocator_AllocDealloc/256', 'Slab 256B'),
            ('BM_SlabAllocator_AllocDealloc/1024', 'Slab 1KB'),
            ('BM_BumpAllocator_Alloc/64', 'Bump 64B'),
            ('BM_BumpAllocator_Alloc/256', 'Bump 256B'),
            ('BM_BumpAllocator_Alloc/1024', 'Bump 1KB'),
            ('BM_ArenaAllocator_Alloc/64', 'Arena 64B'),
            ('BM_ArenaAllocator_Alloc/256', 'Arena 256B'),
            ('BM_ArenaAllocator_Alloc/1024', 'Arena 1KB'),
        ]
        av_labels, av_cur, av_base, av_types = [], [], [], []
        for bm_name, label in alloc_names:
            bc = find(cur, 'allocators', bm_name)
            if bc:
                av_labels.append(label)
                av_cur.append(round(bc['cpu_time'], 2))
                atype = 'slab' if 'Slab' in label else ('bump' if 'Bump' in label else 'arena')
                av_types.append(atype)
                bb = find(base, 'allocators', bm_name)
                av_base.append(round(bb['cpu_time'], 2) if bb else 0)
        data['allocators_version'] = {
            'labels': av_labels, 'cur': av_cur, 'base': av_base, 'types': av_types,
        }

    # ── Frame scaling ──
    frame_sizes = [64, 512, 4096, 16384]
    enc_x, enc_y, dec_x, dec_y = [], [], [], []
    for sz in frame_sizes:
        b = find(cur, 'frame_codec', f'BM_FrameCodec_Encode/{sz}')
        if b:
            enc_x.append(sz)
            enc_y.append(round(b.get('bytes_per_second', 0) / 1e9, 2))
        b = find(cur, 'frame_codec', f'BM_FrameCodec_Decode/{sz}')
        if b:
            dec_x.append(sz)
            dec_y.append(round(b.get('bytes_per_second', 0) / 1e9, 2))
    data['frame_scaling'] = {
        'enc_x': enc_x, 'enc_y': enc_y, 'dec_x': dec_x, 'dec_y': dec_y,
        'x_labels': ['64B', '512B', '4KB', '16KB'],
    }

    # ── Frame version ──
    if has_baseline:
        fv_names = []
        for sz in frame_sizes:
            fv_names.append((f'BM_FrameCodec_Encode/{sz}', f'Enc {sz}B', 'encode'))
            fv_names.append((f'BM_FrameCodec_Decode/{sz}', f'Dec {sz}B', 'decode'))
        fv_labels, fv_cur, fv_base, fv_types = [], [], [], []
        for bm_name, label, ctype in fv_names:
            bc = find(cur, 'frame_codec', bm_name)
            if bc:
                fv_labels.append(label)
                fv_cur.append(round(bc.get('bytes_per_second', 0) / 1e9, 2))
                fv_types.append(ctype)
                bb = find(base, 'frame_codec', bm_name)
                fv_base.append(round(bb.get('bytes_per_second', 0) / 1e9, 2) if bb else 0)
        data['frame_version'] = {
            'labels': fv_labels, 'cur': fv_cur, 'base': fv_base, 'types': fv_types,
        }

    # ── Serialization build + read ──
    ser_sizes = [64, 512, 4096]
    ser_labels = ['64B', '512B', '4KB']
    fb_build, heap_build, fb_read, heap_read = [], [], [], []
    for sz in ser_sizes:
        b = find(cur, 'serialization', f'BM_FlatBuffers_Build/{sz}')
        fb_build.append(round(b['cpu_time'], 2) if b else 0)
        b = find(cur, 'serialization', f'BM_HeapAlloc_Build/{sz}')
        heap_build.append(round(b['cpu_time'], 2) if b else 0)
        b = find(cur, 'serialization', f'BM_FlatBuffers_Read/{sz}')
        fb_read.append(round(b['cpu_time'], 2) if b else 0)
        b = find(cur, 'serialization', f'BM_HeapAlloc_Read/{sz}')
        heap_read.append(round(b['cpu_time'], 2) if b else 0)
    data['serialization'] = {
        'labels': ser_labels,
        'fb_build': fb_build, 'heap_build': heap_build,
        'fb_read': fb_read, 'heap_read': heap_read,
    }

    # ── Serialization version ──
    if has_baseline:
        sv_names = [
            ('BM_FlatBuffers_Build/64', 'FB Build 64B'),
            ('BM_FlatBuffers_Build/512', 'FB Build 512B'),
            ('BM_FlatBuffers_Build/4096', 'FB Build 4KB'),
            ('BM_HeapAlloc_Build/64', 'Heap Build 64B'),
            ('BM_HeapAlloc_Build/512', 'Heap Build 512B'),
            ('BM_HeapAlloc_Build/4096', 'Heap Build 4KB'),
            ('BM_FlatBuffers_Read/64', 'FB Read 64B'),
            ('BM_FlatBuffers_Read/512', 'FB Read 512B'),
            ('BM_FlatBuffers_Read/4096', 'FB Read 4KB'),
            ('BM_HeapAlloc_Read/64', 'Heap Read 64B'),
            ('BM_HeapAlloc_Read/512', 'Heap Read 512B'),
            ('BM_HeapAlloc_Read/4096', 'Heap Read 4KB'),
        ]
        sv_labels, sv_cur, sv_base = [], [], []
        for bm_name, label in sv_names:
            bc = find(cur, 'serialization', bm_name)
            if bc:
                sv_labels.append(label)
                sv_cur.append(round(bc['cpu_time'], 2))
                bb = find(base, 'serialization', bm_name)
                sv_base.append(round(bb['cpu_time'], 2) if bb else 0)
        data['serialization_version'] = {
            'labels': sv_labels, 'cur': sv_cur, 'base': sv_base,
        }

    # ── Dispatcher lookup ──
    hs = [10, 100, 1000]
    disp_labels, disp_vals = [], []
    for h in hs:
        b = find(cur, 'dispatcher', f'BM_Dispatcher_Lookup/{h}')
        if b:
            disp_labels.append(f'{h} handlers')
            disp_vals.append(round(b['cpu_time'], 2))
    data['dispatcher_lookup'] = {'labels': disp_labels, 'values': disp_vals}

    # ── Dispatcher session lookup (flat_map vs std::unordered_map) ──
    session_sizes = [100, 1000, 10000, 100000]
    sl_labels, flat_sl, std_sl = [], [], []
    for ss in session_sizes:
        bf = find(cur, 'dispatcher', f'BM_FlatMap_SessionLookup/{ss}')
        bs = find(cur, 'dispatcher', f'BM_StdMap_SessionLookup/{ss}')
        if bf and bs:
            sl_labels.append(f'{ss:,} sessions')
            flat_sl.append(round(bf['cpu_time'], 2))
            std_sl.append(round(bs['cpu_time'], 2))
    # Fallback to regular Lookup if SessionLookup not found
    if not sl_labels:
        for h in hs:
            bf = find(cur, 'dispatcher', f'BM_FlatMap_Lookup/{h}')
            bs = find(cur, 'dispatcher', f'BM_StdMap_Lookup/{h}')
            if bf and bs:
                sl_labels.append(f'{h} entries')
                flat_sl.append(round(bf['cpu_time'], 2))
                std_sl.append(round(bs['cpu_time'], 2))
    data['dispatcher_session_lookup'] = {
        'labels': sl_labels, 'flat': flat_sl, 'std': std_sl,
    }

    # ── Dispatcher iteration ──
    iter_sizes = [100, 1000, 10000]
    it_labels, flat_it, std_it = [], [], []
    for ss in iter_sizes:
        bf = find(cur, 'dispatcher', f'BM_FlatMap_SessionIterate/{ss}')
        bs = find(cur, 'dispatcher', f'BM_StdMap_SessionIterate/{ss}')
        if bf and bs:
            it_labels.append(f'{ss:,} entries')
            flat_it.append(round(bf['cpu_time'], 2))
            std_it.append(round(bs['cpu_time'], 2))
    data['dispatcher_iteration'] = {
        'labels': it_labels, 'flat': flat_it, 'std': std_it,
    }

    # ── Dispatcher version ──
    if has_baseline:
        dv_labels, dv_cur, dv_base = [], [], []
        for h in hs:
            bc = find(cur, 'dispatcher', f'BM_Dispatcher_Lookup/{h}')
            if bc:
                dv_labels.append(f'Dispatcher {h}')
                dv_cur.append(round(bc['cpu_time'], 2))
                bb = find(base, 'dispatcher', f'BM_Dispatcher_Lookup/{h}')
                dv_base.append(round(bb['cpu_time'], 2) if bb else 0)
        data['dispatcher_version'] = {
            'labels': dv_labels, 'cur': dv_cur, 'base': dv_base,
        }

    # ── Session ptr comparison ──
    b_intr = find(cur, 'session_lifecycle', 'BM_SessionPtr_Copy')
    b_shared = find(cur, 'session_lifecycle', 'BM_SharedPtr_Copy')
    data['session_ptr'] = {
        'intr': round(b_intr['cpu_time'], 2) if b_intr else 0,
        'shared': round(b_shared['cpu_time'], 2) if b_shared else 0,
    }

    # ── Session version ──
    if has_baseline:
        st_names = [
            ('timing_wheel', 'BM_TimingWheel_ScheduleOnly', 'Schedule'),
            ('timing_wheel', 'BM_TimingWheel_ScheduleAndExpire', 'Sched+Expire'),
            ('session_lifecycle', 'BM_SessionPtr_Copy', 'intrusive_ptr Copy'),
            ('session_lifecycle', 'BM_SharedPtr_Copy', 'shared_ptr Copy'),
        ]
        stv_labels, stv_cur, stv_base = [], [], []
        for stem, bm_name, label in st_names:
            bc = find(cur, stem, bm_name)
            if bc:
                stv_labels.append(label)
                stv_cur.append(round(bc['cpu_time'], 2))
                bb = find(base, stem, bm_name)
                stv_base.append(round(bb['cpu_time'], 2) if bb else 0)
        data['session_version'] = {
            'labels': stv_labels, 'cur': stv_cur, 'base': stv_base,
        }

    # ── Buffer methodology ──
    buf_sizes = [64, 512, 4096]
    buf_labels = ['64B', '512B', '4KB']
    zc_v, naive_v = [], []
    for sz in buf_sizes:
        b = find(cur, 'ring_buffer', f'BM_RingBuffer_WriteRead/{sz}')
        zc_v.append(round(b.get('bytes_per_second', 0) / 1e9, 2) if b else 0)
        b = find(cur, 'ring_buffer', f'BM_NaiveBuffer_CopyWrite/{sz}')
        naive_v.append(round(b.get('bytes_per_second', 0) / 1e9, 2) if b else 0)
    data['buffer_methodology'] = {
        'labels': buf_labels, 'zero_copy': zc_v, 'naive': naive_v,
    }

    # ── Buffer version ──
    if has_baseline:
        bv_names = []
        for sz in buf_sizes:
            bv_names.append((f'BM_RingBuffer_WriteRead/{sz}', f'ZeroCopy {sz}B', 'writeread'))
            bv_names.append((f'BM_RingBuffer_Linearize/{sz}', f'Linearize {sz}B', 'linearize'))
            bv_names.append((f'BM_NaiveBuffer_CopyWrite/{sz}', f'Naive {sz}B', 'naive'))
        bv_labels, bv_cur, bv_base, bv_types = [], [], [], []
        for bm_name, label, ctype in bv_names:
            bc = find(cur, 'ring_buffer', bm_name)
            if bc:
                bv_labels.append(label)
                bv_cur.append(round(bc.get('bytes_per_second', 0) / 1e9, 2))
                bv_types.append(ctype)
                bb = find(base, 'ring_buffer', bm_name)
                bv_base.append(round(bb.get('bytes_per_second', 0) / 1e9, 2) if bb else 0)
        data['buffer_version'] = {
            'labels': bv_labels, 'cur': bv_cur, 'base': bv_base, 'types': bv_types,
        }

    return data


def _build_methodology_table(cur):
    """방법론 비교 핵심 수치 테이블 데이터."""
    rows = []

    # SPSC vs MPSC
    bs = find(cur, 'spsc_queue', 'BM_SpscQueue_Throughput/1024')
    bm = find(cur, 'mpsc_queue', 'BM_MpscQueue_1P1C/1024')
    if bs and bm and bs['cpu_time'] > 0:
        ratio = bm['cpu_time'] / bs['cpu_time']
        rows.append([
            'SPSC vs MPSC (1P1C)',
            f"SPSC: {ft(bs['cpu_time'])}",
            f"MPSC: {ft(bm['cpu_time'])}",
            f'{ratio:.1f}x',
        ])

    # Slab vs malloc
    ba = find(cur, 'allocators', 'BM_SlabAllocator_AllocDealloc/64')
    bb = find(cur, 'allocators', 'BM_Malloc_AllocFree/64')
    if ba and bb and ba['cpu_time'] > 0:
        ratio = bb['cpu_time'] / ba['cpu_time']
        rows.append([
            'Slab vs malloc (64B)',
            f"Slab: {ft(ba['cpu_time'])}",
            f"malloc: {ft(bb['cpu_time'])}",
            f'{ratio:.1f}x',
        ])

    # intrusive_ptr vs shared_ptr
    b_intr = find(cur, 'session_lifecycle', 'BM_SessionPtr_Copy')
    b_shared = find(cur, 'session_lifecycle', 'BM_SharedPtr_Copy')
    if b_intr and b_shared and b_intr['cpu_time'] > 0:
        ratio = b_shared['cpu_time'] / b_intr['cpu_time']
        rows.append([
            'intrusive_ptr vs shared_ptr',
            f"intrusive: {ft(b_intr['cpu_time'])}",
            f"shared: {ft(b_shared['cpu_time'])}",
            f'{ratio:.1f}x',
        ])

    # Zero-copy vs Naive
    ba = find(cur, 'ring_buffer', 'BM_RingBuffer_WriteRead/512')
    bb = find(cur, 'ring_buffer', 'BM_NaiveBuffer_CopyWrite/512')
    if ba and bb:
        ba_tp = ba.get('bytes_per_second', 0)
        bb_tp = bb.get('bytes_per_second', 0)
        if bb_tp > 0:
            ratio = ba_tp / bb_tp
            rows.append([
                'Zero-copy vs Naive (512B)',
                f"ZeroCopy: {ba_tp/1e9:.1f} GB/s",
                f"Naive: {bb_tp/1e9:.1f} GB/s",
                f'{ratio:.1f}x',
            ])

    # flat_map vs unordered_map
    ba = find(cur, 'dispatcher', 'BM_FlatMap_Lookup/100')
    bb = find(cur, 'dispatcher', 'BM_StdMap_Lookup/100')
    if ba and bb and bb['cpu_time'] > 0:
        ratio = ba['cpu_time'] / bb['cpu_time']
        rows.append([
            'flat_map vs unordered_map (raw)',
            f"flat_map: {ft(ba['cpu_time'])}",
            f"unordered_map: {ft(bb['cpu_time'])}",
            f'{ratio:.1f}x',
        ])

    # FlatBuffers Build vs Heap Build
    fb_b = find(cur, 'serialization', 'BM_FlatBuffers_Build/512')
    heap_b = find(cur, 'serialization', 'BM_HeapAlloc_Build/512')
    if fb_b and heap_b and heap_b['cpu_time'] > 0:
        ratio = fb_b['cpu_time'] / heap_b['cpu_time']
        rows.append([
            'FlatBuffers vs Heap Build (512B)',
            f"FB: {ft(fb_b['cpu_time'])}",
            f"Heap: {ft(heap_b['cpu_time'])}",
            f'{ratio:.1f}x',
        ])

    # FlatBuffers Read vs Heap Read
    fb_r = find(cur, 'serialization', 'BM_FlatBuffers_Read/512')
    heap_r = find(cur, 'serialization', 'BM_HeapAlloc_Read/512')
    if fb_r and heap_r and fb_r['cpu_time'] > 0:
        ratio = heap_r['cpu_time'] / fb_r['cpu_time']
        rows.append([
            'FlatBuffers vs Heap Read (512B)',
            f"FB: {ft(fb_r['cpu_time'])}",
            f"Heap: {ft(heap_r['cpu_time'])}",
            f'{ratio:.1f}x',
        ])

    return rows


def _build_summary_delta(cur, base):
    """전 컴포넌트 Δ% 데이터."""
    targets = [
        ('spsc_queue', 'BM_SpscQueue_Throughput/1024', 'SPSC 1P1C'),
        ('mpsc_queue', 'BM_MpscQueue_1P1C/1024', 'MPSC 1P1C'),
        ('allocators', 'BM_SlabAllocator_AllocDealloc/64', 'Slab 64B'),
        ('ring_buffer', 'BM_RingBuffer_WriteRead/512', 'RingBuffer 512B'),
        ('frame_codec', 'BM_FrameCodec_Encode/512', 'FrameCodec Encode'),
        ('dispatcher', 'BM_Dispatcher_Lookup/100', 'Dispatcher'),
        ('timing_wheel', 'BM_TimingWheel_ScheduleOnly', 'TimingWheel'),
        ('session_lifecycle', 'BM_SessionPtr_Copy', 'SessionPtr Copy'),
        ('serialization', 'BM_FlatBuffers_Build/512', 'FlatBuffers 512B'),
        ('cross_core_message_passing', 'BM_CrossCore_PostThroughput/real_time', 'Cross-core Msg'),
        ('frame_pipeline', 'BM_FramePipeline/4096', 'Frame Pipeline'),
        ('session_throughput', 'BM_Session_EchoRoundTrip/4096', 'Session Echo'),
    ]
    result = []
    for stem, n, label in targets:
        bc = find(cur, stem, n)
        bb = find(base, stem, n)
        if bc and bb and bb.get('cpu_time', 0) > 0:
            pct = round(delta_pct(bb['cpu_time'], bc['cpu_time']), 1)
            result.append({'label': label, 'delta': pct})
    return result


def _build_integration_data(cur, base, has_baseline):
    """통합 벤치마크 데이터."""
    result = {}

    # Latency
    bc = find(cur, 'cross_core_latency', 'BM_CrossCore_Latency/iterations:10000/real_time')
    if bc:
        if bc.get('error_occurred', False):
            result['latency'] = {
                'error': True,
                'error_msg': bc.get('error_message', 'Unknown error'),
                'has_data': False,
            }
        else:
            avg_rtt = bc.get('avg_rtt_ns', bc['real_time'])
            val_us = round(avg_rtt / 1000, 2)
            lat = {'has_data': True, 'error': False, 'value_us': val_us}
            if has_baseline:
                bb = find(base, 'cross_core_latency',
                          'BM_CrossCore_Latency/iterations:10000/real_time')
                if bb and not bb.get('error_occurred', False):
                    base_rtt = bb.get('avg_rtt_ns', bb['real_time'])
                    lat['base_us'] = round(base_rtt / 1000, 2)
            result['latency'] = lat
    else:
        result['latency'] = {'has_data': False, 'error': False}

    # Throughput
    bc = find(cur, 'cross_core_message_passing', 'BM_CrossCore_PostThroughput/real_time')
    if bc:
        tp = {'value_m': round(bc.get('items_per_second', 0) / 1e6, 2)}
        if has_baseline:
            bb = find(base, 'cross_core_message_passing',
                      'BM_CrossCore_PostThroughput/real_time')
            if bb:
                tp['base_m'] = round(bb.get('items_per_second', 0) / 1e6, 2)
        result['throughput'] = tp
    else:
        result['throughput'] = {}

    # Pipeline
    pipeline_targets = [
        ('frame_pipeline', 'BM_FramePipeline/4096', 'Frame Pipeline'),
        ('session_throughput', 'BM_Session_EchoRoundTrip/4096', 'Session Echo'),
    ]
    pip_labels, pip_cur, pip_base = [], [], []
    for stem, n, label in pipeline_targets:
        bc_p = find(cur, stem, n)
        if bc_p:
            pip_labels.append(label)
            pip_cur.append(round(bc_p.get('bytes_per_second', 0) / 1e6, 1))
            if has_baseline:
                bb_p = find(base, stem, n)
                pip_base.append(round(bb_p.get('bytes_per_second', 0) / 1e6, 1) if bb_p else 0)
            else:
                pip_base.append(0)
    result['pipeline'] = {'labels': pip_labels, 'cur': pip_cur, 'base': pip_base}

    return result


# ====================================================================
# JavaScript
# ====================================================================

def _get_js(embed_data, has_baseline, summary_delta, integration_data):
    """모든 Plotly 차트를 렌더링하는 JavaScript."""

    data_json = json.dumps(embed_data, ensure_ascii=False)
    summary_json = json.dumps(summary_delta, ensure_ascii=False)
    integration_json = json.dumps(integration_data, ensure_ascii=False)

    return f'''
// ── Embedded Benchmark Data ──
var D = {data_json};
var SUMMARY = {summary_json};
var INTEG = {integration_json};
var HAS_BASELINE = {'true' if has_baseline else 'false'};

// ── Colors ──
var C = {{
  primary: '#2E8EB0',
  accent: '#F59E0B',
  success: '#34D399',
  danger: '#EF4444',
  purple: '#A78BFA',
  baseline: '#64748B',
  bg: '#1E293B',
  grid: 'rgba(255,255,255,0.05)',
}};

var LAYOUT_COMMON = {{
  paper_bgcolor: C.bg,
  plot_bgcolor: C.bg,
  font: {{ family: 'system-ui, -apple-system, sans-serif', color: '#F1F5F9' }},
  margin: {{ l: 60, r: 30, t: 90, b: 60 }},
  xaxis: {{ gridcolor: C.grid, gridwidth: 0.5 }},
  yaxis: {{ gridcolor: C.grid, gridwidth: 0.5 }},
  hoverlabel: {{ bgcolor: '#334155', font: {{ size: 13, color: '#F1F5F9' }}, bordercolor: 'rgba(255,255,255,0.1)' }},
}};

function mergeLayout(overrides) {{
  var result = JSON.parse(JSON.stringify(LAYOUT_COMMON));
  for (var key in overrides) {{
    if (typeof overrides[key] === 'object' && !Array.isArray(overrides[key]) && result[key]) {{
      for (var k2 in overrides[key]) {{
        result[key][k2] = overrides[key][k2];
      }}
    }} else {{
      result[key] = overrides[key];
    }}
  }}
  return result;
}}

function formatNs(ns) {{
  if (ns >= 1e6) return (ns / 1e6).toFixed(2) + ' ms';
  if (ns >= 1e3) return (ns / 1e3).toFixed(1) + ' us';
  return ns.toFixed(1) + ' ns';
}}

function annoText(base, cur, isTime) {{
  if (base === 0) return '';
  var pct = ((cur - base) / base) * 100;
  if (isTime) {{
    return (pct < 0 ? '' : '+') + pct.toFixed(1) + '%';
  }} else {{
    var tpPct = ((cur - base) / base) * 100;
    return (tpPct > 0 ? '+' : '') + tpPct.toFixed(1) + '%';
  }}
}}

function deltaColor(pct, isTime) {{
  if (isTime) return pct < 0 ? C.success : (pct > 0 ? C.danger : C.baseline);
  return pct > 0 ? C.success : (pct < 0 ? C.danger : C.baseline);
}}

// =====================================================
// Section 2: Queue
// =====================================================
(function() {{
  var d = D.queue_methodology;
  if (!d || !d.labels.length) return;

  var traces = [
    {{
      x: d.labels, y: d.spsc, name: 'SPSC (wait-free)', type: 'bar',
      marker: {{ color: C.primary }},
      text: d.spsc.map(function(v) {{ return formatNs(v); }}),
      textposition: 'outside',
      hovertemplate: '%{{x}}<br>SPSC: %{{y:.1f}} ns<extra></extra>',
    }},
    {{
      x: d.labels, y: d.mpsc, name: 'MPSC (lock-free)', type: 'bar',
      marker: {{ color: C.accent }},
      text: d.mpsc.map(function(v) {{ return formatNs(v); }}),
      textposition: 'outside',
      hovertemplate: '%{{x}}<br>MPSC: %{{y:.1f}} ns<extra></extra>',
    }},
  ];

  // Add ratio annotations
  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.spsc[i] > 0) {{
      var ratio = d.mpsc[i] / d.spsc[i];
      var pctDiff = ((d.mpsc[i] - d.spsc[i]) / d.mpsc[i]) * 100;
      annotations.push({{
        x: d.labels[i], y: Math.max(d.spsc[i], d.mpsc[i]) * 1.15,
        text: 'SPSC ' + (pctDiff > 0 ? '+' : '') + pctDiff.toFixed(0) + '%',
        showarrow: false, font: {{ color: C.success, size: 13, family: 'system-ui' }},
      }});
    }}
  }}

  var maxY = Math.max.apply(null, d.spsc.concat(d.mpsc));
  Plotly.newPlot('chart-queue-methodology', traces, mergeLayout({{
    title: {{ text: 'SPSC vs MPSC', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'CPU Time (ns)', range: [0, maxY * 1.35] }},
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// Queue version comparison
(function() {{
  if (!HAS_BASELINE || !D.queue_version) return;
  var d = D.queue_version;
  if (!d.labels.length) return;

  var traces = [
    {{ x: d.labels, y: d.spsc_base, name: 'SPSC (base)', type: 'bar', marker: {{ color: '#1B5E7B' }} }},
    {{ x: d.labels, y: d.spsc_cur, name: 'SPSC (current)', type: 'bar', marker: {{ color: C.primary }} }},
    {{ x: d.labels, y: d.mpsc_base, name: 'MPSC (base)', type: 'bar', marker: {{ color: '#A16207' }} }},
    {{ x: d.labels, y: d.mpsc_cur, name: 'MPSC (current)', type: 'bar', marker: {{ color: C.accent }} }},
  ];

  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.spsc_base[i] > 0) {{
      var pct = ((d.spsc_cur[i] - d.spsc_base[i]) / d.spsc_base[i]) * 100;
      annotations.push({{
        x: d.labels[i], y: Math.max(d.spsc_cur[i], d.spsc_base[i]) * 1.1,
        text: (pct < 0 ? '' : '+') + pct.toFixed(1) + '%',
        showarrow: false,
        font: {{ color: deltaColor(pct, true), size: 12 }},
        xshift: -30,
      }});
    }}
    if (d.mpsc_base[i] > 0) {{
      var pct2 = ((d.mpsc_cur[i] - d.mpsc_base[i]) / d.mpsc_base[i]) * 100;
      annotations.push({{
        x: d.labels[i], y: Math.max(d.mpsc_cur[i], d.mpsc_base[i]) * 1.1,
        text: (pct2 < 0 ? '' : '+') + pct2.toFixed(1) + '%',
        showarrow: false,
        font: {{ color: deltaColor(pct2, true), size: 12 }},
        xshift: 30,
      }});
    }}
  }}

  Plotly.newPlot('chart-queue-version', traces, mergeLayout({{
    title: {{ text: 'Queue — Version Comparison', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'CPU Time (ns)' }},
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// =====================================================
// Section 3: Allocators
// =====================================================
(function() {{
  var d = D.allocators_methodology;
  if (!d || !d.labels.length) return;

  var traces = [
    {{ x: d.labels, y: d.slab, name: 'Slab', type: 'bar', marker: {{ color: C.primary }},
       hovertemplate: '%{{x}}<br>Slab: %{{y:.1f}} ns<extra></extra>' }},
    {{ x: d.labels, y: d.bump, name: 'Bump', type: 'bar', marker: {{ color: C.accent }},
       hovertemplate: '%{{x}}<br>Bump: %{{y:.1f}} ns<extra></extra>' }},
    {{ x: d.labels, y: d.arena, name: 'Arena', type: 'bar', marker: {{ color: C.success }},
       hovertemplate: '%{{x}}<br>Arena: %{{y:.1f}} ns<extra></extra>' }},
    {{ x: d.labels, y: d.malloc, name: 'malloc/free', type: 'bar', marker: {{ color: C.danger }},
       hovertemplate: '%{{x}}<br>malloc: %{{y:.1f}} ns<extra></extra>' }},
  ];

  // make_shared horizontal line
  var shapes = [];
  if (d.make_shared > 0) {{
    shapes.push({{
      type: 'line', xref: 'paper', x0: 0, x1: 1,
      y0: d.make_shared, y1: d.make_shared,
      line: {{ color: C.purple, width: 2, dash: 'dash' }},
    }});
    traces.push({{
      x: [d.labels[0]], y: [d.make_shared],
      name: 'make_shared (' + d.make_shared.toFixed(0) + ' ns)',
      type: 'scatter', mode: 'markers',
      marker: {{ color: C.purple, size: 0 }},
      showlegend: true,
    }});
  }}

  // Ratio annotations
  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.slab[i] > 0 && d.malloc[i] > 0) {{
      var ratio = d.malloc[i] / d.slab[i];
      annotations.push({{
        x: d.labels[i],
        y: Math.max(d.slab[i], d.bump[i], d.arena[i], d.malloc[i]) * 1.08,
        text: 'malloc/' + ratio.toFixed(1) + 'x',
        showarrow: false,
        font: {{ color: C.primary, size: 12 }},
      }});
    }}
  }}

  var allVals = d.slab.concat(d.bump, d.arena, d.malloc);
  var maxY = Math.max.apply(null, allVals);
  if (d.make_shared > maxY) maxY = d.make_shared;

  Plotly.newPlot('chart-allocators-methodology', traces, mergeLayout({{
    title: {{ text: 'Memory Allocator Comparison', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'CPU Time (ns)', range: [0, maxY * 1.35] }},
    shapes: shapes,
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// Allocators version
(function() {{
  if (!HAS_BASELINE || !D.allocators_version) return;
  var d = D.allocators_version;
  if (!d.labels.length || !d.base.some(function(v) {{ return v > 0; }})) return;

  var colorMap = {{ slab: C.primary, bump: C.accent, arena: C.success }};
  var baseColorMap = {{ slab: '#1B5E7B', bump: '#A16207', arena: '#166534' }};

  var baseCols = d.types.map(function(t) {{ return baseColorMap[t]; }});
  var curCols = d.types.map(function(t) {{ return colorMap[t]; }});

  var traces = [
    {{ x: d.labels, y: d.base, name: 'Baseline', type: 'bar', marker: {{ color: baseCols }} }},
    {{ x: d.labels, y: d.cur, name: 'Current', type: 'bar', marker: {{ color: curCols }} }},
  ];

  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.base[i] > 0) {{
      var pct = ((d.cur[i] - d.base[i]) / d.base[i]) * 100;
      annotations.push({{
        x: d.labels[i], y: Math.max(d.cur[i], d.base[i]) * 1.08,
        text: (pct < 0 ? '' : '+') + pct.toFixed(1) + '%',
        showarrow: false, font: {{ color: deltaColor(pct, true), size: 11 }},
      }});
    }}
  }}

  Plotly.newPlot('chart-allocators-version', traces, mergeLayout({{
    title: {{ text: 'Allocators — Version Comparison', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'CPU Time (ns)' }},
    xaxis: {{ tickangle: -45 }},
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// =====================================================
// Section 4: Frame Codec
// =====================================================
(function() {{
  var d = D.frame_scaling;
  if (!d || (!d.enc_x.length && !d.dec_x.length)) return;

  var traces = [];
  if (d.enc_x.length) {{
    traces.push({{
      x: d.enc_x, y: d.enc_y, name: 'Encode', type: 'scatter', mode: 'lines+markers+text',
      line: {{ color: C.primary, width: 3 }},
      marker: {{ color: C.primary, size: 10 }},
      text: d.enc_y.map(function(v) {{ return v.toFixed(1) + ' GB/s'; }}),
      textposition: 'top center',
      textfont: {{ size: 12 }},
      fill: 'tozeroy', fillcolor: 'rgba(46,142,176,0.15)',
      hovertemplate: '%{{x}}B<br>Encode: %{{y:.2f}} GB/s<extra></extra>',
    }});
  }}
  if (d.dec_x.length) {{
    traces.push({{
      x: d.dec_x, y: d.dec_y, name: 'Decode', type: 'scatter', mode: 'lines+markers+text',
      line: {{ color: C.accent, width: 3 }},
      marker: {{ color: C.accent, size: 10 }},
      text: d.dec_y.map(function(v) {{ return v.toFixed(1) + ' GB/s'; }}),
      textposition: 'bottom center',
      textfont: {{ size: 12 }},
      fill: 'tozeroy', fillcolor: 'rgba(245,158,11,0.12)',
      hovertemplate: '%{{x}}B<br>Decode: %{{y:.2f}} GB/s<extra></extra>',
    }});
  }}

  Plotly.newPlot('chart-frame-scaling', traces, mergeLayout({{
    title: {{ text: 'FrameCodec Throughput Scaling', font: {{ size: 16 }} }},
    xaxis: {{
      title: 'Payload Size', type: 'log',
      tickvals: d.enc_x.length ? d.enc_x : d.dec_x,
      ticktext: d.x_labels,
    }},
    yaxis: {{ title: 'Throughput (GB/s)' }},
    legend: {{ orientation: 'h', y: 1.1, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// Frame version
(function() {{
  if (!HAS_BASELINE || !D.frame_version) return;
  var d = D.frame_version;
  if (!d.labels.length || !d.base.some(function(v) {{ return v > 0; }})) return;

  var curCols = d.types.map(function(t) {{ return t === 'encode' ? C.primary : C.accent; }});
  var baseCols = d.types.map(function(t) {{ return t === 'encode' ? '#1B5E7B' : '#A16207'; }});

  var traces = [
    {{ x: d.labels, y: d.base, name: 'Baseline', type: 'bar', marker: {{ color: baseCols }} }},
    {{ x: d.labels, y: d.cur, name: 'Current', type: 'bar', marker: {{ color: curCols }} }},
  ];

  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.base[i] > 0) {{
      var pct = ((d.cur[i] - d.base[i]) / d.base[i]) * 100;
      annotations.push({{
        x: d.labels[i], y: Math.max(d.cur[i], d.base[i]) * 1.08,
        text: (pct > 0 ? '+' : '') + pct.toFixed(1) + '%',
        showarrow: false, font: {{ color: deltaColor(pct, false), size: 11 }},
      }});
    }}
  }}

  Plotly.newPlot('chart-frame-version', traces, mergeLayout({{
    title: {{ text: 'FrameCodec — Version Comparison', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'Throughput (GB/s)' }},
    xaxis: {{ tickangle: -45 }},
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// =====================================================
// Section 5: Serialization
// =====================================================
(function() {{
  var d = D.serialization;
  if (!d) return;

  // Build chart (left)
  if (d.fb_build.some(function(v) {{ return v > 0; }}) || d.heap_build.some(function(v) {{ return v > 0; }})) {{
    var annotations = [];
    for (var i = 0; i < d.labels.length; i++) {{
      if (d.fb_build[i] > 0 && d.heap_build[i] > 0) {{
        var ratio = d.fb_build[i] / d.heap_build[i];
        annotations.push({{
          x: d.labels[i], y: Math.max(d.fb_build[i], d.heap_build[i]) * 1.08,
          text: ratio.toFixed(1) + 'x',
          showarrow: false, font: {{ color: C.primary, size: 12 }},
        }});
      }}
    }}
    var maxBuild = Math.max.apply(null, d.fb_build.concat(d.heap_build));

    Plotly.newPlot('chart-ser-build', [
      {{ x: d.labels, y: d.fb_build, name: 'FlatBuffers', type: 'bar', marker: {{ color: C.primary }},
         hovertemplate: '%{{x}}<br>FB: %{{y:.1f}} ns<extra></extra>' }},
      {{ x: d.labels, y: d.heap_build, name: 'new + memcpy', type: 'bar', marker: {{ color: C.accent }},
         hovertemplate: '%{{x}}<br>Heap: %{{y:.1f}} ns<extra></extra>' }},
    ], mergeLayout({{
      title: {{ text: 'Build (Serialization)', font: {{ size: 15 }} }},
      barmode: 'group',
      yaxis: {{ title: 'CPU Time (ns)', range: [0, maxBuild * 1.3] }},
      annotations: annotations,
      legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
      margin: {{ l: 50, r: 20, t: 90, b: 50 }},
    }}), {{ responsive: true }});
  }}

  // Read chart (right)
  if (d.fb_read.some(function(v) {{ return v > 0; }}) || d.heap_read.some(function(v) {{ return v > 0; }})) {{
    var annotations2 = [];
    for (var i = 0; i < d.labels.length; i++) {{
      if (d.fb_read[i] > 0 && d.heap_read[i] > 0) {{
        var ratio2 = d.heap_read[i] / d.fb_read[i];
        annotations2.push({{
          x: d.labels[i], y: Math.max(d.fb_read[i], d.heap_read[i]) * 1.08,
          text: ratio2.toFixed(1) + 'x',
          showarrow: false, font: {{ color: C.primary, size: 12, family: 'system-ui' }},
        }});
      }}
    }}
    var maxRead = Math.max.apply(null, d.fb_read.concat(d.heap_read));

    Plotly.newPlot('chart-ser-read', [
      {{ x: d.labels, y: d.fb_read, name: 'FlatBuffers (zero-copy)', type: 'bar', marker: {{ color: C.primary }},
         hovertemplate: '%{{x}}<br>FB: %{{y:.1f}} ns<extra></extra>' }},
      {{ x: d.labels, y: d.heap_read, name: 'memcpy deserialization', type: 'bar', marker: {{ color: C.accent }},
         hovertemplate: '%{{x}}<br>Heap: %{{y:.1f}} ns<extra></extra>' }},
    ], mergeLayout({{
      title: {{ text: 'Read (Deserialization)', font: {{ size: 15 }} }},
      barmode: 'group',
      yaxis: {{ title: 'CPU Time (ns)', range: [0, maxRead * 1.3] }},
      annotations: annotations2,
      legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
      margin: {{ l: 50, r: 20, t: 90, b: 50 }},
    }}), {{ responsive: true }});
  }}
}})();

// Serialization version
(function() {{
  if (!HAS_BASELINE || !D.serialization_version) return;
  var d = D.serialization_version;
  if (!d.labels.length || !d.base.some(function(v) {{ return v > 0; }})) return;

  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.base[i] > 0) {{
      var pct = ((d.cur[i] - d.base[i]) / d.base[i]) * 100;
      annotations.push({{
        x: d.labels[i], y: Math.max(d.cur[i], d.base[i]) * 1.08,
        text: (pct < 0 ? '' : '+') + pct.toFixed(1) + '%',
        showarrow: false, font: {{ color: deltaColor(pct, true), size: 11 }},
      }});
    }}
  }}

  Plotly.newPlot('chart-ser-version', [
    {{ x: d.labels, y: d.base, name: 'Baseline', type: 'bar', marker: {{ color: C.baseline }} }},
    {{ x: d.labels, y: d.cur, name: 'Current', type: 'bar', marker: {{ color: C.primary }} }},
  ], mergeLayout({{
    title: {{ text: 'Serialization — Version Comparison', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'CPU Time (ns)' }},
    xaxis: {{ tickangle: -45 }},
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// =====================================================
// Section 6: Dispatcher — Iteration (main scenario)
// =====================================================

// Dispatcher iteration
(function() {{
  var d = D.dispatcher_iteration;
  if (!d || !d.labels.length) return;

  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.flat[i] > 0 && d.std[i] > 0) {{
      var ratio = d.std[i] / d.flat[i];
      annotations.push({{
        x: d.labels[i], y: Math.max(d.flat[i], d.std[i]) * 1.06,
        text: ratio.toFixed(1) + 'x',
        showarrow: false, font: {{ color: C.primary, size: 12 }},
      }});
    }}
  }}
  var maxY = Math.max.apply(null, d.flat.concat(d.std));

  Plotly.newPlot('chart-dispatcher-iteration', [
    {{ x: d.labels, y: d.flat, name: 'flat_map', type: 'bar', marker: {{ color: C.primary }},
       hovertemplate: '%{{x}}<br>flat_map: %{{y:.2f}} ns<extra></extra>' }},
    {{ x: d.labels, y: d.std, name: 'std::unordered_map', type: 'bar', marker: {{ color: C.accent }},
       hovertemplate: '%{{x}}<br>unordered_map: %{{y:.2f}} ns<extra></extra>' }},
  ], mergeLayout({{
    title: {{ text: 'flat_map vs std::unordered_map — Iteration', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'CPU Time (ns)', range: [0, maxY * 1.3] }},
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// Dispatcher version
(function() {{
  if (!HAS_BASELINE || !D.dispatcher_version) return;
  var d = D.dispatcher_version;
  if (!d.labels.length || !d.base.some(function(v) {{ return v > 0; }})) return;

  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.base[i] > 0) {{
      var pct = ((d.cur[i] - d.base[i]) / d.base[i]) * 100;
      annotations.push({{
        x: d.labels[i], y: Math.max(d.cur[i], d.base[i]) * 1.08,
        text: (pct < 0 ? '' : '+') + pct.toFixed(1) + '%',
        showarrow: false, font: {{ color: deltaColor(pct, true), size: 11 }},
      }});
    }}
  }}

  Plotly.newPlot('chart-dispatcher-version', [
    {{ x: d.labels, y: d.base, name: 'Baseline', type: 'bar', marker: {{ color: C.baseline }} }},
    {{ x: d.labels, y: d.cur, name: 'Current', type: 'bar', marker: {{ color: C.primary }} }},
  ], mergeLayout({{
    title: {{ text: 'Dispatcher — Version Comparison', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'CPU Time (ns)' }},
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// =====================================================
// Section 7: Session & Timer
// =====================================================
(function() {{
  var d = D.session_ptr;
  if (!d || d.intr === 0) return;

  var labels = ['intrusive_ptr'];
  var values = [d.intr];
  var colors = [C.primary];
  if (d.shared > 0) {{
    labels.push('shared_ptr');
    values.push(d.shared);
    colors.push(C.accent);
  }}

  var annotations = [];
  if (d.intr > 0 && d.shared > 0) {{
    var ratio = d.shared / d.intr;
    annotations.push({{
      x: 0.5, y: Math.max(d.intr, d.shared) * 1.12,
      xref: 'paper',
      text: 'intrusive_ptr = ' + ratio.toFixed(1) + 'x faster',
      showarrow: false,
      font: {{ color: C.primary, size: 14 }},
    }});
  }}

  Plotly.newPlot('chart-session-ptr', [
    {{
      x: labels, y: values, type: 'bar',
      marker: {{ color: colors }},
      text: values.map(function(v) {{ return formatNs(v); }}),
      textposition: 'inside',
      insidetextanchor: 'middle',
      textfont: {{ color: '#FFFFFF', size: 14 }},
      hovertemplate: '%{{x}}: %{{y:.1f}} ns<extra></extra>',
    }},
  ], mergeLayout({{
    title: {{ text: 'intrusive_ptr vs shared_ptr — Copy Cost', font: {{ size: 16 }} }},
    yaxis: {{ title: 'CPU Time (ns)', range: [0, Math.max.apply(null, values) * 1.35] }},
    showlegend: false,
    annotations: annotations,
  }}), {{ responsive: true }});
}})();

// Session version
(function() {{
  if (!HAS_BASELINE || !D.session_version) return;
  var d = D.session_version;
  if (!d.labels.length || !d.base.some(function(v) {{ return v > 0; }})) return;

  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.base[i] > 0) {{
      var pct = ((d.cur[i] - d.base[i]) / d.base[i]) * 100;
      annotations.push({{
        x: d.labels[i], y: Math.max(d.cur[i], d.base[i]) * 1.08,
        text: (pct < 0 ? '' : '+') + pct.toFixed(1) + '%',
        showarrow: false, font: {{ color: deltaColor(pct, true), size: 11 }},
      }});
    }}
  }}

  Plotly.newPlot('chart-session-version', [
    {{ x: d.labels, y: d.base, name: 'Baseline', type: 'bar', marker: {{ color: C.baseline }} }},
    {{ x: d.labels, y: d.cur, name: 'Current', type: 'bar', marker: {{ color: C.primary }} }},
  ], mergeLayout({{
    title: {{ text: 'Session & Timer — Version Comparison', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'CPU Time (ns)' }},
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// =====================================================
// Section 8: Buffer
// =====================================================
(function() {{
  var d = D.buffer_methodology;
  if (!d || !d.labels.length) return;

  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.zero_copy[i] > 0 && d.naive[i] > 0) {{
      var ratio = d.zero_copy[i] / d.naive[i];
      annotations.push({{
        x: d.labels[i], y: Math.max(d.zero_copy[i], d.naive[i]) * 1.08,
        text: ratio.toFixed(1) + 'x',
        showarrow: false, font: {{ color: C.primary, size: 12 }},
      }});
    }}
  }}
  var maxY = Math.max.apply(null, d.zero_copy.concat(d.naive));

  Plotly.newPlot('chart-buffer-methodology', [
    {{ x: d.labels, y: d.zero_copy, name: 'Zero-copy (RingBuffer)', type: 'bar',
       marker: {{ color: C.primary }},
       hovertemplate: '%{{x}}<br>Zero-copy: %{{y:.2f}} GB/s<extra></extra>' }},
    {{ x: d.labels, y: d.naive, name: 'Naive memcpy', type: 'bar',
       marker: {{ color: C.accent }},
       hovertemplate: '%{{x}}<br>Naive: %{{y:.2f}} GB/s<extra></extra>' }},
  ], mergeLayout({{
    title: {{ text: 'Zero-copy vs Naive memcpy — Throughput', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'Throughput (GB/s)', range: [0, maxY * 1.3] }},
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// Buffer version
(function() {{
  if (!HAS_BASELINE || !D.buffer_version) return;
  var d = D.buffer_version;
  if (!d.labels.length || !d.base.some(function(v) {{ return v > 0; }})) return;

  var typeColor = {{ writeread: C.primary, linearize: C.accent, naive: C.danger }};
  var typeBaseColor = {{ writeread: '#1B5E7B', linearize: '#A16207', naive: '#991B1B' }};
  var curCols = d.types.map(function(t) {{ return typeColor[t]; }});
  var baseCols = d.types.map(function(t) {{ return typeBaseColor[t]; }});

  var annotations = [];
  for (var i = 0; i < d.labels.length; i++) {{
    if (d.base[i] > 0) {{
      var pct = ((d.cur[i] - d.base[i]) / d.base[i]) * 100;
      annotations.push({{
        x: d.labels[i], y: Math.max(d.cur[i], d.base[i]) * 1.08,
        text: (pct > 0 ? '+' : '') + pct.toFixed(1) + '%',
        showarrow: false, font: {{ color: deltaColor(pct, false), size: 11 }},
      }});
    }}
  }}

  Plotly.newPlot('chart-buffer-version', [
    {{ x: d.labels, y: d.base, name: 'Baseline', type: 'bar', marker: {{ color: baseCols }} }},
    {{ x: d.labels, y: d.cur, name: 'Current', type: 'bar', marker: {{ color: curCols }} }},
  ], mergeLayout({{
    title: {{ text: 'RingBuffer — Version Comparison', font: {{ size: 16 }} }},
    barmode: 'group',
    yaxis: {{ title: 'Throughput (GB/s)' }},
    xaxis: {{ tickangle: -45 }},
    annotations: annotations,
    legend: {{ orientation: 'h', y: 1.12, x: 0.5, xanchor: 'center' }},
  }}), {{ responsive: true }});
}})();

// =====================================================
// Section 9: Summary Delta
// =====================================================
(function() {{
  if (!HAS_BASELINE || !SUMMARY.length) return;

  var labels = SUMMARY.map(function(s) {{ return s.label; }}).reverse();
  var deltas = SUMMARY.map(function(s) {{ return s.delta; }}).reverse();
  var colors = deltas.map(function(d) {{ return d < 0 ? C.success : C.danger; }});

  Plotly.newPlot('chart-summary-delta', [
    {{
      y: labels, x: deltas, type: 'bar', orientation: 'h',
      marker: {{ color: colors }},
      text: deltas.map(function(d) {{ return (d < 0 ? '' : '+') + d.toFixed(1) + '%'; }}),
      textposition: 'outside',
      textfont: {{ size: 12 }},
      hovertemplate: '%{{y}}: %{{x:+.1f}}%<extra></extra>',
    }},
  ], mergeLayout({{
    title: {{ text: 'All Components Performance Delta', font: {{ size: 16 }} }},
    xaxis: {{ title: 'Change % (negative = improvement)', zeroline: true, zerolinecolor: '#475569', zerolinewidth: 1 }},
    yaxis: {{ automargin: true }},
    showlegend: false,
    margin: {{ l: 160, r: 60, t: 90, b: 60 }},
  }}), {{ responsive: true }});
}})();

// =====================================================
// Section 10: Integration
// =====================================================
(function() {{
  // RTT
  var lat = INTEG.latency;
  if (lat && lat.has_data && !lat.error) {{
    var rttLabels = ['Current'];
    var rttVals = [lat.value_us];
    var rttColors = [C.primary];
    if (lat.base_us !== undefined) {{
      rttLabels = ['Baseline', 'Current'];
      rttVals = [lat.base_us, lat.value_us];
      rttColors = [C.baseline, C.primary];
    }}

    var annotations = [];
    if (lat.base_us !== undefined && lat.base_us > 0) {{
      var pct = ((lat.value_us - lat.base_us) / lat.base_us) * 100;
      annotations.push({{
        x: 0.5, y: -0.15, xref: 'paper', yref: 'paper',
        text: 'Delta: ' + (pct < 0 ? '' : '+') + pct.toFixed(1) + '%',
        showarrow: false,
        font: {{ color: deltaColor(pct, true), size: 13 }},
      }});
    }}

    Plotly.newPlot('chart-integration-rtt', [
      {{
        x: rttLabels, y: rttVals, type: 'bar',
        marker: {{ color: rttColors, opacity: 0.85 }},
        text: rttVals.map(function(v) {{ return v.toFixed(1) + ' us'; }}),
        textposition: 'outside',
        hovertemplate: '%{{x}}: %{{y:.1f}} us<extra></extra>',
      }},
    ], mergeLayout({{
      title: {{ text: 'Cross-core RTT', font: {{ size: 16 }} }},
      yaxis: {{ title: 'Latency (us)' }},
      showlegend: false,
      annotations: annotations,
    }}), {{ responsive: true }});
  }}

  // Throughput
  var tp = INTEG.throughput;
  if (tp && tp.value_m !== undefined) {{
    var tpLabels = ['Current'];
    var tpVals = [tp.value_m];
    var tpColors = [C.primary];
    if (tp.base_m !== undefined) {{
      tpLabels = ['Baseline', 'Current'];
      tpVals = [tp.base_m, tp.value_m];
      tpColors = [C.baseline, C.primary];
    }}

    var annotations2 = [];
    if (tp.base_m !== undefined && tp.base_m > 0) {{
      var pct2 = ((tp.value_m - tp.base_m) / tp.base_m) * 100;
      annotations2.push({{
        x: 0.5, y: -0.15, xref: 'paper', yref: 'paper',
        text: 'Delta: ' + (pct2 > 0 ? '+' : '') + pct2.toFixed(1) + '%',
        showarrow: false,
        font: {{ color: deltaColor(pct2, false), size: 13 }},
      }});
    }}

    Plotly.newPlot('chart-integration-throughput', [
      {{
        x: tpLabels, y: tpVals, type: 'bar',
        marker: {{ color: tpColors, opacity: 0.85 }},
        text: tpVals.map(function(v) {{ return v.toFixed(1) + 'M msg/s'; }}),
        textposition: 'outside',
        hovertemplate: '%{{x}}: %{{y:.1f}}M msg/s<extra></extra>',
      }},
    ], mergeLayout({{
      title: {{ text: 'Cross-core Message Throughput', font: {{ size: 16 }} }},
      yaxis: {{ title: 'Throughput (M msg/s)' }},
      showlegend: false,
      annotations: annotations2,
    }}), {{ responsive: true }});
  }}

  // Pipeline
  var pip = INTEG.pipeline;
  if (pip && pip.labels && pip.labels.length) {{
    var traces = [];
    var hasBase = pip.base.some(function(v) {{ return v > 0; }});

    if (hasBase) {{
      traces.push({{
        x: pip.labels, y: pip.base, name: 'Baseline', type: 'bar',
        marker: {{ color: C.baseline }},
      }});
      traces.push({{
        x: pip.labels, y: pip.cur, name: 'Current', type: 'bar',
        marker: {{ color: C.primary }},
      }});
    }} else {{
      traces.push({{
        x: pip.labels, y: pip.cur, type: 'bar',
        marker: {{ color: C.primary }},
        text: pip.cur.map(function(v) {{ return v.toFixed(0) + ' MB/s'; }}),
        textposition: 'outside',
        hovertemplate: '%{{x}}: %{{y:.0f}} MB/s<extra></extra>',
      }});
    }}

    var annotations3 = [];
    if (hasBase) {{
      for (var i = 0; i < pip.labels.length; i++) {{
        if (pip.base[i] > 0) {{
          var pct3 = ((pip.cur[i] - pip.base[i]) / pip.base[i]) * 100;
          annotations3.push({{
            x: pip.labels[i], y: Math.max(pip.cur[i], pip.base[i]) * 1.06,
            text: (pct3 > 0 ? '+' : '') + pct3.toFixed(1) + '%',
            showarrow: false, font: {{ color: deltaColor(pct3, false), size: 12 }},
          }});
        }}
      }}
    }}

    Plotly.newPlot('chart-integration-pipeline', traces, mergeLayout({{
      title: {{ text: 'Pipeline Throughput (4KB)', font: {{ size: 16 }} }},
      barmode: 'group',
      yaxis: {{ title: 'Throughput (MB/s)' }},
      annotations: annotations3,
      legend: {{ orientation: 'h', y: 1.1, x: 0.5, xanchor: 'center' }},
      showlegend: hasBase,
    }}), {{ responsive: true }});
  }}
}})();

// =====================================================
// Sidebar active state tracking
// =====================================================
(function() {{
  var sections = document.querySelectorAll('[id^="section-"]');
  var navItems = document.querySelectorAll('.nav-item');

  var observer = new IntersectionObserver(function(entries) {{
    entries.forEach(function(entry) {{
      if (entry.isIntersecting) {{
        navItems.forEach(function(item) {{
          item.classList.remove('active');
          if (item.getAttribute('href') === '#' + entry.target.id) {{
            item.classList.add('active');
          }}
        }});
      }}
    }});
  }}, {{
    rootMargin: '-80px 0px -60% 0px',
    threshold: 0,
  }});

  sections.forEach(function(section) {{
    observer.observe(section);
  }});

  // Smooth scroll
  navItems.forEach(function(item) {{
    item.addEventListener('click', function(e) {{
      e.preventDefault();
      var target = document.querySelector(this.getAttribute('href'));
      if (target) {{
        target.scrollIntoView({{ behavior: 'smooth', block: 'start' }});
      }}
    }});
  }});
}})();
'''


# ====================================================================
# Main
# ====================================================================

def main():
    parser = argparse.ArgumentParser(
        description='Apex Core benchmark report generator v3 -- interactive HTML')
    parser.add_argument('--data-dir', required=True,
                        help='Benchmark data root directory')
    parser.add_argument('--baseline', default=None,
                        help='Baseline version tag (optional)')
    parser.add_argument('--current', required=True,
                        help='Current version tag')
    parser.add_argument('--analysis', default=None,
                        help='Analysis JSON path')
    parser.add_argument('--output', default='report',
                        help='Output directory')
    args = parser.parse_args()

    analysis = load_analysis(args.analysis)

    # Load current version data
    cur_dir = os.path.join(args.data_dir, args.current)
    cur = load_benchmarks(cur_dir)
    if not cur:
        print(f"Error: no benchmarks found: {cur_dir}", file=sys.stderr)
        sys.exit(1)

    # Load baseline (optional)
    base = None
    if args.baseline:
        base_dir = os.path.join(args.data_dir, args.baseline)
        base = load_benchmarks(base_dir)
        if not base:
            print(f"Warning: baseline not found: {base_dir}", file=sys.stderr)
            print("Generating standalone report.", file=sys.stderr)
            base = None

    # Load metadata
    metadata = load_metadata(cur_dir)

    os.makedirs(args.output, exist_ok=True)

    mode = f"{args.baseline} -> {args.current}" if base else f"{args.current} (standalone)"
    print(f"Loaded: {mode}, {len(cur)} benchmark files")

    print("Generating HTML report...")
    html = build_html(cur, base, analysis, metadata, args.current, args.baseline)

    out_path = os.path.join(args.output, 'benchmark_report.html')
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(html)

    print(f"Report generated: {out_path}")


if __name__ == '__main__':
    main()
