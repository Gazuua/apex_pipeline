# Benchmark Report v2 — 버전 비교 + 방법론 비교 체계

## 개요

기존 Release vs Debug 비교 보고서를 **버전 간 성능 비교 + 방법론 비교** 체계로 전면 재설계.
v0.5.10.0 SPSC mesh 전환을 기점으로, 이후 모든 버전의 성능 변화를 추적하고
아키텍처 선택의 성능 근거를 수치로 제공하는 보고서 시스템.

## 동기

- Release vs Debug 비교는 "컴파일러 최적화가 빠르다"는 당연한 결론만 제공
- 실질적으로 필요한 것은 **버전 간 성능 변화 추적** (회귀 감지, 개선 증명)
- 아키텍처 선택(SPSC vs MPSC, 커스텀 할당기 vs malloc 등)의 **방법론 비교**가 기술 문서/포트폴리오에 필요

## 1. 데이터 구조

### 저장 레이아웃

```
docs/apex_core/benchmark/
├── v0.5.10.0/                 ← 버전별 디렉토리
│   ├── mpsc_queue.json
│   ├── spsc_queue.json
│   ├── allocators.json
│   ├── ring_buffer.json
│   ├── frame_codec.json
│   ├── dispatcher.json
│   ├── timing_wheel.json
│   ├── session_lifecycle.json
│   ├── serialization.json
│   ├── cross_core_latency.json
│   ├── cross_core_message_passing.json
│   ├── frame_pipeline.json
│   ├── session_throughput.json
│   └── metadata.json
├── v0.6.0.0/                  ← 다음 버전 (향후)
│   └── ...
├── analysis.json              ← 분석 코멘터리
└── report/
    ├── benchmark_report.pdf
    └── charts/*.png
```

- 벤치마크 JSON: Google Benchmark 출력 원본 그대로 저장
- **Git 추적**: 버전당 ~50KB (13 JSON × 2~5KB), PDF ~700KB — repo 임팩트 미미
- `apex_core/benchmark_results/`는 임시 실행 공간으로 유지 (gitignore)

### metadata.json

```json
{
  "version": "v0.5.10.0",
  "commit": "21ff952",
  "date": "2026-03-21T01:15:46+09:00",
  "host_name": "DESKTOP-LS4B0FN",
  "physical_cores": 4,
  "logical_cores": 8,
  "mhz_per_cpu": 2400,
  "caches": { "l1d": "32 KB", "l2": "256 KB", "l3": "8 MB" },
  "total_ram_mb": 12168,
  "compiler": "MSVC 19.44",
  "build_type": "Release"
}
```

## 2. CLI 인터페이스

```bash
# 기존 (제거)
python generate_benchmark_report.py --release=... --debug=...

# 신규
python generate_benchmark_report.py \
    --data-dir=docs/apex_core/benchmark \
    --baseline=v0.5.9.0 \
    --current=v0.5.10.0 \
    --analysis=docs/apex_core/benchmark/analysis.json \
    --output=docs/apex_core/benchmark/report
```

- `--baseline` 생략 시 → 단독 보고서 (비교 없이 절대값만)
- `--baseline` 지정 시 → 비교 보고서 (변화율 Δ% + 비교 차트)

## 3. 보고서 구성 (10페이지)

| 페이지 | 섹션 | 버전 비교 | 방법론 비교 |
|--------|------|-----------|------------|
| 1 | 표지 | 시스템 정보, baseline↔current 버전 | 핵심 변화 요약 |
| 2 | 큐 성능 | SPSC & MPSC 버전 비교 | ①SPSC vs MPSC |
| 3 | 메모리 할당기 | Slab/Bump/Arena 버전 비교 | ②3종 vs malloc vs make_shared |
| 4 | 프레임 처리 | FrameCodec 버전 비교 | ⑥페이로드 크기별 스케일링 |
| 5 | 직렬화 | FlatBuffers/HeapAlloc 버전 비교 | ⑦FlatBuffers vs new+memcpy |
| 6 | 디스패처 | MessageDispatcher 버전 비교 | ③flat_map vs unordered_map |
| 7 | 세션 & 타이머 | SessionLifecycle+TimingWheel 버전 비교 | ④intrusive_ptr vs shared_ptr |
| 8 | 버퍼 | RingBuffer 버전 비교 | ⑤zero-copy vs naive memcpy |
| 9 | 통합 | Cross-core RTT/처리량/Pipeline 버전 비교 | — |
| 10 | 종합 요약 | 전 컴포넌트 Δ% 수평 바 차트 | 방법론 비교 핵심 수치 테이블 |

### 각 섹션 공통 레이아웃

1. **데이터 테이블** — current 버전 벤치마크 수치
2. **버전 비교 차트** — baseline vs current 그룹 바, Δ% 표기 (baseline 있을 때만)
3. **방법론 비교 차트** — 접근법 A vs B, 배수 표기 (해당 섹션만)
4. **분석 코멘터리** — analysis.json에서 로드

### 페이지 제약

10페이지는 **목표**이며 하드 제약이 아니다. 섹션 내 차트/표가 A4 한 장에 들어가지 않으면
해당 섹션에서 자동으로 다음 페이지로 넘긴다 (reportlab의 Flowable 자동 페이지 분할).
단, 차트 크기는 `figsize` 최대 `(13, 4.5)`로 제한하여 과도한 확장을 방지한다.

### analysis.json 키 (11개)

```json
{
  "queue": "SPSC & MPSC 큐 분석...",
  "allocators": "메모리 할당기 3종 분석...",
  "frame_codec": "프레임 코덱 분석...",
  "serialization": "직렬화 방식 비교 분석...",
  "dispatcher": "디스패처 분석...",
  "session_timer": "세션 & 타이머 분석...",
  "ring_buffer": "링 버퍼 분석...",
  "integration": "통합 벤치마크 분석...",
  "overview": "종합 요약...",
  "version_summary": "버전 변화 요약...",
  "methodology_summary": "방법론 비교 요약..."
}
```

## 4. 신규/확장 벤치마크

### 4.1 bench_slab_allocator → bench_allocators (리네임 + 확장)

기존:
- `BM_SlabAllocator_AllocDealloc/{64,256,1024}`
- `BM_Malloc_AllocFree/{64,256,1024}`
- `BM_MakeShared_AllocDealloc`

추가:
- `BM_BumpAllocator_Alloc/{64,256,1024}` — alloc N회 후 batch reset
- `BM_ArenaAllocator_Alloc/{64,256,1024}` — alloc N회 후 batch reset

Bump/Arena는 개별 dealloc 없는 monotonic 할당기. "alloc+dealloc per op" vs "alloc-only + batch reset" 패턴 차이가 핵심.

### 4.2 bench_dispatcher — std::unordered_map 변형

추가:
- `BM_Dispatcher_StdMap_Lookup/{10,100,1000}` — std::unordered_map 기반

### 4.3 bench_session_lifecycle — shared_ptr 변형

추가:
- `BM_SharedPtr_Copy` — std::shared_ptr 동일 패턴으로 비교

### 4.4 bench_ring_buffer — naive memcpy 변형

추가:
- `BM_NaiveBuffer_CopyWrite/{64,512,4096}` — std::vector + memcpy

### 4.5 bench_serialization (신규)

- `BM_FlatBuffers_Build/{64,512,4096}` — FlatBufferBuilder 페이로드 생성
- `BM_HeapAlloc_Build/{64,512,4096}` — new char[] + 수동 memcpy

### CMake 변경

```cmake
# 리네임
apex_add_benchmark(bench_allocators      micro/bench_allocators.cpp)

# 신규
apex_add_benchmark(bench_serialization   micro/bench_serialization.cpp)

# 삭제
# bench_slab_allocator (bench_allocators로 대체)
```

총 벤치마크 실행 파일: 12 → 13개

## 5. 차트 디자인

### 버전 비교 차트 (공통)

- 그룹 바 차트: baseline(회색 계열) vs current(프라이머리 컬러)
- 바 위에 Δ% 표기: 개선 = 초록 `-54%`, 퇴보 = 빨강 `+12%`
- baseline 없으면 current 단독 바

### 방법론 비교 차트 (섹션별)

| 섹션 | 차트 유형 | X축 | 시리즈 |
|------|----------|-----|--------|
| 큐 | 그룹 바 | 시나리오(1P1C, Backpressure, Concurrent) | SPSC, MPSC |
| 할당기 | 그룹 바 | 크기(64B, 256B, 1KB) | Slab, Bump, Arena, malloc, make_shared |
| 프레임 | 라인 플롯 | 페이로드 크기(64B→16KB) | Encode, Decode |
| 직렬화 | 그룹 바 | 크기(64B, 512B, 4KB) | FlatBuffers, new+memcpy |
| 디스패처 | 그룹 바 | 핸들러 수(10, 100, 1000) | flat_map, unordered_map |
| 세션 | 심플 바 | — | intrusive_ptr, shared_ptr |
| 버퍼 | 그룹 바 | 크기(64B, 512B, 4KB) | zero-copy, naive memcpy |

### 종합 요약 (10페이지)

- 수평 바 차트: 전 컴포넌트 Δ% 한눈에 보기
- 테이블: 방법론 비교 핵심 수치 (A vs B = Nx)

## 6. 워크플로우

### 벤치마크 실행 커맨드 패턴

각 벤치마크는 개별 커맨드로 실행하며, `--benchmark_out`으로 출력 파일명을 지정한다.
파일명이 곧 데이터 식별자 (`load_benchmarks()`가 `jf.stem`을 키로 사용).

```bash
apex_core/bin/release/bench_{name}.exe \
    --benchmark_format=json \
    --benchmark_out=apex_core/benchmark_results/{name}.json
```

### 바이너리 → 출력 파일명 대응표

| 바이너리 | 출력 파일명 | 스크립트 키 |
|----------|-----------|------------|
| `bench_mpsc_queue` | `mpsc_queue.json` | `mpsc_queue` |
| `bench_spsc_queue` | `spsc_queue.json` | `spsc_queue` |
| `bench_allocators` | `allocators.json` | `allocators` |
| `bench_ring_buffer` | `ring_buffer.json` | `ring_buffer` |
| `bench_frame_codec` | `frame_codec.json` | `frame_codec` |
| `bench_dispatcher` | `dispatcher.json` | `dispatcher` |
| `bench_timing_wheel` | `timing_wheel.json` | `timing_wheel` |
| `bench_session_lifecycle` | `session_lifecycle.json` | `session_lifecycle` |
| `bench_serialization` | `serialization.json` | `serialization` |
| `bench_cross_core_latency` | `cross_core_latency.json` | `cross_core_latency` |
| `bench_cross_core_message_passing` | `cross_core_message_passing.json` | `cross_core_message_passing` |
| `bench_frame_pipeline` | `frame_pipeline.json` | `frame_pipeline` |
| `bench_session_throughput` | `session_throughput.json` | `session_throughput` |

### 전체 순서

```
1. Release 빌드 (queue-lock.sh build release)
2. 벤치마크 13개 순차 실행 (개별 커맨드, 위 대응표 참조)
   → JSON을 apex_core/benchmark_results/ 에 임시 저장
3. metadata.json 생성 (시스템 정보 + git commit + 버전)
4. docs/apex_core/benchmark/{version}/ 으로 복사
5. analysis.json 작성 (에이전트가 결과 분석)
6. generate_benchmark_report.py 실행
7. 커밋: 데이터 + 보고서 + analysis.json
```

## 7. 마이그레이션

### 데이터 마이그레이션

v0.5.10.0은 **첫 번째 버전 데이터**이므로, 새 벤치마크(bench_allocators, bench_serialization) 구현 후
**13개 벤치마크를 전부 다시 실행**하여 완전한 데이터 셋을 확보한다.

기존 `apex_core/benchmark_results/release/` 의 부분 데이터(9개)는 폐기.
기존 `docs/apex_core/benchmark/v0.5.10.0_*.{pdf,json,png}` flat 파일도 삭제 (v2 디렉토리 구조로 대체).

| 항목 | 처리 |
|------|------|
| `apex_core/benchmark_results/release/slab_allocator.json` | 폐기 (bench_allocators 재실행으로 대체) |
| `apex_core/benchmark_results/release/slab_pool.json` | 폐기 (slab_allocator.json의 복사본이었음) |
| `docs/apex_core/benchmark/v0.5.10.0_*` (기존 flat 파일) | 삭제 (v2 디렉토리 구조로 대체) |
| `generate_benchmark_report.py` | 전면 리팩토링 |
| `bench_slab_allocator.cpp` | `bench_allocators.cpp`로 리네임 + 확장 |
| CMakeLists.txt | bench_slab_allocator → bench_allocators, bench_serialization 추가 |

### analysis.json 키 이름 변경 매핑

기존 키 이름이 대거 변경되므로, 기존 analysis.json은 폐기하고 새로 작성한다.

| 기존 키 (v1) | 신규 키 (v2) | 비고 |
|-------------|-------------|------|
| `mpsc_queue` + `spsc_queue` | `queue` | 통합 |
| `slab_pool` | `allocators` | 3종으로 확장 |
| `timing_session` | `session_timer` | 이름 변경 |
| `ring_buffer` | `ring_buffer` | 유지 |
| `frame_codec` | `frame_codec` | 유지 |
| `dispatcher` | `dispatcher` | 유지 |
| `integration` | `integration` | 유지 |
| `overview` | `overview` | 유지 |
| — (신규) | `serialization` | 신규 추가 |
| — (신규) | `version_summary` | 신규 추가 |
| — (신규) | `methodology_summary` | 신규 추가 |

## 8. 변경 파일 목록

**신규:**
- `apex_core/benchmarks/micro/bench_allocators.cpp`
- `apex_core/benchmarks/micro/bench_serialization.cpp`
- `docs/apex_core/benchmark/v0.5.10.0/metadata.json`
- `docs/apex_core/benchmark/v0.5.10.0/*.json` (13개)

**리팩토링:**
- `apex_tools/benchmark/report/generate_benchmark_report.py`
- `apex_tools/benchmark/report/README.md`

**확장:**
- `apex_core/benchmarks/micro/bench_dispatcher.cpp`
- `apex_core/benchmarks/micro/bench_ring_buffer.cpp`
- `apex_core/benchmarks/micro/bench_session_lifecycle.cpp`

**삭제:**
- `apex_core/benchmarks/micro/bench_slab_allocator.cpp` (→ bench_allocators로 대체)
- `docs/apex_core/benchmark/v0.5.10.0_benchmark_report.pdf` (v1 flat 파일)
- `docs/apex_core/benchmark/v0.5.10.0_analysis.json` (v1 flat 파일)
- `docs/apex_core/benchmark/v0.5.10.0_spsc_vs_mpsc.png` (v1 flat 파일)
- `apex_core/benchmark_results/release/slab_pool.json` (slab_allocator.json의 복사본)
- `apex_core/benchmark_results/release/slab_allocator.json` (bench_allocators 재실행으로 대체)
- `apex_core/benchmark_results/analysis.json` (v1 경로, docs/apex_core/benchmark/analysis.json으로 대체)

**갱신:**
- `apex_core/benchmarks/CMakeLists.txt` — bench_slab_allocator → bench_allocators, bench_serialization 추가
- `apex_core/benchmarks/README.md` — 벤치마크 목록/실행 가이드 전면 갱신
- `apex_core/CLAUDE.md` — 수정 범위:
  - 벤치마크 실행 개수: 11 → 13개
  - CLI 옵션: `--release/--debug` → `--baseline/--current`
  - analysis.json 경로: `apex_core/benchmark_results/` → `docs/apex_core/benchmark/`
  - analysis.json 키 목록: 9개 → 11개 (키 이름도 변경)
  - 에이전트 워크플로우 스텝 갱신
