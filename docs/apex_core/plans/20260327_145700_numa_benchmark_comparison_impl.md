# NUMA+Affinity 벤치마크 비교 보고서 — 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** v0.5.10.0 / v0.6.5.0(affinity OFF) / v0.6.5.0(affinity ON) 3-way 벤치마크 비교 보고서 생성

**Architecture:** integration 벤치마크 3개에 `--affinity` 플래그 추가 → Release 빌드 → 벤치마크 순차 실행 (micro 9 + integration OFF 3 + integration ON 3) → 데이터 수집 → HTML 보고서 생성

**Tech Stack:** C++23, Google Benchmark, Plotly.js (HTML 차트), MSVC Release

**설계서 대비 변경**: bench_frame_pipeline, bench_session_throughput은 단일 스레드로 확인되어 affinity 비교 대상에서 제외. 3개만 해당: architecture_comparison, cross_core_latency, cross_core_message_passing.

---

## 파일 구조

| 작업 | 파일 | 역할 |
|------|------|------|
| 생성 | `apex_core/benchmarks/bench_affinity_helper.hpp` | `--affinity` 파싱 + CoreAssignment 구성 |
| 수정 | `apex_core/benchmarks/bench_main.cpp` | affinity 초기화 호출 추가 |
| 수정 | `apex_core/benchmarks/integration/bench_architecture_comparison.cpp` | 워커 스레드에 affinity 적용 |
| 수정 | `apex_core/benchmarks/integration/bench_cross_core_latency.cpp` | CoreEngine core_assignments 연결 |
| 수정 | `apex_core/benchmarks/integration/bench_cross_core_message_passing.cpp` | CoreEngine core_assignments 연결 |
| 생성 | `docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/data/metadata.json` | 환경 정보 |
| 생성 | `docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/data/*.json` | 벤치마크 결과 |
| 생성 | `docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/benchmark_report.html` | 3-way 비교 보고서 |

---

### Task 1: bench_affinity_helper.hpp 생성 + bench_main.cpp 수정

**Files:**
- Create: `apex_core/benchmarks/bench_affinity_helper.hpp`
- Modify: `apex_core/benchmarks/bench_main.cpp`

- [ ] **Step 1: bench_affinity_helper.hpp 작성**

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/core_engine.hpp>
#include <apex/core/cpu_topology.hpp>
#include <apex/core/thread_affinity.hpp>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace apex::bench
{

/// Global affinity state — set by init_affinity(), read by benchmark functions.
inline bool g_affinity_enabled = false;
inline std::vector<apex::core::CoreAssignment> g_core_assignments;

/// Parse --affinity=on|off from argv, strip it, populate globals.
/// Call before benchmark::Initialize().
inline void init_affinity(int& argc, char** argv)
{
    bool affinity_on = false;
    int write_idx = 1;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], "--affinity=", 11) == 0)
        {
            std::string val(argv[i] + 11);
            affinity_on = (val == "on" || val == "true" || val == "1");
            continue;
        }
        argv[write_idx++] = argv[i];
    }
    argc = write_idx;

    if (!affinity_on)
    {
        std::cout << "Affinity: OFF (default)\n";
        return;
    }

    auto topology = apex::core::discover_topology();
    g_core_assignments.reserve(topology.physical_core_count());
    for (const auto& pc : topology.physical_cores)
    {
        g_core_assignments.push_back(
            {.logical_core_id = pc.primary_logical_id(), .numa_node = pc.numa_node});
    }
    g_affinity_enabled = true;
    std::cout << "Affinity: ON (" << g_core_assignments.size() << " physical cores)\n";
}

/// Build CoreAssignment vector for N cores (used by CoreEngine benchmarks).
/// When affinity is enabled, maps to discovered physical cores (round-robin if N > physical).
/// When disabled, returns empty vector (CoreEngine legacy behavior).
inline std::vector<apex::core::CoreAssignment> build_assignments(uint32_t num_cores)
{
    if (!g_affinity_enabled)
        return {};

    std::vector<apex::core::CoreAssignment> result;
    result.reserve(num_cores);
    for (uint32_t i = 0; i < num_cores; ++i)
    {
        result.push_back(g_core_assignments[i % g_core_assignments.size()]);
    }
    return result;
}

/// Apply thread affinity for raw-thread benchmarks (not CoreEngine).
/// worker_index is mapped to physical core via round-robin.
inline void apply_worker_affinity(int worker_index)
{
    if (!g_affinity_enabled || g_core_assignments.empty())
        return;
    auto idx = static_cast<size_t>(worker_index) % g_core_assignments.size();
    (void)apex::core::apply_thread_affinity(g_core_assignments[idx].logical_core_id);
}

} // namespace apex::bench
```

- [ ] **Step 2: bench_main.cpp에 init_affinity 호출 추가**

`bench_main.cpp` 수정 — `benchmark::Initialize` 전에 호출:

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "bench_affinity_helper.hpp"
#include "system_profile.hpp"
#include <benchmark/benchmark.h>

int main(int argc, char** argv)
{
    apex::bench::init_affinity(argc, argv);

    auto profile = apex::bench::detect_system_profile();
    profile.print();

    benchmark::Initialize(&argc, argv);
    benchmark::AddCustomContext("physical_cores", std::to_string(profile.physical_cores));
    benchmark::AddCustomContext("logical_cores", std::to_string(profile.logical_cores));
    benchmark::AddCustomContext("total_ram_mb", std::to_string(profile.total_ram_bytes / (1024 * 1024)));

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
```

- [ ] **Step 3: 커밋**

```bash
git add apex_core/benchmarks/bench_affinity_helper.hpp apex_core/benchmarks/bench_main.cpp
git commit -m "feat(bench): --affinity 플래그 파싱 헬퍼 + bench_main 연동 (BACKLOG-264)"
git push
```

---

### Task 2: bench_cross_core_latency.cpp affinity 연결

**Files:**
- Modify: `apex_core/benchmarks/integration/bench_cross_core_latency.cpp`

- [ ] **Step 1: include 추가 + core_assignments 연결**

파일 상단에 include 추가:
```cpp
#include "../bench_affinity_helper.hpp"
```

CoreEngineConfig 구성 변경 (기존 L14-19):
```cpp
// 기존:
CoreEngineConfig config{.num_cores = 2,
                        .spsc_queue_capacity = 65536,
                        .tick_interval = std::chrono::milliseconds{1},
                        .drain_batch_limit = 1024,
                        .core_assignments = {},
                        .numa_aware = true};

// 변경:
CoreEngineConfig config{.num_cores = 2,
                        .spsc_queue_capacity = 65536,
                        .tick_interval = std::chrono::milliseconds{1},
                        .drain_batch_limit = 1024,
                        .core_assignments = apex::bench::build_assignments(2),
                        .numa_aware = true};
```

- [ ] **Step 2: 커밋**

```bash
git add apex_core/benchmarks/integration/bench_cross_core_latency.cpp
git commit -m "feat(bench): cross_core_latency affinity 연결 (BACKLOG-264)"
git push
```

---

### Task 3: bench_cross_core_message_passing.cpp affinity 연결

**Files:**
- Modify: `apex_core/benchmarks/integration/bench_cross_core_message_passing.cpp`

- [ ] **Step 1: include 추가 + core_assignments 연결**

파일 상단에 include 추가:
```cpp
#include "../bench_affinity_helper.hpp"
```

CoreEngineConfig 구성 변경 (기존 L12-17):
```cpp
// 기존:
CoreEngineConfig config{.num_cores = 2,
                        .spsc_queue_capacity = 65536,
                        .tick_interval = std::chrono::milliseconds{1},
                        .drain_batch_limit = 1024,
                        .core_assignments = {},
                        .numa_aware = true};

// 변경:
CoreEngineConfig config{.num_cores = 2,
                        .spsc_queue_capacity = 65536,
                        .tick_interval = std::chrono::milliseconds{1},
                        .drain_batch_limit = 1024,
                        .core_assignments = apex::bench::build_assignments(2),
                        .numa_aware = true};
```

- [ ] **Step 2: 커밋**

```bash
git add apex_core/benchmarks/integration/bench_cross_core_message_passing.cpp
git commit -m "feat(bench): cross_core_message_passing affinity 연결 (BACKLOG-264)"
git push
```

---

### Task 4: bench_architecture_comparison.cpp affinity 연결

**Files:**
- Modify: `apex_core/benchmarks/integration/bench_architecture_comparison.cpp`

이 벤치마크는 CoreEngine을 사용하지 않고 raw thread를 생성하므로, 워커 람다에서 직접 `apply_worker_affinity()`를 호출해야 한다.

- [ ] **Step 1: include 추가**

파일 상단에 추가:
```cpp
#include "../bench_affinity_helper.hpp"
```

- [ ] **Step 2: BM_PerCore_Stateful 워커에 affinity 적용**

worker 람다 시작 부분에 추가 (L73 부근):

```cpp
// 기존:
auto worker = [&per_core_sessions](int c) {
    boost::asio::io_context ctx;
    // ...
};

// 변경:
auto worker = [&per_core_sessions](int c) {
    apex::bench::apply_worker_affinity(c);
    boost::asio::io_context ctx;
    // ...
};
```

- [ ] **Step 3: BM_Shared_Stateful 워커에 affinity 적용**

스레드 생성 부분 변경 (L134-137):

```cpp
// 기존:
for (int t = 0; t < num_threads - 1; ++t)
    threads.emplace_back([&ctx]() { ctx.run(); });
ctx.run();

// 변경:
for (int t = 0; t < num_threads - 1; ++t)
    threads.emplace_back([&ctx, t]() {
        apex::bench::apply_worker_affinity(t);
        ctx.run();
    });
apex::bench::apply_worker_affinity(num_threads - 1);
ctx.run();
```

- [ ] **Step 4: BM_Shared_LockFree_Stateful 워커에 동일 적용**

스레드 생성 부분 변경 (L182-185):

```cpp
// 기존:
for (int t = 0; t < num_threads - 1; ++t)
    threads.emplace_back([&ctx]() { ctx.run(); });
ctx.run();

// 변경:
for (int t = 0; t < num_threads - 1; ++t)
    threads.emplace_back([&ctx, t]() {
        apex::bench::apply_worker_affinity(t);
        ctx.run();
    });
apex::bench::apply_worker_affinity(num_threads - 1);
ctx.run();
```

- [ ] **Step 5: 커밋**

```bash
git add apex_core/benchmarks/integration/bench_architecture_comparison.cpp
git commit -m "feat(bench): architecture_comparison 워커 스레드 affinity 적용 (BACKLOG-264)"
git push
```

---

### Task 5: clang-format + Release 빌드

- [ ] **Step 1: clang-format 실행**

```bash
find apex_core/benchmarks \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) | xargs clang-format -i
```

변경 있으면 커밋.

- [ ] **Step 2: Release 빌드**

```bash
"<프로젝트루트>/apex_tools/apex-agent/run-hook" queue build release
```

`run_in_background: true`로 실행, 완료 대기.

- [ ] **Step 3: 빌드 결과 확인**

빌드 로그에서 에러 0건 확인. 실패 시 수정 후 재빌드.

---

### Task 6: Micro 벤치마크 실행 (9개 순차)

각 벤치마크를 `apex-agent queue benchmark` 경유로 **1개씩** 실행.
결과를 `apex_core/benchmark_results/`에 JSON으로 저장.
진행 상황을 "N/9 완료" 형태로 사용자에게 보고.

실행 순서 및 커맨드 (프로젝트 루트 절대경로 = `ROOT`):

```bash
# 1/9
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_mpsc_queue.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/mpsc_queue.json

# 2/9
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_spsc_queue.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/spsc_queue.json

# 3/9
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_allocators.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/allocators.json

# 4/9
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_ring_buffer.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/ring_buffer.json

# 5/9
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_frame_codec.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/frame_codec.json

# 6/9
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_dispatcher.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/dispatcher.json

# 7/9
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_timing_wheel.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/timing_wheel.json

# 8/9
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_serialization.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/serialization.json

# 9/9
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_session_lifecycle.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/session_lifecycle.json
```

---

### Task 7: Integration 벤치마크 — affinity OFF (3개 순차)

```bash
# 1/3
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_architecture_comparison.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/architecture_comparison.json

# 2/3
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_cross_core_latency.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/cross_core_latency.json

# 3/3
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_cross_core_message_passing.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/cross_core_message_passing.json
```

추가로 단일 스레드 integration도 실행 (버전 비교용):

```bash
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_frame_pipeline.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/frame_pipeline.json

"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_session_throughput.exe \
  --benchmark_format=json --benchmark_out=apex_core/benchmark_results/session_throughput.json
```

---

### Task 8: Integration 벤치마크 — affinity ON (3개 순차)

`--affinity=on` 플래그 추가, 출력 파일명에 `_affinity` 접미사.

```bash
# 1/3
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_architecture_comparison.exe \
  --affinity=on --benchmark_format=json \
  --benchmark_out=apex_core/benchmark_results/architecture_comparison_affinity.json

# 2/3
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_cross_core_latency.exe \
  --affinity=on --benchmark_format=json \
  --benchmark_out=apex_core/benchmark_results/cross_core_latency_affinity.json

# 3/3
"${ROOT}/apex_tools/apex-agent/run-hook" queue benchmark \
  apex_core/bin/release/bench_cross_core_message_passing.exe \
  --affinity=on --benchmark_format=json \
  --benchmark_out=apex_core/benchmark_results/cross_core_message_passing_affinity.json
```

---

### Task 9: 데이터 정리 + metadata.json

- [ ] **Step 1: 버전 디렉토리 생성 및 데이터 복사**

```bash
mkdir -p docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/data
cp apex_core/benchmark_results/*.json docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/data/
```

- [ ] **Step 2: metadata.json 생성**

`docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/data/metadata.json`:

```json
{
  "version": "v0.6.5",
  "date": "<실행일 ISO 형식>",
  "host_name": "<hostname>",
  "cpu_name": "Intel Core i5-9300H (Coffee Lake)",
  "physical_cores": 4,
  "logical_cores": 8,
  "p_cores": 0,
  "e_cores": 0,
  "mhz_per_cpu": 2400,
  "max_boost_mhz": 4100,
  "caches": {
    "l1d": "32 KB (per-core)",
    "l2": "256 KB (per-core)",
    "l3": "8 MB (shared)"
  },
  "numa_nodes": 1,
  "affinity_support": true,
  "total_ram_mb": "<실측값>",
  "compiler": "MSVC, C++23",
  "build_type": "Release",
  "benchmark_library": "Google Benchmark v1.9.4"
}
```

값은 벤치마크 JSON context에서 추출하여 채운다.

- [ ] **Step 3: 커밋**

```bash
git add docs/apex_core/benchmark/v0.6.5.0/
git commit -m "data(bench): v0.6.5.0 i5-9300H 벤치마크 데이터 수집 (BACKLOG-264)"
git push
```

---

### Task 10: HTML 3-way 비교 보고서 생성

**Files:**
- Create: `docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/benchmark_report.html`

기존 `v0.6.1.0/i7-14700-20C28T/benchmark_report.html`의 디자인 스타일(다크 테마, Plotly.js 인터랙티브 차트)을 따르되, 3-way 비교 구조를 적용한다.

보고서 데이터 소스:
- `docs/apex_core/benchmark/v0.5.10.0/i5-9300H-4C8T/data/*.json` (baseline)
- `docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/data/*.json` (current OFF)
- `docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/data/*_affinity.json` (current ON)

JSON 데이터를 읽어서 HTML 내 `<script>` 블록에 인라인 임베딩 후, Plotly.js로 시각화.

- [ ] **Step 1: 데이터 수집 + 분석**

3개 소스의 JSON 데이터를 파싱하여 비교 테이블/차트 데이터 구성:
- Micro: v0.5.10.0 vs v0.6.5.0 (items_per_second 또는 bytes_per_second 기준)
- Architecture comparison: 3-way 코어 스케일링 곡선
- Cross-core: 3-way latency/throughput 비교

- [ ] **Step 2: HTML 보고서 작성**

섹션 구성:
1. **Hero + Executive Summary** — 환경 카드, 핵심 수치 4개
2. **Version Comparison** — Micro + Integration(OFF) vs v0.5.10.0 bar charts
3. **Affinity Impact** — architecture_comparison OFF/ON 오버레이, cross-core 비교
4. **3-Way Comparison** — architecture_comparison 3선 스케일링 + 효율 테이블
5. **Conclusions** — 관찰 요약, i7-14700 예상, 한계점

Plotly.js CDN 로드, 데이터는 `const benchData = { ... }` 인라인.

- [ ] **Step 3: 커밋**

```bash
git add docs/apex_core/benchmark/v0.6.5.0/i5-9300H-4C8T/benchmark_report.html
git commit -m "docs(bench): v0.6.5.0 i5-9300H 3-way 비교 보고서 (BACKLOG-264)"
git push
```

---

### Task 11: 설계서 업데이트 + 문서 갱신

- [ ] **Step 1: 설계서 scope 수정 반영**

`docs/apex_core/plans/20260327_143325_numa_benchmark_comparison_design.md`에서 affinity 대상을 5개 → 3개로 수정 (bench_frame_pipeline, bench_session_throughput 제외).

- [ ] **Step 2: BENCHMARK_GUIDE.md에 affinity 플래그 문서화**

`docs/apex_core/benchmark/BENCHMARK_GUIDE.md`에 `--affinity=on|off` 사용법 추가.

- [ ] **Step 3: progress 문서 작성**

`docs/apex_core/progress/` 에 타임스탬프 파일명으로 작업 결과 요약.

- [ ] **Step 4: 커밋**

```bash
git add docs/
git commit -m "docs(bench): 벤치마크 가이드 affinity 플래그 문서화 + progress (BACKLOG-264)"
git push
```
