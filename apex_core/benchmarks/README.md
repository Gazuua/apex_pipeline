# Apex Core Benchmarks

v0.3.0.0에서 도입된 벤치마크 인프라 가이드.
C++ 마이크로/통합 벤치마크(Google Benchmark)와 Python 보고서 생성 도구로 구성된다.

## 디렉토리 구조

```
apex_core/benchmarks/          ← C++ 벤치마크 (빌드 시스템 통합)
├── bench_main.cpp             ← 커스텀 main (SystemProfile 자동 출력)
├── bench_helpers.hpp          ← 공통 유틸 (TCP 소켓 페어, 프레임 빌더)
├── system_profile.hpp         ← CPU/RAM 자동 감지 (Windows/Linux)
├── micro/                     ← 마이크로 벤치마크 (단일 컴포넌트)
│   ├── bench_mpsc_queue.cpp
│   ├── bench_ring_buffer.cpp
│   ├── bench_frame_codec.cpp
│   ├── bench_dispatcher.cpp
│   ├── bench_timing_wheel.cpp
│   ├── bench_slab_allocator.cpp
│   └── bench_session_lifecycle.cpp
└── integration/               ← 통합 벤치마크 (컴포넌트 조합)
    ├── bench_cross_core_latency.cpp
    ├── bench_cross_core_message_passing.cpp
    ├── bench_frame_pipeline.cpp
    └── bench_session_throughput.cpp

apex_tools/benchmark/          ← Python 보고서 생성 도구
└── report/
    ├── generate_benchmark_report.py  ← 시각화 차트 + PDF 보고서 통합
    ├── requirements.txt
    └── README.md                     ← 상세 사용법

apex_core/bin/{variant}/       ← 실행 파일 출력 (debug/, release/)
apex_core/benchmark_results/   ← 결과 JSON 저장 (gitignore)
```

## 빌드

벤치마크는 **Release 빌드**(`release` 프리셋)로 측정해야 한다. Debug는 참고용.

```bash
# Windows (MSYS bash) — Release
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat release"

# Windows (MSYS bash) — Debug
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"

# Linux — Release
./apex_core/build.sh release
```

바이너리 출력: `apex_core/bin/{variant}/bench_*.exe`

- Release → `bin/release/`
- Debug → `bin/debug/`
- DLL은 빌드 시 자동 복사 (`$<TARGET_RUNTIME_DLLS>`)

## 실행

### 중요: queue-lock.sh 경유 필수

벤치마크는 **반드시 `queue-lock.sh benchmark`를 통해 실행한다.**
빌드와 동일한 lock을 공유하여 CPU 경합을 방지한다. 직접 실행은 PreToolUse hook이 차단한다.

```bash
# 올바른 실행 방법
"$(pwd)/apex_tools/queue-lock.sh" benchmark apex_core/bin/release/bench_mpsc_queue.exe

# JSON 출력 포함
"$(pwd)/apex_tools/queue-lock.sh" benchmark apex_core/bin/release/bench_mpsc_queue.exe \
    --benchmark_format=json --benchmark_out=apex_core/benchmark_results/mpsc_queue.json
```

벤치마크는 **절대 병렬로 돌리지 않는다.** CPU 경합으로 결과가 오염된다.
하나씩 순서대로 실행할 것.

### 마이크로 벤치마크

개별 컴포넌트의 단위 성능을 측정한다.

```bash
LOCK="$(pwd)/apex_tools/queue-lock.sh"

# 개별 실행 (Release)
"$LOCK" benchmark apex_core/bin/release/bench_mpsc_queue.exe
"$LOCK" benchmark apex_core/bin/release/bench_spsc_queue.exe
"$LOCK" benchmark apex_core/bin/release/bench_ring_buffer.exe
"$LOCK" benchmark apex_core/bin/release/bench_frame_codec.exe
"$LOCK" benchmark apex_core/bin/release/bench_dispatcher.exe
"$LOCK" benchmark apex_core/bin/release/bench_timing_wheel.exe
"$LOCK" benchmark apex_core/bin/release/bench_slab_allocator.exe
"$LOCK" benchmark apex_core/bin/release/bench_session_lifecycle.exe

# JSON 출력 (결과 저장용)
"$LOCK" benchmark apex_core/bin/release/bench_mpsc_queue.exe \
    --benchmark_format=json --benchmark_out=apex_core/benchmark_results/mpsc_queue.json
```

| 벤치마크 | 측정 대상 |
|----------|-----------|
| `bench_mpsc_queue` | MpscQueue 처리량 (MPSC lock-free) |
| `bench_spsc_queue` | SpscQueue 처리량/레이턴시 (SPSC wait-free) |
| `bench_ring_buffer` | RingBuffer read/write |
| `bench_frame_codec` | FrameCodec encode/decode |
| `bench_dispatcher` | MessageDispatcher 핸들러 조회 (10/100/1000 핸들러) |
| `bench_timing_wheel` | TimingWheel O(1) 타임아웃 관리 |
| `bench_slab_allocator` | SlabAllocator vs malloc vs make_shared |
| `bench_session_lifecycle` | Session 생성/소멸 사이클 |

### 통합 벤치마크

여러 컴포넌트가 조합된 파이프라인 성능을 측정한다.

```bash
LOCK="$(pwd)/apex_tools/queue-lock.sh"
"$LOCK" benchmark apex_core/bin/release/bench_cross_core_latency.exe
"$LOCK" benchmark apex_core/bin/release/bench_cross_core_message_passing.exe
"$LOCK" benchmark apex_core/bin/release/bench_frame_pipeline.exe
"$LOCK" benchmark apex_core/bin/release/bench_session_throughput.exe
"$LOCK" benchmark apex_core/bin/release/bench_architecture_comparison.exe
```

| 벤치마크 | 측정 대상 |
|----------|-----------|
| `bench_cross_core_latency` | 코어 간 메시지 왕복 지연시간 |
| `bench_cross_core_message_passing` | 코어 간 메시지 처리량 |
| `bench_frame_pipeline` | encode → ring buffer → decode → dispatch 전체 경로 |
| `bench_session_throughput` | 세션 기반 메시지 처리량 |
| `bench_architecture_comparison` | Per-core vs Shared 아키텍처 비교 |

### E2E 부하 테스트

echo 서버를 띄운 뒤 부하 테스터로 실측한다.

```bash
# 1) echo 서버 실행
apex_core/bin/debug/echo_server.exe

# 2) 부하 테스트 (별도 터미널)
apex_core/bin/release/echo_loadtest.exe --connections=100 --duration=30 --payload=256

# JSON 출력
apex_core/bin/release/echo_loadtest.exe --json > apex_core/benchmark_results/loadtest_after.json
```

**옵션:**

| 플래그 | 기본값 | 설명 |
|--------|--------|------|
| `--host=HOST` | `127.0.0.1` | 서버 주소 |
| `--port=PORT` | `9000` | 서버 포트 |
| `--connections=N` | `logical_cores * 50` | 동시 접속 수 |
| `--duration=SECS` | `30` | 측정 시간 |
| `--payload=BYTES` | `256` | 페이로드 크기 |
| `--warmup=SECS` | `3` | 워밍업 시간 |
| `--json` | off | JSON stdout 출력 |

## PDF 보고서 생성

Google Benchmark JSON 결과를 시각화 차트 + PDF 보고서로 생성한다.
상세 사용법은 `apex_tools/benchmark/report/README.md` 참조.

### 설치

```bash
pip install -r apex_tools/benchmark/report/requirements.txt
```

### 실행

```bash
python apex_tools/benchmark/report/generate_benchmark_report.py \
    --release=apex_core/benchmark_results/release \
    --debug=apex_core/benchmark_results/debug \
    --analysis=apex_core/benchmark_results/analysis.json \
    --output=apex_core/benchmark_results/report
```

- `--analysis` 생략 시 분석 코멘터리 없이 데이터+차트만 생성
- 출력: `report/benchmark_report.pdf` (9페이지) + `report/charts/*.png`

## 시스템 프로파일

모든 C++ 벤치마크는 실행 시 시스템 정보를 자동 출력한다 (`system_profile.hpp`):

```
=== System Profile ===
Physical cores: 6
Logical cores:  12
Total RAM:      32768 MB
Available RAM:  24576 MB
Bench cores:    6
======================
```

이 정보는 Google Benchmark JSON 출력에도 `context` 필드로 포함된다.

## 전체 워크플로우

### 새 버전 벤치마크 실행 + 보고서 생성

```bash
VERSION="v0.6.0.0"  # 현재 버전
BASELINE="v0.5.10.0" # 비교 대상 (이전 버전). 첫 벤치마크면 생략

# 1. Release 빌드
queue-lock.sh build release

# 2. 벤치마크 13개 순차 실행 (JSON 저장) — queue-lock.sh 경유 필수
LOCK="$(pwd)/apex_tools/queue-lock.sh"
for bench in mpsc_queue spsc_queue allocators ring_buffer frame_codec \
             dispatcher timing_wheel session_lifecycle serialization \
             cross_core_latency cross_core_message_passing \
             frame_pipeline session_throughput; do
    "$LOCK" benchmark apex_core/bin/release/bench_${bench}.exe \
        --benchmark_format=json \
        --benchmark_out=apex_core/benchmark_results/${bench}.json
done

# architecture_comparison은 별도 실행 (Per-core vs Shared 비교, 장시간 소요)
"$LOCK" benchmark apex_core/bin/release/bench_architecture_comparison.exe \
    --benchmark_format=json \
    --benchmark_out=apex_core/benchmark_results/architecture_comparison.json

# 3. 버전 디렉토리로 복사 + metadata.json 생성
mkdir -p docs/apex_core/benchmark/${VERSION}
cp apex_core/benchmark_results/*.json docs/apex_core/benchmark/${VERSION}/
# metadata.json은 에이전트가 자동 생성

# 4. analysis.json 작성 (에이전트가 결과 분석 후 작성)

# 5. PDF 보고서 생성
python apex_tools/benchmark/report/generate_benchmark_report.py \
    --data-dir=docs/apex_core/benchmark \
    --baseline=${BASELINE} \
    --current=${VERSION} \
    --analysis=docs/apex_core/benchmark/analysis.json \
    --output=docs/apex_core/benchmark/report

# baseline 없이 (첫 벤치마크):
python apex_tools/benchmark/report/generate_benchmark_report.py \
    --data-dir=docs/apex_core/benchmark \
    --current=${VERSION} \
    --analysis=docs/apex_core/benchmark/analysis.json \
    --output=docs/apex_core/benchmark/report
```

### 버전 비교 보고서 출력 형태

baseline을 지정하면 각 섹션에 **Δ% 비교 차트**가 추가된다:
- 각 컴포넌트별로 baseline vs current 그룹 바 차트
- 개선은 **초록색 -N%**, 퇴보는 **빨간색 +N%**로 표기
- 마지막 페이지에 전체 컴포넌트 변화율 수평 바 차트

### E2E 부하 테스트 (선택)

echo 서버를 띄운 뒤 부하 테스터로 실측한다.

```bash
# 1) echo 서버 실행
apex_core/bin/release/echo_server.exe

# 2) 부하 테스트 (별도 터미널)
apex_core/bin/release/echo_loadtest.exe --connections=100 --duration=30 --payload=256

# JSON 출력
apex_core/bin/release/echo_loadtest.exe --json > apex_core/benchmark_results/loadtest.json
```
```
