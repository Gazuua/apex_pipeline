# Benchmark Report Generator

Apex Core 벤치마크 결과를 시각화하여 PDF 보고서로 생성하는 도구.
**버전 간 성능 비교 + 7개 방법론 비교** 체계.

## 구조

```
apex_tools/benchmark/report/
├── generate_benchmark_report.py   ← 보고서 생성 스크립트
├── requirements.txt               ← Python 의존성
└── README.md                      ← 이 문서

docs/apex_core/benchmark/
├── v0.5.10.0/                     ← 버전별 벤치마크 데이터
│   ├── *.json                     ← Google Benchmark JSON (13개)
│   └── metadata.json              ← 시스템 정보, 커밋, 날짜
├── analysis.json                  ← 섹션별 분석 코멘터리
└── report/
    ├── benchmark_report.pdf       ← 생성된 PDF (~10페이지)
    └── charts/*.png               ← 생성된 차트 이미지 (14개)
```

## 사용법

### 의존성 설치

```bash
pip install -r apex_tools/benchmark/report/requirements.txt
```

추가로 PDF 렌더링 검증이 필요하면: `pip install PyMuPDF`

### 보고서 생성

```bash
# 버전 비교 보고서 (baseline + current)
python apex_tools/benchmark/report/generate_benchmark_report.py \
    --data-dir=docs/apex_core/benchmark \
    --baseline=v0.5.9.0 \
    --current=v0.5.10.0 \
    --analysis=docs/apex_core/benchmark/analysis.json \
    --output=docs/apex_core/benchmark/report

# 단독 보고서 (첫 벤치마크, baseline 없음)
python apex_tools/benchmark/report/generate_benchmark_report.py \
    --data-dir=docs/apex_core/benchmark \
    --current=v0.5.10.0 \
    --analysis=docs/apex_core/benchmark/analysis.json \
    --output=docs/apex_core/benchmark/report
```

### 출력물

- `report/benchmark_report.pdf` — ~10페이지 PDF 보고서
- `report/charts/*.png` — 14개 차트 이미지 (PDF에 포함됨)

## PDF 보고서 구성

| 페이지 | 섹션 | 버전 비교 | 방법론 비교 |
|--------|------|-----------|------------|
| 1 | 표지 | 시스템 정보, 버전 비교 헤더 | 핵심 변화 요약 |
| 2 | 큐 성능 | SPSC & MPSC 버전 비교 | SPSC vs MPSC |
| 3 | 메모리 할당기 | Slab/Bump/Arena 버전 비교 | 3종 vs malloc vs make_shared |
| 4 | 프레임 처리 | FrameCodec 버전 비교 | 페이로드 크기별 스케일링 |
| 5 | 직렬화 | FlatBuffers/HeapAlloc 버전 비교 | FlatBuffers vs new+memcpy |
| 6 | 디스패처 | MessageDispatcher 버전 비교 | flat_map vs unordered_map |
| 7 | 세션 & 타이머 | SessionLifecycle+TimingWheel | intrusive_ptr vs shared_ptr |
| 8 | 버퍼 | RingBuffer 버전 비교 | zero-copy vs naive memcpy |
| 9 | 통합 | Cross-core RTT/처리량/Pipeline | — |
| 10 | 종합 요약 | 전 컴포넌트 Δ% overview | 방법론 비교 핵심 수치 |

각 섹션은 **데이터 테이블 → 버전 비교 차트 → 방법론 비교 차트 → 분석 코멘터리** 순서로 구성된다.

## analysis.json 형식

```json
{
  "queue": "큐 분석 (HTML 태그 허용: <b>, <br/>)",
  "allocators": "메모리 할당기 분석...",
  "frame_codec": "프레임 코덱 분석...",
  "serialization": "직렬화 비교 분석...",
  "dispatcher": "디스패처 분석...",
  "session_timer": "세션 & 타이머 분석...",
  "ring_buffer": "링 버퍼 분석...",
  "integration": "통합 벤치마크 분석...",
  "overview": "종합 요약...",
  "version_summary": "버전 변화 요약...",
  "methodology_summary": "방법론 비교 요약..."
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

차트는 matplotlib의 Malgun Gothic을 사용한다.
