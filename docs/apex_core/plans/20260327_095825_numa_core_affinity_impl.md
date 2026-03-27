# BACKLOG-40: NUMA 바인딩 + Core Affinity — 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** CPU Topology Discovery + Thread Affinity 핀닝 + NUMA 메모리 바인딩으로 Per-Core 아키텍처의 캐시 안정성과 NUMA 로컬리티를 확보한다.

**Architecture:** `CpuTopology`가 시스템 정보를 탐지하고, `Server`가 코어 배정을 계산하여 `CoreEngine`에 전달. `CoreEngine::run_core()`에서 스레드 진입 직후 플랫폼별 affinity/NUMA API를 호출.

**Tech Stack:** C++23, Windows API (`GetLogicalProcessorInformationEx`), Linux sysfs + `pthread_setaffinity_np` + `set_mempolicy`, TOML config, GTest

---

### Task 1: CpuTopology — 토폴로지 탐지 모듈

**Files:**
- Create: `apex_core/include/apex/core/cpu_topology.hpp`
- Create: `apex_core/src/cpu_topology.cpp`

- [ ] **Step 1: 헤더 작성** — `CoreType` enum, `PhysicalCore`/`CpuTopology` 구조체, `discover_topology()` 선언
- [ ] **Step 2: Windows 구현** — `GetLogicalProcessorInformationEx(RelationProcessorCore)` + `RelationNumaNode`
- [ ] **Step 3: Linux 구현** — sysfs `/sys/devices/system/cpu/` 파싱
- [ ] **Step 4: Fallback** — 탐지 실패 시 `hardware_concurrency()` 기반 기본 토폴로지

### Task 2: ThreadAffinity — 플랫폼별 affinity 적용

**Files:**
- Create: `apex_core/include/apex/core/thread_affinity.hpp`
- Create: `apex_core/src/thread_affinity.cpp`

- [ ] **Step 1: 헤더 작성** — `apply_thread_affinity()`, `apply_numa_memory_policy()` 선언
- [ ] **Step 2: Windows 구현** — `SetThreadAffinityMask`
- [ ] **Step 3: Linux 구현** — `pthread_setaffinity_np` + `set_mempolicy`
- [ ] **Step 4: 실패 시 경고 로그만, 예외 없음**

### Task 3: AffinityConfig — 설정 통합

**Files:**
- Modify: `apex_core/include/apex/core/server_config.hpp`
- Modify: `apex_core/src/config.cpp`

- [ ] **Step 1: `AffinityConfig` 구조체 추가** — `enabled`, `worker_cores`, `numa_aware`
- [ ] **Step 2: `ServerConfig`에 `AffinityConfig affinity` 필드 추가**
- [ ] **Step 3: `config.cpp`에 `[affinity]` TOML 섹션 파싱 추가**

### Task 4: CoreEngine 통합

**Files:**
- Modify: `apex_core/include/apex/core/core_engine.hpp`
- Modify: `apex_core/src/core_engine.cpp`

- [ ] **Step 1: `CoreAssignment` 구조체 + `CoreEngineConfig`에 assignments 벡터 추가**
- [ ] **Step 2: `CoreEngine` 생성자에서 assignments 저장**
- [ ] **Step 3: `run_core()`에서 affinity + NUMA 적용, 시작 로그 출력**
- [ ] **Step 4: `num_cores=0` 자동 탐지를 물리 코어 수로 변경 (assignments 비어있으면 기존 동작)**

### Task 5: Server 통합 — 토폴로지 탐지 + 코어 배정

**Files:**
- Modify: `apex_core/src/server.cpp`

- [ ] **Step 1: `Server::Server()`에서 토폴로지 탐지** — affinity.enabled 시 `discover_topology()` 호출
- [ ] **Step 2: 코어 배정 계산** — auto/manual 모드, P-Core 우선 정렬
- [ ] **Step 3: `CoreEngineConfig`에 assignments 전달**
- [ ] **Step 4: 시작 로그** — 토폴로지 요약 + 코어 배정 정보

### Task 6: 테스트

**Files:**
- Create: `apex_core/tests/unit/test_cpu_topology.cpp`
- Create: `apex_core/tests/unit/test_thread_affinity.cpp`

- [ ] **Step 1: topology 불변식 테스트** — 물리 ≥ 1, 논리 ≥ 물리, NUMA ≥ 1
- [ ] **Step 2: affinity 적용 테스트** — 핀닝 후 실행 코어 검증
- [ ] **Step 3: AffinityConfig TOML 파싱 테스트** — 기존 test_config.cpp에 추가

### Task 7: CMake + clang-format

**Files:**
- Modify: `apex_core/CMakeLists.txt`
- Modify: `apex_core/tests/unit/CMakeLists.txt`

- [ ] **Step 1: 라이브러리에 `cpu_topology.cpp`, `thread_affinity.cpp` 추가**
- [ ] **Step 2: 테스트 타겟 추가**
- [ ] **Step 3: clang-format 실행**
- [ ] **Step 4: 빌드 + 테스트 실행**
