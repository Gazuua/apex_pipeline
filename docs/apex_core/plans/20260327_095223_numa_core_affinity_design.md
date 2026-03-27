# BACKLOG-40: NUMA 바인딩 + Core Affinity — 설계 문서

## 배경

v0.6.1 벤치마크(i7-14700, 20C/28T)에서 Per-Core 아키텍처가 8 P-Core까지 선형 확장(7.65M msg/s) 후 E-Core 구간에서 21.8% 하락(5.98M msg/s)하는 'Straggler Effect' 확인. 원인: OS 스케줄러가 워커 스레드를 임의 코어에 배정하면서 발생하는 캐시 미스, P/E 성능 비대칭, NUMA 리모트 메모리 접근.

대칭 서버 환경(Xeon/EPYC)에서도 CPU affinity + NUMA 바인딩은 캐시 마이그레이션 방지, NUMA 로컬리티, tail latency 안정화에 필수적.

## 핵심 결정 사항

| 항목 | 결정 | 근거 |
|------|------|------|
| 메인 워커 핀닝 대상 | **물리 코어 1:1, P+E 전부 사용** | E-Core를 놀리면 반쪽짜리. shared-nothing이라 느린 코어가 다른 코어에 영향 없음 |
| BlockingTaskExecutor | **affinity 없음, OS 위임** | 간헐적 CPU-bound 작업, 핀닝 실익 없음. HT 시블링 고정 시 메인 워커 실행 유닛 침범 위험 |
| 설정 모델 | **자동 우선, config 오버라이드** | 대부분 자동으로 충분, 컨테이너/특수 환경에서 수동 필요 |
| `num_cores=0` 기본값 | **전체 물리 코어 수** | 기존 `hardware_concurrency()`(논리 코어)에서 변경. HT는 이벤트루프에 비효율 |
| NUMA 메모리 바인딩 | **Linux: `set_mempolicy(MPOL_BIND)`, Windows: first-touch** | 워커 초기화 시 설정하면 per-core 할당기가 자동으로 로컬 노드에 할당 |
| Topology 탐지 실패 | **graceful degradation** | 경고 로그 후 기존 동작(affinity 없음)으로 fallback. 서버가 뜨는 게 최우선 |

## 아키텍처

### 1. CPU Topology Discovery

시스템 CPU 토폴로지를 탐지하여 구조화된 정보를 제공하는 읽기 전용 모듈.

```cpp
enum class CoreType : uint8_t { Performance, Efficiency, Unknown };

struct PhysicalCore {
    uint32_t physical_id;                   // 물리 코어 ID
    uint32_t numa_node;                     // NUMA 노드 (단일소켓이면 0)
    CoreType type;                          // P/E/Unknown
    std::vector<uint32_t> logical_ids;      // HT 시블링들의 논리 코어 ID
};

struct CpuTopology {
    std::vector<PhysicalCore> physical_cores;
    uint32_t numa_node_count;

    // 편의 메서드
    std::vector<PhysicalCore> cores_by_numa(uint32_t node) const;
    std::vector<PhysicalCore> performance_cores() const;
    std::vector<PhysicalCore> efficiency_cores() const;
};

// 팩토리 — 플랫폼별 탐지, 실패 시 fallback 반환
CpuTopology discover_topology();
```

**플랫폼 구현**:
- **Windows**: `GetLogicalProcessorInformationEx(RelationProcessorCore)` — `EfficiencyClass` 필드로 P/E 구분, `RelationNumaNode`로 NUMA 탐지
- **Linux**: `/sys/devices/system/cpu/cpu*/topology/core_id` + `physical_package_id`로 물리 코어 매핑, `/sys/devices/system/cpu/cpu*/cpu_capacity` (커널 5.7+)로 P/E 구분, `/sys/devices/system/node/`로 NUMA

**Fallback** (탐지 실패 시): `hardware_concurrency()`개의 `CoreType::Unknown`, 단일 NUMA 노드, 논리 ID 1:1 매핑으로 구성. 경고 로그 출력.

### 2. Affinity Configuration

`ServerConfig`에 추가되는 설정 구조체.

```cpp
struct AffinityConfig {
    bool enabled = true;                     // false면 affinity 미적용 (기존 동작)
    std::vector<uint32_t> worker_cores = {}; // 빈 배열 = 자동 (전체 물리 코어)
    bool numa_aware = true;                  // NUMA 메모리 바인딩 활성화
};
```

**TOML 매핑**:
```toml
[affinity]
enabled = true
worker_cores = []
numa_aware = true
```

**자동 모드** (`worker_cores` 비어있을 때):
1. `CpuTopology` 탐지
2. 전체 물리 코어를 워커 후보로 선정
3. `num_cores`와 조합:
   - `num_cores = 0` → 물리 코어 전체 수로 결정
   - `num_cores = N` → N개 선택 (P-Core 우선, 같은 NUMA 노드 우선)

**수동 모드** (`worker_cores` 지정):
- 지정된 논리 코어 ID에 1:1 핀닝
- topology 검증 후 불일치 시 경고만 (차단 안 함)
- `num_cores`는 `worker_cores.size()`로 자동 덮어씌움

### 3. Thread Affinity 적용

`CoreEngine::run_core()` 진입 직후, io_context 루프 시작 전에 적용.

```cpp
void CoreEngine::run_core(uint32_t core_id)
{
    tls_core_id_ = core_id;

    // affinity 설정
    if (affinity_enabled_)
        apply_affinity(core_id, assigned_cores_[core_id]);

    auto& ctx = *cores_[core_id];
    // ... 기존 로직 (tick timer, io_ctx.run())
}
```

**플랫폼 추상화 함수**:

```cpp
// thread_affinity.hpp
bool apply_thread_affinity(uint32_t logical_core_id);   // 현재 스레드를 지정 논리 코어에 핀닝
bool apply_numa_memory_policy(uint32_t numa_node);       // 현재 스레드의 메모리 할당을 지정 NUMA 노드로 제한
```

- **Windows**: `SetThreadAffinityMask(GetCurrentThread(), 1ULL << logical_id)`
- **Linux**: `pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset)`
- **NUMA (Linux)**: `set_mempolicy(MPOL_BIND, node_mask, max_node)` — `run_core()` 진입 직후 호출하면 이후 per-core 할당기(BumpAllocator, ArenaAllocator) 메모리가 로컬 노드에 할당
- **NUMA (Windows)**: 별도 처리 없음 — first-touch 정책으로 충분
- 모든 실패는 경고 로그만, 예외 안 던짐

**BlockingTaskExecutor**: 변경 없음. 기존 `boost::asio::thread_pool` 그대로, affinity 미적용.

### 4. 시작 로그

```
[INFO] CPU Topology: 20 physical cores (8P+12E), 28 logical, 1 NUMA node
[INFO] Core affinity: worker[0]→P-Core 0 (logical 0), worker[1]→P-Core 1 (logical 2), ...
[INFO] BlockingTaskExecutor: 2 threads, no affinity (OS scheduled)
```

Fallback 시:
```
[WARN] CPU topology detection failed: <reason>. Falling back to 28 cores, no affinity.
```

## 파일 배치

| 파일 | 역할 | 신규/수정 |
|------|------|-----------|
| `apex_core/include/apex/core/cpu_topology.hpp` | CpuTopology, PhysicalCore 구조체 + `discover_topology()` | 신규 |
| `apex_core/src/cpu_topology.cpp` | 플랫폼별 탐지 구현 | 신규 |
| `apex_core/include/apex/core/thread_affinity.hpp` | `apply_thread_affinity()`, `apply_numa_memory_policy()` | 신규 |
| `apex_core/src/thread_affinity.cpp` | 플랫폼별 affinity 적용 구현 | 신규 |
| `apex_core/include/apex/core/server_config.hpp` | `AffinityConfig` 추가 | 수정 |
| `apex_core/src/core_engine.cpp` | `run_core()`에 affinity 호출 추가 | 수정 |
| `apex_core/src/server.cpp` | topology 탐지 + 코어 배정 로직 | 수정 |

## CMake

- 기존 `apex_core` 타겟에 소스 추가. 별도 라이브러리 불필요
- Linux에서 libnuma 링크: `find_library(NUMA numa)` + optional 링크 (없으면 `set_mempolicy` no-op)

## 테스트

- `cpu_topology_test.cpp` — 현재 머신에서 탐지 결과 불변식 검증 (물리 코어 ≥ 1, 논리 ≥ 물리, NUMA ≥ 1)
- `thread_affinity_test.cpp` — affinity 설정 후 `GetCurrentProcessorNumber()` / `sched_getcpu()`로 실제 핀닝 검증

## 스코프 외

- 비대칭 워크로드 분배 (코어 성능 가중치 기반 연결 분배) — 향후 별도 백로그
- BlockingTaskExecutor affinity — 현재 불필요
- 런타임 코어 마이그레이션 — shared-nothing이라 불필요
