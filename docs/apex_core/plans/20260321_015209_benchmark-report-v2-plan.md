# Benchmark Report v2 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Release vs Debug 비교를 버전 간 비교 + 7개 방법론 비교 체계로 전환

**Architecture:** 13개 벤치마크(5 확장 + 1 신규) → 버전 태그 디렉토리에 JSON 저장 → Python 보고서 스크립트가 baseline/current 비교 PDF 생성

**Tech Stack:** C++23 (Google Benchmark), Python 3 (matplotlib + reportlab), CMake/Ninja

**Spec:** `docs/apex_core/plans/20260321_014316_benchmark-report-v2-design.md`

---

## Phase 1: C++ 벤치마크 확장 (병렬 가능)

### Task 1: bench_allocators.cpp — 할당기 3종 + malloc + make_shared

**Files:**
- Create: `apex_core/benchmarks/micro/bench_allocators.cpp`
- Delete: `apex_core/benchmarks/micro/bench_slab_allocator.cpp`

- [ ] **Step 1: bench_allocators.cpp 작성**

기존 bench_slab_allocator.cpp를 기반으로 BumpAllocator, ArenaAllocator 벤치마크 추가.
Bump/Arena는 alloc-only + batch reset 패턴:
```cpp
// Bump: N회 alloc 후 reset, per-alloc 시간 측정
static void BM_BumpAllocator_Alloc(benchmark::State& state) {
    const auto sz = static_cast<size_t>(state.range(0));
    BumpAllocator alloc(sz * 1024);  // 충분한 용량
    for (auto _ : state) {
        void* p = alloc.allocate(sz);
        benchmark::DoNotOptimize(p);
        if (alloc.used_bytes() + sz > alloc.capacity()) alloc.reset();
    }
    state.SetItemsProcessed(state.iterations());
}
```

기존 벤치마크 유지: SlabAllocator, Malloc, MakeShared.

- [ ] **Step 2: bench_slab_allocator.cpp 삭제**

### Task 2: bench_serialization.cpp — FlatBuffers vs new+memcpy

**Files:**
- Create: `apex_core/benchmarks/micro/bench_serialization.cpp`

- [ ] **Step 1: bench_serialization.cpp 작성**

EchoRequest 스키마 사용. FlatBufferBuilder로 페이로드 빌드 vs raw new+memcpy:
```cpp
// FlatBuffers: builder.CreateVector + Finish
// HeapAlloc: new char[WireHeader::SIZE + sz] + memcpy
```

사이즈: {64, 512, 4096}

### Task 3: bench_dispatcher.cpp — std::unordered_map 변형

**Files:**
- Modify: `apex_core/benchmarks/micro/bench_dispatcher.cpp`

- [ ] **Step 1: BM_Dispatcher_StdMap_Lookup 추가**

기존 BM_Dispatcher_Lookup과 동일 패턴, std::unordered_map<uint32_t, std::function<void()>> 사용.

### Task 4: bench_session_lifecycle.cpp — shared_ptr 변형

**Files:**
- Modify: `apex_core/benchmarks/micro/bench_session_lifecycle.cpp`

- [ ] **Step 1: BM_SharedPtr_Copy 추가**

기존 BM_SessionPtr_Copy와 동일 패턴, std::shared_ptr<Session> 복사 벤치마크.

### Task 5: bench_ring_buffer.cpp — naive memcpy 변형

**Files:**
- Modify: `apex_core/benchmarks/micro/bench_ring_buffer.cpp`

- [ ] **Step 1: BM_NaiveBuffer_CopyWrite 추가**

std::vector<uint8_t> + memcpy 패턴으로 동일 크기 write/read 측정.

### Task 6: CMakeLists.txt 업데이트

**Files:**
- Modify: `apex_core/benchmarks/CMakeLists.txt`

- [ ] **Step 1: bench_slab_allocator → bench_allocators, bench_serialization 추가**

## Phase 2: 보고서 스크립트 리팩토링

### Task 7: generate_benchmark_report.py 전면 재작성

**Files:**
- Rewrite: `apex_tools/benchmark/report/generate_benchmark_report.py`

- [ ] **Step 1: CLI/데이터 로딩 변경** — `--baseline/--current/--data-dir`
- [ ] **Step 2: 차트 함수 재작성** — 10개 차트 (버전 비교 + 방법론 비교)
- [ ] **Step 3: PDF 빌더 재작성** — 10페이지 구성

## Phase 3: 빌드 + 실행 + 보고서

### Task 8: 빌드 및 벤치마크 실행

- [ ] **Step 1: clang-format**
- [ ] **Step 2: Release 빌드**
- [ ] **Step 3: 13개 벤치마크 순차 실행 (개별 커맨드)**
- [ ] **Step 4: metadata.json 생성**
- [ ] **Step 5: docs/apex_core/benchmark/v0.5.10.0/ 에 복사**

### Task 9: 보고서 생성

- [ ] **Step 1: analysis.json 작성**
- [ ] **Step 2: generate_benchmark_report.py 실행**
- [ ] **Step 3: PDF 검증**

## Phase 4: 정리

### Task 10: 문서 갱신 + 정리

- [ ] **Step 1: 구 파일 삭제** (v1 flat 파일, slab_pool.json 등)
- [ ] **Step 2: apex_core/CLAUDE.md 갱신**
- [ ] **Step 3: apex_core/benchmarks/README.md 갱신**
- [ ] **Step 4: apex_tools/benchmark/report/README.md 갱신**
