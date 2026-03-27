# NUMA+Affinity 벤치마크 비교 보고서 — 설계서

- **백로그**: BACKLOG-264
- **브랜치**: feature/numa-benchmark
- **환경**: i5-9300H 4C/8T (Coffee Lake, 단일 NUMA 노드)
- **비교 대상**: v0.5.10.0 (baseline) / v0.6.5.0 (affinity OFF) / v0.6.5.0 (affinity ON)

## 1. 벤치마크 코드 수정

### 1.1 Affinity 헬퍼 (신규)

`apex_core/benchmarks/bench_affinity_helper.hpp` — 공통 유틸:

- `parse_affinity_option(argc, argv)` → `std::vector<CoreAssignment>` 반환
- `--affinity` 플래그 파싱: `on`이면 `CpuTopology::discover()` → 물리 코어 기반 assignment 구성, `off`/미지정이면 빈 벡터
- Google Benchmark가 미인식 플래그를 무시하므로, argv에서 직접 파싱 후 제거 → `benchmark::Initialize()`에 전달

### 1.2 수정 대상 (Integration 5개)

| 파일 | 변경 |
|------|------|
| `bench_architecture_comparison.cpp` | `CoreEngineConfig.core_assignments` = `parse_affinity_option()` 결과 |
| `bench_cross_core_latency.cpp` | 동일 |
| `bench_cross_core_message_passing.cpp` | 동일 |
| `bench_frame_pipeline.cpp` | 동일 |
| `bench_session_throughput.cpp` | 동일 |

각 파일에서 `CoreEngineConfig` 구성 부분만 수정. `core_assignments = {}` → helper 함수 결과로 교체.

### 1.3 Micro 벤치마크

변경 없음 (단일 스레드, affinity 무관).

## 2. 실행 & 데이터 수집

### 2.1 빌드

```bash
apex-agent queue build release
```

### 2.2 실행 순서

**Step 1 — Micro 벤치마크** (1회, 9개):
```
bench_mpsc_queue, bench_spsc_queue, bench_allocators, bench_ring_buffer,
bench_frame_codec, bench_dispatcher, bench_timing_wheel,
bench_serialization, bench_session_lifecycle
```
각각 `--benchmark_format=json --benchmark_out=<path>.json`

**Step 2 — Integration, affinity OFF** (5개):
```
bench_architecture_comparison --affinity=off
bench_cross_core_latency --affinity=off
bench_cross_core_message_passing --affinity=off
bench_frame_pipeline --affinity=off
bench_session_throughput --affinity=off
```

**Step 3 — Integration, affinity ON** (5개):
동일 벤치마크, `--affinity=on`, 별도 파일(`_affinity.json`)로 출력.

### 2.3 데이터 디렉토리

```
docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/
  data/
    metadata.json
    mpsc_queue.json                        # micro
    spsc_queue.json
    allocators.json
    ring_buffer.json
    frame_codec.json
    dispatcher.json
    timing_wheel.json
    serialization.json
    session_lifecycle.json
    architecture_comparison.json           # integration, affinity OFF
    cross_core_latency.json
    cross_core_message_passing.json
    frame_pipeline.json
    session_throughput.json
    architecture_comparison_affinity.json  # integration, affinity ON
    cross_core_latency_affinity.json
    cross_core_message_passing_affinity.json
    frame_pipeline_affinity.json
    session_throughput_affinity.json
```

### 2.4 metadata.json

```json
{
  "version": "v0.6.5",
  "date": "<실행일>",
  "cpu_name": "Intel Core i5-9300H (Coffee Lake)",
  "physical_cores": 4,
  "logical_cores": 8,
  "p_cores": 0,
  "e_cores": 0,
  "numa_nodes": 1,
  "affinity_support": true,
  "compiler": "MSVC, C++23",
  "build_type": "Release",
  "benchmark_library": "Google Benchmark v1.9.4"
}
```

## 3. 보고서 구조

### 3.1 형식

단일 HTML 파일, Plotly.js 차트 내장, 다크 테마 (i7-14700 보고서 스타일 답습).

### 3.2 산출물

```
docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/benchmark_report.html
```

### 3.3 보고서 구성

**1. Executive Summary**
- 환경 정보 (i5-9300H, 4C/8T, 단일 NUMA)
- 핵심 수치 카드 4개: 최대 throughput 개선율, affinity 효과(%), tail latency 변화, 스케일링 효율

**2. Version Comparison (v0.5.10.0 → v0.6.5.0)**
- Micro 벤치마크별 성능 변화 (bar chart + % 변화 표)
- Integration 벤치마크 (affinity OFF) vs v0.5.10.0 비교
- 버전 간 주요 변경사항 요약

**3. Affinity Impact Analysis (v0.6.5.0 OFF vs ON)**
- architecture_comparison: 코어 수별 throughput 곡선 (OFF/ON 오버레이)
- cross_core_latency: RTT / one-way 비교
- cross_core_message_passing: post throughput 비교
- frame_pipeline / session_throughput: 종합 처리량 비교
- Straggler Effect 분석: 물리 코어 초과(5-8코어) 구간에서 affinity 효과

**4. 3-Way Comparison**
- architecture_comparison 스케일링: v0.5.10.0 / v0.6.5.0(OFF) / v0.6.5.0(ON) 3선 오버레이
- 코어 수별 확장 효율(scaling efficiency) 테이블
- Per-core vs Shared 격차 변화 추이

**5. Conclusions & Next Steps**
- i5-9300H 관찰 요약
- i7-14700 (P+E, 20C/28T) 벤치마크 예상 효과
- 한계점 (단일 NUMA, 4물리 코어 제한)

### 3.4 분석 관측 포인트

| 관측 포인트 | 기대 |
|------------|------|
| HT 시블링 회피 | 물리 4코어 핀닝 → 논리 8코어 구간 성능 저하 방지 |
| OS 스케줄러 마이그레이션 제거 | 캐시 cold start 감소 → latency 안정화 |
| 코어 4→8 확장 시 Straggler | OFF: HT 진입으로 하락 / ON: 물리 코어만 사용하여 안정 |
| cross-core latency | 핀닝으로 일관된 코어 간 거리 → RTT 분산 감소 |
