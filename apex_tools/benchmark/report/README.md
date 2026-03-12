# Benchmark Report Generator

Apex Core 벤치마크 결과를 시각화하여 PDF 보고서로 생성하는 도구.

## 구조

```
apex_tools/benchmark/report/
├── generate_benchmark_report.py   ← 보고서 템플릿 (레이아웃/디자인/차트)
├── requirements.txt               ← Python 의존성
└── README.md                      ← 이 문서

apex_core/benchmark_results/
├── release/*.json                 ← Release 벤치마크 데이터
├── debug/*.json                   ← Debug 벤치마크 데이터
├── analysis.json                  ← 섹션별 분석 코멘터리
└── report/
    ├── benchmark_report.pdf       ← 생성된 PDF (9페이지)
    └── charts/*.png               ← 생성된 차트 이미지
```

**핵심 원칙: 템플릿과 분석의 분리**

- `generate_benchmark_report.py`는 **레이아웃/디자인/차트 생성** 전용 — 디자인 변경 요청이 없는 한 수정하지 않는다
- `analysis.json`은 **섹션별 분석 텍스트** — 벤치마크를 새로 돌릴 때마다 데이터에 맞게 새로 작성한다
- 벤치마크 JSON 데이터는 Google Benchmark가 자동 생성한다

## 사용법

### 의존성 설치

```bash
pip install -r apex_tools/benchmark/report/requirements.txt
```

추가로 PDF 렌더링 검증이 필요하면: `pip install PyMuPDF`

### 보고서 생성

```bash
# 분석 코멘터리 포함 (전체 보고서)
python apex_tools/benchmark/report/generate_benchmark_report.py \
    --release=apex_core/benchmark_results/release \
    --debug=apex_core/benchmark_results/debug \
    --analysis=apex_core/benchmark_results/analysis.json \
    --output=apex_core/benchmark_results/report

# 분석 없이 데이터+차트만 (--analysis 생략)
python apex_tools/benchmark/report/generate_benchmark_report.py \
    --release=apex_core/benchmark_results/release \
    --debug=apex_core/benchmark_results/debug \
    --output=apex_core/benchmark_results/report
```

### 출력물

- `report/benchmark_report.pdf` — 9페이지 PDF 보고서
- `report/charts/*.png` — 7개 차트 이미지 (PDF에 포함됨)

## PDF 보고서 구성

| 페이지 | 섹션 | 내용 |
|--------|------|------|
| 1 | 표지 | 시스템 정보 (코어, 캐시, 메모리, 빌드 환경) |
| 2 | MpscQueue | Lock-free MPSC 큐 성능 |
| 3 | RingBuffer | Zero-copy 수신 버퍼 처리량 |
| 4 | FrameCodec | 프레임 인코딩/디코딩 |
| 5 | MessageDispatcher | 핸들러 조회 (Release vs Debug) |
| 6 | SlabPool | O(1) 슬랩 메모리 풀 vs malloc |
| 7 | TimingWheel + Session | 타임아웃 관리 + 세션 생성/복사 |
| 8 | Integration | 코어 간 RTT, 처리량, 파이프라인 |
| 9 | 종합 비교 | Release/Debug 전체 비율 + CPU/IO 분류 |

각 섹션은 **데이터 테이블 → 분석 코멘터리 → 시각화 차트** 순서로 구성된다.

## analysis.json 형식

```json
{
  "mpsc_queue": "분석 텍스트 (HTML 태그 허용: <b>, <br/>)",
  "ring_buffer": "...",
  "frame_codec": "...",
  "dispatcher": "...",
  "slab_pool": "...",
  "timing_session": "...",
  "integration": "...",
  "overview": "..."
}
```

### 작성 규칙

- **언어**: 한글 베이스 + 영어 기술 용어 혼용
- **HTML 태그**: `<b>핵심 수치</b>`, `<br/><br/>` (문단 구분)
- **분량**: 각 섹션 3~10줄
- **내용**: 핵심 수치 강조 + 아키텍처 맥락에서의 의미 해석 + 실제 서비스 환경과의 연관성
- 키가 누락되면 해당 섹션의 분석 박스가 스킵됨 (데이터+차트는 그대로 출력)

## 폰트 요구사항

Windows 환경에서 `C:/Windows/Fonts/` 경로의 폰트를 사용한다:

- **맑은 고딕** (malgun.ttf, malgunbd.ttf) — PDF 본문/제목
- **나눔스퀘어** (NanumSquareR.ttf) — 보조 폰트

차트는 matplotlib의 Malgun Gothic을 사용한다.
