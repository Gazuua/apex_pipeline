# BACKLOG-40: NUMA 바인딩 + Core Affinity — 완료

**브랜치**: feature/numa-core-affinity
**PR**: #218
**상태**: 완료

## 작업 요약

v0.6.1 벤치마크에서 확인된 Straggler Effect(P/E 비대칭 코어에서 E-Core 진입 시 21.8% 하락)를 해결하고, 대칭 서버 환경(Xeon/EPYC)에서의 캐시 안정성 + NUMA 로컬리티를 확보하기 위한 CPU Topology Discovery + Thread Affinity + NUMA 메모리 바인딩 구현.

## 구현 내용

### 신규 모듈
- **CpuTopology** (`cpu_topology.hpp/cpp`): Windows `GetLogicalProcessorInformationEx` / Linux sysfs 기반 물리 코어 탐지, P/E 분류(Intel 하이브리드), NUMA 노드 매핑. 탐지 실패 시 graceful fallback.
- **ThreadAffinity** (`thread_affinity.hpp/cpp`): `SetThreadGroupAffinity`(Win) / `pthread_setaffinity_np`(Linux) + `set_mempolicy(MPOL_BIND)` NUMA 메모리 바인딩. `CPU_SETSIZE` 상한 체크.

### 기존 모듈 수정
- **ServerConfig** (`server_config.hpp`): `AffinityConfig` 구조체 추가 — `enabled`(기본 true), `worker_cores`(빈=자동), `numa_aware`(기본 true)
- **config.cpp**: TOML `[affinity]` 섹션 파싱 — 중복/비정수 입력 검증
- **CoreEngine** (`core_engine.hpp/cpp`): `CoreAssignment` 구조체, `run_core()`에서 affinity + NUMA 적용
- **Server** (`server.cpp`): 생성자에서 topology 탐지 → P-Core 우선 정렬 → 물리 코어 1:1 배정

### 설계 결정
| 항목 | 결정 |
|------|------|
| 워커 핀닝 | 물리 코어 1:1, P+E 전부 사용 |
| `num_cores=0` | 전체 물리 코어 수 (기존 `hardware_concurrency` 논리 코어에서 변경) |
| BlockingTaskExecutor | affinity 미적용 (OS 위임) |
| 설정 모델 | 자동 우선, `worker_cores` config 오버라이드 가능 |
| NUMA | Linux `set_mempolicy(MPOL_BIND)`, Windows first-touch |
| 탐지 실패 | graceful degradation (경고 로그 + 기존 동작 fallback) |

## 테스트
- `test_cpu_topology`: 10건 (불변식 8 + 추가 2)
- `test_thread_affinity`: 5건 (fixture + 핀닝 검증 + NUMA)
- `test_config`: AffinityConfig 파싱 8건 (기본값, disabled, manual, 경계값)
- 기존 테스트 42곳: `.affinity={.enabled=false}` 추가로 기존 동작 보존

## Auto-Review
- 리뷰어 4명 (logic, systems, design, test) 병렬 디스패치
- 총 25건 수정 (CRITICAL 1, MEDIUM 5, LOW 19)
- CRITICAL: Linux NUMA 비연속 노드 탐지 누락 (`break` → `continue`)
- 상세: `docs/apex_core/review/20260327_102253_auto-review.md`

## 관련 백로그
- BACKLOG-40: RESOLVED (FIXED)
- BACKLOG-264: 등록 (적용 전후 벤치마크 비교 보고서 — 별도 PR)
