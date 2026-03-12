#!/usr/bin/env python3
"""
generate_benchmark_report.py -- Apex Core 벤치마크 시각화 보고서 생성기.

Google Benchmark JSON → 차트(PNG) + PDF 보고서.
레이아웃/디자인/차트는 이 스크립트(템플릿)에 고정,
섹션별 분석 코멘터리는 외부 analysis.json에서 로드.

Usage:
    python generate_benchmark_report.py \
        --release=apex_core/benchmark_results/release \
        --debug=apex_core/benchmark_results/debug \
        --analysis=apex_core/benchmark_results/analysis.json \
        --output=apex_core/benchmark_results/report

analysis.json 없이 실행하면 분석 코멘터리 없이 데이터 + 차트만 생성.
"""

import argparse
import json
import os
import sys
from datetime import datetime
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.font_manager as fm
import numpy as np

from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import inch
from reportlab.lib.enums import TA_LEFT, TA_JUSTIFY
from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Spacer, Image, Table, TableStyle,
    PageBreak, Flowable
)
from reportlab.lib import colors
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

# ═══════════════════════════════════════════════════════════════
# Font Registration
# ═══════════════════════════════════════════════════════════════

FONT_DIR = 'C:/Windows/Fonts'

def register_fonts():
    """한글 폰트 등록."""
    malgun = f'{FONT_DIR}/malgun.ttf'
    malgun_bold = f'{FONT_DIR}/malgunbd.ttf'
    if not os.path.exists(malgun) or not os.path.exists(malgun_bold):
        print(f"오류: 맑은 고딕 폰트를 찾을 수 없습니다 ({FONT_DIR})", file=sys.stderr)
        print("Windows 환경에서만 실행 가능합니다.", file=sys.stderr)
        sys.exit(1)
    pdfmetrics.registerFont(TTFont('MalgunGothic', malgun))
    pdfmetrics.registerFont(TTFont('MalgunGothic-Bold', malgun_bold))
    pdfmetrics.registerFontFamily(
        'MalgunGothic', normal='MalgunGothic', bold='MalgunGothic-Bold')

# matplotlib 한글 설정
def setup_mpl_korean():
    """matplotlib에서 한글 폰트 사용."""
    font_path = f'{FONT_DIR}/malgun.ttf'
    if os.path.exists(font_path):
        fm.fontManager.addfont(font_path)
        plt.rcParams['font.family'] = 'Malgun Gothic'
    plt.rcParams['axes.unicode_minus'] = False


# ═══════════════════════════════════════════════════════════════
# Design System
# ═══════════════════════════════════════════════════════════════

class C:
    """Color palette — modern, muted tones."""
    PRIMARY     = colors.HexColor('#1B5E7B')   # deep teal
    PRIMARY_L   = colors.HexColor('#E8F4F8')   # light teal bg
    ACCENT      = colors.HexColor('#D4A843')   # warm gold
    ACCENT_L    = colors.HexColor('#FDF6E3')   # cream bg
    TEXT        = colors.HexColor('#1A1A2E')   # near-black
    TEXT_SEC    = colors.HexColor('#6B7280')   # gray-500
    BG          = colors.HexColor('#FFFFFF')
    BG_ALT      = colors.HexColor('#F7F8FA')   # subtle gray
    BORDER      = colors.HexColor('#E5E7EB')   # gray-200
    SUCCESS     = colors.HexColor('#059669')   # emerald
    WARN        = colors.HexColor('#D97706')   # amber
    DANGER      = colors.HexColor('#DC2626')   # red
    WHITE       = colors.white
    DIVIDER     = colors.HexColor('#D1D5DB')

# Chart palette
CH = {
    'release': '#1B5E7B',
    'release_l': '#3B8EAB',
    'debug':   '#D97706',
    'debug_l': '#F59E0B',
    'slab':    '#059669',
    'malloc':  '#DC2626',
    'shared':  '#7C3AED',
    'accent':  '#D4A843',
    'bg':      '#FAFBFD',
    'grid':    '#E5E7EB',
}

PAGE_W, PAGE_H = A4
MARGIN = 0.75 * inch
CONTENT_W = PAGE_W - 2 * MARGIN


# ═══════════════════════════════════════════════════════════════
# Custom Flowables
# ═══════════════════════════════════════════════════════════════

class ColorBar(Flowable):
    """Thin accent bar (used as section divider)."""
    def __init__(self, width, height=2, color=C.PRIMARY):
        super().__init__()
        self.width = width
        self.height = height
        self.color = color

    def draw(self):
        self.canv.setFillColor(self.color)
        self.canv.roundRect(0, 0, self.width, self.height, 1, fill=1, stroke=0)

    def wrap(self, aW, aH):
        return self.width, self.height + 4


class AnalysisBox(Flowable):
    """분석 코멘터리 박스 — 왼쪽에 accent bar, 배경색 + 텍스트."""
    def __init__(self, text, width, bar_color=C.ACCENT, bg_color=C.ACCENT_L):
        super().__init__()
        self.text = text
        self.box_width = width
        self.bar_color = bar_color
        self.bg_color = bg_color
        self._style = ParagraphStyle('AnalysisInner',
            fontName='MalgunGothic', fontSize=9, leading=15,
            textColor=C.TEXT, alignment=TA_JUSTIFY)
        self._para = Paragraph(self.text, self._style)

    def wrap(self, aW, aH):
        inner_w = self.box_width - 20  # bar(4) + padding(16)
        _, h = self._para.wrap(inner_w, aH)
        self._height = h + 20  # top+bottom padding
        return self.box_width, self._height

    def draw(self):
        c = self.canv
        # Background
        c.setFillColor(self.bg_color)
        c.roundRect(0, 0, self.box_width, self._height, 4, fill=1, stroke=0)
        # Left accent bar
        c.setFillColor(self.bar_color)
        c.roundRect(0, 0, 4, self._height, 2, fill=1, stroke=0)
        # Text
        self._para.drawOn(c, 14, 10)


class SectionHeader(Flowable):
    """섹션 제목 — 번호 원 + 제목 텍스트."""
    def __init__(self, number, title, width):
        super().__init__()
        self.number = str(number)
        self.title = title
        self.total_width = width

    def wrap(self, aW, aH):
        return self.total_width, 32

    def draw(self):
        c = self.canv
        # Number circle
        c.setFillColor(C.PRIMARY)
        c.circle(14, 14, 14, fill=1, stroke=0)
        c.setFillColor(C.WHITE)
        c.setFont('MalgunGothic-Bold', 13)
        tw = c.stringWidth(self.number, 'MalgunGothic-Bold', 13)
        c.drawString(14 - tw/2, 9, self.number)
        # Title text
        c.setFillColor(C.TEXT)
        c.setFont('MalgunGothic-Bold', 15)
        c.drawString(36, 7, self.title)


# ═══════════════════════════════════════════════════════════════
# JSON Helpers
# ═══════════════════════════════════════════════════════════════

def load_analysis(path: str) -> dict:
    """분석 텍스트를 JSON 파일에서 로드."""
    if not path or not os.path.exists(path):
        print(f"경고: 분석 파일 없음 ({path}) — 분석 코멘터리 없이 생성", file=sys.stderr)
        return {}
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def load_benchmarks(directory: str) -> dict:
    results = {}
    dirpath = Path(directory)
    if not dirpath.exists():
        return results
    for jf in sorted(dirpath.glob('*.json')):
        try:
            with open(jf) as f:
                data = json.load(f)
            results[jf.stem] = {
                'benchmarks': data.get('benchmarks', []),
                'context': data.get('context', {}),
            }
        except (json.JSONDecodeError, KeyError):
            pass
    return results


def ctx(results: dict) -> dict:
    for d in results.values():
        c = d.get('context', {})
        if c: return c
    return {}


def find(data: dict, stem: str, name: str):
    if stem not in data: return None
    for b in data[stem]['benchmarks']:
        if b['name'] == name: return b
    return None


def ft(ns: float) -> str:
    if ns >= 1e6: return f"{ns/1e6:.2f} ms"
    if ns >= 1e3: return f"{ns/1e3:.1f} us"
    return f"{ns:.1f} ns"


def ftp(b: dict) -> str:
    bps = b.get('bytes_per_second', 0)
    ips = b.get('items_per_second', 0)
    if bps > 1e9: return f"{bps/1e9:.1f} GB/s"
    if bps > 1e6: return f"{bps/1e6:.1f} MB/s"
    if ips > 1e6: return f"{ips/1e6:.1f}M items/s"
    if ips > 0:   return f"{ips:,.0f} items/s"
    return "-"


# ═══════════════════════════════════════════════════════════════
# Chart Generation
# ═══════════════════════════════════════════════════════════════

def _setup():
    setup_mpl_korean()
    plt.rcParams.update({
        'figure.facecolor': CH['bg'],
        'axes.facecolor': CH['bg'],
        'axes.grid': True,
        'axes.spines.top': False,
        'axes.spines.right': False,
        'axes.spines.left': True,
        'axes.spines.bottom': True,
        'grid.color': CH['grid'],
        'grid.alpha': 0.5,
        'grid.linewidth': 0.4,
        'font.size': 9,
        'axes.titlesize': 11,
        'axes.titleweight': 'bold',
        'axes.labelsize': 9,
        'axes.edgecolor': '#CCC',
    })


def _save(fig, path):
    fig.savefig(path, dpi=170, bbox_inches='tight', facecolor=CH['bg'],
                pad_inches=0.15)
    plt.close(fig)
    return path


def gen_mpsc(rel, dbg, d):
    _setup()
    names = [
        ('BM_MpscQueue_1P1C/1024', '1P1C\ncap=1K'),
        ('BM_MpscQueue_1P1C/65536', '1P1C\ncap=64K'),
        ('BM_MpscQueue_2P1C', '2P1C'),
        ('BM_MpscQueue_Backpressure', 'Backpressure'),
    ]
    labels, rv, dv = [], [], []
    for n, l in names:
        br, bd = find(rel, 'mpsc_queue', n), find(dbg, 'mpsc_queue', n)
        if br and bd:
            labels.append(l); rv.append(br['cpu_time']); dv.append(bd['cpu_time'])
    x = np.arange(len(labels)); w = 0.32
    fig, ax = plt.subplots(figsize=(8.5, 4))
    ax.bar(x - w/2, rv, w, label='Release', color=CH['release'], zorder=3)
    ax.bar(x + w/2, dv, w, label='Debug', color=CH['debug'], alpha=0.75, zorder=3)
    for i, (r, dd) in enumerate(zip(rv, dv)):
        ax.text(i, max(r, dd) * 1.08, f'{dd/r:.0f}x' if r else '', ha='center',
                fontsize=8, fontweight='bold', color='#555')
    ax.set_ylabel('CPU Time (ns)'); ax.set_xticks(x); ax.set_xticklabels(labels)
    ax.legend(framealpha=0.95, fontsize=8, loc='upper left')
    ax.set_title('MpscQueue — Release vs Debug 비교')
    return _save(fig, os.path.join(d, 'mpsc.png'))


def gen_ringbuf(rel, d):
    _setup()
    sizes = [64, 512, 4096]
    wr, lin = [], []
    for b in rel.get('ring_buffer', {}).get('benchmarks', []):
        bps = b.get('bytes_per_second', 0) / 1e9
        for sz in sizes:
            if b['name'] == f'BM_RingBuffer_WriteRead/{sz}': wr.append((sz, bps))
            elif b['name'] == f'BM_RingBuffer_Linearize/{sz}': lin.append((sz, bps))
    wr.sort(); lin.sort()
    fig, ax = plt.subplots(figsize=(7.5, 4))
    ax.plot([s for s,_ in wr], [v for _,v in wr], 'o-', color=CH['release'],
            lw=2.5, ms=8, label='WriteRead', zorder=3)
    ax.plot([s for s,_ in lin], [v for _,v in lin], 's--', color=CH['accent'],
            lw=2.5, ms=8, label='Linearize', zorder=3)
    for sz, val in wr + lin:
        ax.annotate(f'{val:.1f} GB/s', (sz, val), textcoords="offset points",
                   xytext=(0, 11), ha='center', fontsize=8, fontweight='bold')
    ax.set_xlabel('페이로드 크기'); ax.set_ylabel('처리량 (GB/s)')
    ax.set_xscale('log', base=2); ax.set_xticks(sizes)
    ax.set_xticklabels(['64B', '512B', '4KB'])
    ax.legend(framealpha=0.95, fontsize=8)
    ax.set_title('RingBuffer 처리량 — Release')
    return _save(fig, os.path.join(d, 'ringbuf.png'))


def gen_codec(rel, d):
    _setup()
    sizes = [64, 512, 4096, 16384]
    enc, dec = [], []
    for b in rel.get('frame_codec', {}).get('benchmarks', []):
        bps = b.get('bytes_per_second', 0) / 1e9
        for sz in sizes:
            if b['name'] == f'BM_FrameCodec_Encode/{sz}': enc.append((sz, bps))
            elif b['name'] == f'BM_FrameCodec_Decode/{sz}': dec.append((sz, bps))
    enc.sort(); dec.sort()
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.fill_between([s for s,_ in enc], [v for _,v in enc], alpha=0.1, color=CH['release'])
    ax.fill_between([s for s,_ in dec], [v for _,v in dec], alpha=0.1, color=CH['accent'])
    ax.plot([s for s,_ in enc], [v for _,v in enc], 'o-', color=CH['release'],
            lw=2.5, ms=8, label='Encode', zorder=3)
    ax.plot([s for s,_ in dec], [v for _,v in dec], 's-', color=CH['accent'],
            lw=2.5, ms=8, label='Decode', zorder=3)
    for sz, val in enc + dec:
        ax.annotate(f'{val:.1f}', (sz, val), textcoords="offset points",
                   xytext=(0, 11), ha='center', fontsize=8, fontweight='bold')
    ax.set_xlabel('페이로드 크기'); ax.set_ylabel('처리량 (GB/s)')
    ax.set_xscale('log', base=2); ax.set_xticks(sizes)
    ax.set_xticklabels(['64B', '512B', '4KB', '16KB'])
    ax.legend(framealpha=0.95, fontsize=8)
    ax.set_title('FrameCodec 처리량 — Release')
    return _save(fig, os.path.join(d, 'codec.png'))


def gen_dispatcher(rel, dbg, d):
    _setup()
    hs = [10, 100, 1000]
    rv = [find(rel, 'dispatcher', f'BM_Dispatcher_Lookup/{h}') for h in hs]
    dv = [find(dbg, 'dispatcher', f'BM_Dispatcher_Lookup/{h}') for h in hs]
    r_t = [b['cpu_time'] if b else 0 for b in rv]
    d_t = [b['cpu_time'] if b else 0 for b in dv]
    x = np.arange(len(hs)); w = 0.32
    fig, ax = plt.subplots(figsize=(7.5, 4))
    ax.bar(x - w/2, r_t, w, label='Release', color=CH['release'], zorder=3)
    ax.bar(x + w/2, d_t, w, label='Debug', color=CH['debug'], alpha=0.75, zorder=3)
    for i, (r, dd) in enumerate(zip(r_t, d_t)):
        ax.text(i, max(r, dd) * 1.08, f'{dd/r:.0f}x' if r else '', ha='center',
                fontsize=8, fontweight='bold', color='#555')
    ax.set_ylabel('CPU Time (ns)'); ax.set_xticks(x)
    ax.set_xticklabels([f'{h}개 핸들러' for h in hs])
    ax.legend(framealpha=0.95, fontsize=8)
    ax.set_title('MessageDispatcher Lookup — Release vs Debug')
    return _save(fig, os.path.join(d, 'dispatcher.png'))


def gen_slab(rel, d):
    _setup()
    sizes = [64, 256, 1024]
    sv, mv, shared = [], [], None
    for b in rel.get('slab_pool', {}).get('benchmarks', []):
        for sz in sizes:
            if b['name'] == f'BM_SlabPool_AllocDealloc/{sz}': sv.append(b['cpu_time'])
            elif b['name'] == f'BM_Malloc_AllocFree/{sz}': mv.append(b['cpu_time'])
        if b['name'] == 'BM_MakeShared_AllocDealloc': shared = b['cpu_time']
    if len(sv) != len(sizes) or len(mv) != len(sizes):
        return None
    x = np.arange(len(sizes)); w = 0.28
    fig, ax = plt.subplots(figsize=(8, 4.5))
    ax.bar(x - w, sv, w, label='SlabPool', color=CH['slab'], zorder=3)
    ax.bar(x, mv, w, label='malloc/free', color=CH['malloc'], zorder=3)
    if shared:
        ax.axhline(y=shared, color=CH['shared'], ls='--', lw=2, alpha=0.8,
                   label=f'make_shared ({shared:.0f} ns)', zorder=2)
    for i, (s, m) in enumerate(zip(sv, mv)):
        ax.text(i - w, max(s, m) * 1.08, f'{m/s:.0f}x' if s else '', ha='center',
                fontsize=9, fontweight='bold', color=CH['slab'])
    ax.set_ylabel('CPU Time (ns)'); ax.set_xticks(x)
    ax.set_xticklabels(['64B', '256B', '1024B'])
    ax.legend(framealpha=0.95, fontsize=8)
    ax.set_title('SlabPool vs malloc vs make_shared — Release')
    return _save(fig, os.path.join(d, 'slab.png'))


def gen_integration(rel, dbg, d):
    _setup()
    fig, axes = plt.subplots(1, 3, figsize=(13, 3.8))
    # 1) RTT
    br = find(rel, 'cross_core_latency', 'BM_CrossCore_Latency/iterations:10000/real_time')
    bd = find(dbg, 'cross_core_latency', 'BM_CrossCore_Latency/iterations:10000/real_time')
    if br and bd:
        rr = br.get('avg_rtt_ns', br['real_time'])/1000
        rd = bd.get('avg_rtt_ns', bd['real_time'])/1000
        bars = axes[0].bar(['Release', 'Debug'], [rr, rd],
                           color=[CH['release'], CH['debug']], alpha=0.85, zorder=3)
        for bar, v in zip(bars, [rr, rd]):
            axes[0].text(bar.get_x()+bar.get_width()/2, v*1.04, f'{v:.1f} us',
                        ha='center', fontsize=9, fontweight='bold')
        axes[0].set_ylabel('지연시간 (us)'); axes[0].set_title('코어 간 RTT')
    # 2) Throughput
    br = find(rel, 'cross_core_message_passing', 'BM_CrossCore_PostThroughput/real_time')
    bd = find(dbg, 'cross_core_message_passing', 'BM_CrossCore_PostThroughput/real_time')
    if br and bd:
        ir = br.get('items_per_second',0)/1e6; id_ = bd.get('items_per_second',0)/1e6
        bars = axes[1].bar(['Release', 'Debug'], [ir, id_],
                           color=[CH['release'], CH['debug']], alpha=0.85, zorder=3)
        for bar, v in zip(bars, [ir, id_]):
            axes[1].text(bar.get_x()+bar.get_width()/2, v*1.04, f'{v:.1f}M',
                        ha='center', fontsize=9, fontweight='bold')
        axes[1].set_ylabel('처리량 (M msg/s)'); axes[1].set_title('코어 간 메시지 처리량')
    # 3) Pipeline
    targets = [
        ('frame_pipeline', 'BM_FramePipeline/4096', 'Pipeline'),
        ('session_throughput', 'BM_Session_EchoRoundTrip/4096', 'Session'),
    ]
    labels, rmb, dmb = [], [], []
    for stem, n, l in targets:
        br, bd = find(rel, stem, n), find(dbg, stem, n)
        if br and bd:
            labels.append(l)
            rmb.append(br.get('bytes_per_second',0)/1e6)
            dmb.append(bd.get('bytes_per_second',0)/1e6)
    if labels:
        x = np.arange(len(labels)); w = 0.32
        axes[2].bar(x-w/2, rmb, w, label='Release', color=CH['release'], zorder=3)
        axes[2].bar(x+w/2, dmb, w, label='Debug', color=CH['debug'], alpha=0.75, zorder=3)
        for i, r in enumerate(rmb):
            axes[2].text(i, max(rmb[i], dmb[i])*1.06, f'{r:.0f} MB/s',
                        ha='center', fontsize=8, fontweight='bold', color=CH['release'])
        axes[2].set_ylabel('처리량 (MB/s)'); axes[2].set_xticks(x)
        axes[2].set_xticklabels(labels); axes[2].legend(fontsize=7, framealpha=0.9)
        axes[2].set_title('파이프라인 처리량 (4KB)')
    plt.tight_layout()
    return _save(fig, os.path.join(d, 'integration.png'))


def gen_overview(rel, dbg, d):
    _setup()
    targets = [
        ('dispatcher',  'BM_Dispatcher_Lookup/100',  'Dispatcher Lookup'),
        ('mpsc_queue',  'BM_MpscQueue_1P1C/1024',   'MPSC 1P1C'),
        ('ring_buffer', 'BM_RingBuffer_WriteRead/512','RingBuffer 512B'),
        ('frame_codec', 'BM_FrameCodec_Encode/512',  'FrameCodec Enc'),
        ('slab_pool',   'BM_SlabPool_AllocDealloc/64','SlabPool 64B'),
        ('timing_wheel','BM_TimingWheel_ScheduleOnly','TimingWheel'),
        ('session_lifecycle','BM_SessionPtr_Copy',    'SessionPtr Copy'),
        ('cross_core_message_passing','BM_CrossCore_PostThroughput/real_time','코어 간 메시지'),
        ('frame_pipeline','BM_FramePipeline/4096',    'Frame Pipeline'),
        ('session_throughput','BM_Session_EchoRoundTrip/4096','Session Echo'),
    ]
    labels, ratios = [], []
    for stem, n, l in targets:
        br, bd = find(rel, stem, n), find(dbg, stem, n)
        if br and bd and br['cpu_time'] > 0:
            labels.append(l); ratios.append(bd['cpu_time'] / br['cpu_time'])
    labels.reverse(); ratios.reverse()
    fig, ax = plt.subplots(figsize=(9, 4.5))
    bar_colors = [CH['malloc'] if r > 10 else CH['debug'] if r > 5
                  else CH['release'] for r in ratios]
    bars = ax.barh(labels, ratios, color=bar_colors, height=0.55, zorder=3)
    for bar, r in zip(bars, ratios):
        ax.text(bar.get_width() + 0.4, bar.get_y() + bar.get_height()/2,
                f'{r:.1f}x', va='center', fontsize=8, fontweight='bold')
    ax.set_xlabel('Debug / Release 비율 (낮을수록 I/O 지배적)')
    ax.set_title('컴파일러 최적화 영향도 — 전체 벤치마크')
    ax.axvline(x=1, color='#AAA', ls=':', lw=0.8)
    return _save(fig, os.path.join(d, 'overview.png'))


# ═══════════════════════════════════════════════════════════════
# PDF Builder
# ═══════════════════════════════════════════════════════════════

def S():
    """스타일 사전 생성."""
    return {
        'cover_title': ParagraphStyle('CT', fontName='MalgunGothic-Bold',
            fontSize=28, textColor=C.PRIMARY, leading=36, alignment=TA_LEFT),
        'cover_sub': ParagraphStyle('CS', fontName='MalgunGothic',
            fontSize=14, textColor=C.ACCENT, leading=20),
        'body': ParagraphStyle('BD', fontName='MalgunGothic',
            fontSize=9.5, textColor=C.TEXT, leading=15, alignment=TA_JUSTIFY),
        'body_sm': ParagraphStyle('BS', fontName='MalgunGothic',
            fontSize=8.5, textColor=C.TEXT_SEC, leading=13),
        'meta': ParagraphStyle('MT', fontName='MalgunGothic',
            fontSize=8.5, textColor=C.TEXT_SEC, leading=12),
        'h2': ParagraphStyle('H2', fontName='MalgunGothic-Bold',
            fontSize=12, textColor=C.PRIMARY, spaceBefore=10, spaceAfter=4),
        'toc_item': ParagraphStyle('TI', fontName='MalgunGothic',
            fontSize=10, textColor=C.TEXT, leading=18),
    }


def _table_style():
    return TableStyle([
        ('BACKGROUND',    (0,0), (-1,0), C.PRIMARY),
        ('TEXTCOLOR',     (0,0), (-1,0), C.WHITE),
        ('FONTNAME',      (0,0), (-1,0), 'MalgunGothic-Bold'),
        ('FONTSIZE',      (0,0), (-1,0), 8),
        ('FONTNAME',      (0,1), (-1,-1), 'MalgunGothic'),
        ('FONTSIZE',      (0,1), (-1,-1), 7.5),
        ('GRID',          (0,0), (-1,-1), 0.3, C.BORDER),
        ('TOPPADDING',    (0,0), (-1,-1), 4),
        ('BOTTOMPADDING', (0,0), (-1,-1), 4),
        ('LEFTPADDING',   (0,0), (-1,-1), 5),
        ('RIGHTPADDING',  (0,0), (-1,-1), 5),
        ('ROWBACKGROUNDS',(0,1), (-1,-1), [C.BG, C.BG_ALT]),
        ('ALIGN',         (1,0), (-1,-1), 'RIGHT'),
        ('ALIGN',         (0,0), (0,-1),  'LEFT'),
        ('VALIGN',        (0,0), (-1,-1), 'MIDDLE'),
    ])


def data_table(benchmarks):
    rows = [['벤치마크', 'CPU Time', 'Real Time', '반복 수', '처리량']]
    for b in benchmarks:
        rows.append([
            b['name'].replace('BM_', ''),
            ft(b['cpu_time']), ft(b['real_time']),
            f"{b['iterations']:,}", ftp(b),
        ])
    t = Table(rows, colWidths=[2.1*inch, 0.85*inch, 0.85*inch, 0.8*inch, 1.1*inch])
    t.setStyle(_table_style())
    return t


def _analysis_box(analysis, key, width, alt_color=False):
    """분석 텍스트가 있으면 AnalysisBox를, 없으면 빈 리스트를 반환."""
    text = analysis.get(key, '')
    if not text:
        return []
    if alt_color:
        return [AnalysisBox(text, width, C.PRIMARY, C.PRIMARY_L)]
    return [AnalysisBox(text, width)]


def build_pdf(rel, dbg, charts, analysis, out):
    doc = SimpleDocTemplate(out, pagesize=A4,
        leftMargin=MARGIN, rightMargin=MARGIN,
        topMargin=0.6*inch, bottomMargin=0.55*inch)
    s = S()
    el = []

    # ── 표지 ──
    el.append(Spacer(1, 1.8*inch))
    el.append(ColorBar(CONTENT_W, 3, C.ACCENT))
    el.append(Spacer(1, 0.3*inch))
    el.append(Paragraph("Apex Core", s['cover_title']))
    el.append(Paragraph("벤치마크 성능 보고서", s['cover_sub']))
    el.append(Spacer(1, 0.4*inch))
    el.append(ColorBar(CONTENT_W * 0.4, 1.5, C.BORDER))
    el.append(Spacer(1, 0.4*inch))

    # 시스템 정보
    c = ctx(rel)
    caches = c.get('caches', [])
    l1d = l2 = l3 = '-'
    for ca in caches:
        sz = ca.get('size', 0)
        ss = f"{sz//1024} KB" if sz < 1048576 else f"{sz//1048576} MB"
        if ca.get('level') == 1 and ca.get('type') == 'Data': l1d = ss
        elif ca.get('level') == 2: l2 = ss
        elif ca.get('level') == 3: l3 = ss

    info = [
        ['측정 일시',       datetime.now().strftime('%Y년 %m월 %d일 %H:%M')],
        ['호스트',          c.get('host_name', '-')],
        ['물리 코어',       f"{c.get('physical_cores', '-')}개"],
        ['논리 코어',       f"{c.get('logical_cores', c.get('num_cpus', '-'))}개"],
        ['CPU 클럭',       f"{c.get('mhz_per_cpu', '-')} MHz"],
        ['캐시 (L1D / L2 / L3)', f"{l1d} / {l2} / {l3}"],
        ['총 메모리',       f"{c.get('total_ram_mb', '-')} MB"],
        ['빌드 환경',       'MSVC 19.44, C++23, Release(/O2) + Debug(/Od)'],
        ['Google Benchmark', c.get('library_version', '-')],
        ['벤치마크 파일',    f"Release {len(rel)}개 + Debug {len(dbg)}개"],
    ]
    info_style = TableStyle([
        ('FONTNAME',  (0,0), (0,-1), 'MalgunGothic'),
        ('FONTNAME',  (1,0), (1,-1), 'MalgunGothic-Bold'),
        ('FONTSIZE',  (0,0), (-1,-1), 9),
        ('TEXTCOLOR', (0,0), (0,-1), C.TEXT_SEC),
        ('TEXTCOLOR', (1,0), (1,-1), C.TEXT),
        ('TOPPADDING',(0,0), (-1,-1), 2),
        ('BOTTOMPADDING',(0,0),(-1,-1), 2),
        ('LINEBELOW', (0,0), (-1,-2), 0.3, C.BORDER),
    ])
    it = Table(info, colWidths=[1.8*inch, 4*inch])
    it.setStyle(info_style)
    el.append(it)

    # ═══════════════════════════════════════════════════════════
    # 섹션 1: MpscQueue
    # ═══════════════════════════════════════════════════════════
    el.append(PageBreak())
    el.append(SectionHeader(1, 'MpscQueue — Lock-free MPSC 큐', CONTENT_W))
    el.append(Spacer(1, 8))
    el.append(data_table(rel.get('mpsc_queue', {}).get('benchmarks', [])))
    el.append(Spacer(1, 8))
    el.extend(_analysis_box(analysis, 'mpsc_queue', CONTENT_W))
    el.append(Spacer(1, 8))
    if 'mpsc' in charts:
        el.append(Image(charts['mpsc'], width=CONTENT_W, height=CONTENT_W*0.44))

    # ═══════════════════════════════════════════════════════════
    # 섹션 2: RingBuffer
    # ═══════════════════════════════════════════════════════════
    el.append(PageBreak())
    el.append(SectionHeader(2, 'RingBuffer — Zero-copy 수신 버퍼', CONTENT_W))
    el.append(Spacer(1, 8))
    el.append(data_table(rel.get('ring_buffer', {}).get('benchmarks', [])))
    el.append(Spacer(1, 8))
    el.extend(_analysis_box(analysis, 'ring_buffer', CONTENT_W))
    el.append(Spacer(1, 8))
    if 'ringbuf' in charts:
        el.append(Image(charts['ringbuf'], width=CONTENT_W*0.88, height=CONTENT_W*0.42))

    # ═══════════════════════════════════════════════════════════
    # 섹션 3: FrameCodec
    # ═══════════════════════════════════════════════════════════
    el.append(PageBreak())
    el.append(SectionHeader(3, 'FrameCodec — 프레임 인코딩/디코딩', CONTENT_W))
    el.append(Spacer(1, 8))
    el.append(data_table(rel.get('frame_codec', {}).get('benchmarks', [])))
    el.append(Spacer(1, 8))
    el.extend(_analysis_box(analysis, 'frame_codec', CONTENT_W))
    el.append(Spacer(1, 8))
    if 'codec' in charts:
        el.append(Image(charts['codec'], width=CONTENT_W*0.9, height=CONTENT_W*0.42))

    # ═══════════════════════════════════════════════════════════
    # 섹션 4: MessageDispatcher
    # ═══════════════════════════════════════════════════════════
    el.append(PageBreak())
    el.append(SectionHeader(4, 'MessageDispatcher — 핸들러 조회', CONTENT_W))
    el.append(Spacer(1, 8))
    el.append(data_table(rel.get('dispatcher', {}).get('benchmarks', [])))
    el.append(Spacer(1, 8))
    el.extend(_analysis_box(analysis, 'dispatcher', CONTENT_W))
    el.append(Spacer(1, 8))
    if 'dispatcher' in charts:
        el.append(Image(charts['dispatcher'], width=CONTENT_W*0.88, height=CONTENT_W*0.42))

    # ═══════════════════════════════════════════════════════════
    # 섹션 5: SlabPool
    # ═══════════════════════════════════════════════════════════
    el.append(PageBreak())
    el.append(SectionHeader(5, 'SlabPool — O(1) 슬랩 메모리 풀', CONTENT_W))
    el.append(Spacer(1, 8))
    el.append(data_table(rel.get('slab_pool', {}).get('benchmarks', [])))
    el.append(Spacer(1, 8))
    el.extend(_analysis_box(analysis, 'slab_pool', CONTENT_W))
    el.append(Spacer(1, 8))
    if 'slab' in charts:
        el.append(Image(charts['slab'], width=CONTENT_W*0.9, height=CONTENT_W*0.45))

    # ═══════════════════════════════════════════════════════════
    # 섹션 6: TimingWheel + Session Lifecycle
    # ═══════════════════════════════════════════════════════════
    el.append(PageBreak())
    el.append(SectionHeader(6, 'TimingWheel & Session Lifecycle', CONTENT_W))
    el.append(Spacer(1, 8))
    el.append(Paragraph("TimingWheel — O(1) 타임아웃 관리", s['h2']))
    el.append(data_table(rel.get('timing_wheel', {}).get('benchmarks', [])))
    el.append(Spacer(1, 6))
    el.append(Paragraph("Session Lifecycle — 세션 생성/복사", s['h2']))
    el.append(data_table(rel.get('session_lifecycle', {}).get('benchmarks', [])))
    el.append(Spacer(1, 8))
    el.extend(_analysis_box(analysis, 'timing_session', CONTENT_W))

    # ═══════════════════════════════════════════════════════════
    # 섹션 7: Integration
    # ═══════════════════════════════════════════════════════════
    el.append(PageBreak())
    el.append(SectionHeader(7, 'Integration — 코어 간 통신 & 파이프라인', CONTENT_W))
    el.append(Spacer(1, 8))
    for stem in ['cross_core_latency', 'cross_core_message_passing',
                 'frame_pipeline', 'session_throughput']:
        bs = rel.get(stem, {}).get('benchmarks', [])
        if bs: el.append(data_table(bs)); el.append(Spacer(1, 4))
    el.append(Spacer(1, 4))
    el.extend(_analysis_box(analysis, 'integration', CONTENT_W))
    el.append(Spacer(1, 6))
    if 'integration' in charts:
        el.append(Image(charts['integration'], width=CONTENT_W, height=CONTENT_W*0.28))

    # ═══════════════════════════════════════════════════════════
    # 섹션 8: 종합 비교
    # ═══════════════════════════════════════════════════════════
    el.append(PageBreak())
    el.append(SectionHeader(8, '종합 비교 — Release vs Debug', CONTENT_W))
    el.append(Spacer(1, 8))

    comp = [['벤치마크', 'Release', 'Debug', '비율', '분류']]
    targets = [
        ('dispatcher','BM_Dispatcher_Lookup/100','Dispatcher Lookup'),
        ('mpsc_queue','BM_MpscQueue_1P1C/1024','MPSC 1P1C'),
        ('ring_buffer','BM_RingBuffer_WriteRead/512','RingBuffer 512B'),
        ('frame_codec','BM_FrameCodec_Encode/512','FrameCodec Encode'),
        ('slab_pool','BM_SlabPool_AllocDealloc/64','SlabPool 64B'),
        ('timing_wheel','BM_TimingWheel_ScheduleOnly','TimingWheel Schedule'),
        ('session_lifecycle','BM_SessionPtr_Copy','SessionPtr Copy'),
        ('cross_core_latency','BM_CrossCore_Latency/iterations:10000/real_time','코어 간 RTT'),
        ('cross_core_message_passing','BM_CrossCore_PostThroughput/real_time','코어 간 메시지'),
        ('frame_pipeline','BM_FramePipeline/4096','Frame Pipeline 4KB'),
        ('session_throughput','BM_Session_EchoRoundTrip/4096','Session Echo 4KB'),
    ]
    for stem, n, l in targets:
        br, bd = find(rel, stem, n), find(dbg, stem, n)
        if br and bd and br['cpu_time'] > 0:
            r = bd['cpu_time'] / br['cpu_time']
            cat = 'CPU-bound' if r > 5 else 'Mixed' if r > 2 else 'I/O-bound'
            comp.append([l, ft(br['cpu_time']), ft(bd['cpu_time']), f'{r:.1f}x', cat])

    ct = Table(comp, colWidths=[1.9*inch, 0.8*inch, 0.8*inch, 0.6*inch, 0.9*inch])
    ct.setStyle(_table_style())
    el.append(ct)
    el.append(Spacer(1, 8))
    el.extend(_analysis_box(analysis, 'overview', CONTENT_W, alt_color=True))
    el.append(Spacer(1, 8))
    if 'overview' in charts:
        el.append(Image(charts['overview'], width=CONTENT_W, height=CONTENT_W*0.44))

    doc.build(el)
    print(f"PDF 보고서 생성 완료: {out}")


# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description='Apex Core 벤치마크 보고서 생성기')
    parser.add_argument('--release', required=True)
    parser.add_argument('--debug', required=True)
    parser.add_argument('--analysis', default=None,
                        help='섹션별 분석 텍스트 JSON (없으면 분석 코멘터리 없이 생성)')
    parser.add_argument('--output', default='report')
    args = parser.parse_args()

    register_fonts()
    analysis = load_analysis(args.analysis)
    rel = load_benchmarks(args.release)
    dbg = load_benchmarks(args.debug)
    if not rel:
        print("오류: Release 벤치마크 결과를 찾을 수 없습니다", file=sys.stderr)
        sys.exit(1)

    cd = os.path.join(args.output, 'charts')
    os.makedirs(cd, exist_ok=True)
    print(f"로드 완료: Release {len(rel)}개 + Debug {len(dbg)}개")

    print("차트 생성 중...")
    ch = {}
    ch['mpsc']        = gen_mpsc(rel, dbg, cd)
    ch['ringbuf']     = gen_ringbuf(rel, cd)
    ch['codec']       = gen_codec(rel, cd)
    ch['dispatcher']  = gen_dispatcher(rel, dbg, cd)
    slab_path = gen_slab(rel, cd)
    if slab_path:
        ch['slab'] = slab_path
    ch['integration'] = gen_integration(rel, dbg, cd)
    ch['overview']    = gen_overview(rel, dbg, cd)
    print(f"  {len(ch)}개 차트 생성 완료")

    print("PDF 보고서 생성 중...")
    build_pdf(rel, dbg, ch, analysis, os.path.join(args.output, 'benchmark_report.pdf'))


if __name__ == '__main__':
    main()
