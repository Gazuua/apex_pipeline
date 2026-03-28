# NUMA+Affinity 벤치마크 비교 보고서 — 완료

- **백로그**: BACKLOG-264
- **브랜치**: feature/numa-benchmark
- **기간**: 2026-03-27 ~ 2026-03-28

## 작업 결과

### 코드 변경

- `bench_affinity_helper.hpp` 신규: `--affinity=on` 플래그 파싱 + CoreAssignment 구성
- `bench_main.cpp` 수정: `init_affinity()` 호출 추가
- Integration 벤치마크 3개에 affinity 연결 (architecture_comparison, cross_core_latency, cross_core_message_passing)
- `CMakeLists.txt`: `apex_bench_main`에 `apex::core` 링크 추가
- `system_profile.hpp`: NOMINMAX 재정의 경고 수정
- `bench_mpsc_queue.cpp`: 2P1C 비결정성 수정 (drain 루프 → 단일 dequeue)
- `bench_spsc_queue.cpp`: ConcurrentThroughput 비결정성 수정 (consumed → iterations)
- `generate_benchmark_report.py`: `--hardware` 파라미터 추가
- `run_all_benchmarks.ps1` 신규: 원클릭 PowerShell 벤치마크 스크립트

### 벤치마크 실측

**i5-9300H (4C/8T)**: Micro 9 + Integration OFF 5 + Integration ON 3 = 17회
**i7-14700 (20C/28T)**: Micro 10 + Integration OFF 5 + Integration ON 5 = 20회

### 보고서 산출물

| 환경 | 3-way 비교 | 기존 형식 |
|------|:-:|:-:|
| i5-9300H | O | O |
| i7-14700 | O | O |

### 핵심 관측

**Affinity 효과 (i5-9300H 4C/8T):**
- Cross-core RTT -13.4% (16.6μs → 14.4μs) — 주 효과는 latency 안정화
- PerCore 4코어: -0.9% (throughput 개선 미미)
- PerCore 8코어(HT): -28.4% — 물리 코어 초과 시 역효과

**Affinity 효과 (i7-14700 20C/28T):**
- Cross-core RTT -42% — 고코어에서 효과 증폭
- PostThroughput +67% (2.9M msg/sec)

**버전 간 변화 (v0.5.10→v0.6.5, i5-9300H):**
- SPSC Queue +231%, Ring Buffer +186%, Timing Wheel +153%
- 퇴보 0건 (MPSC 2P1C / SPSC Concurrent는 벤치마크 비결정성 수정, SessionPtr_Copy는 atomic refcount 전환)

**결론:**
- Affinity의 주 효과는 throughput이 아닌 latency 안정화
- 물리 코어 수 이내에서만 유효, 초과 시 역효과
- 최종 결론은 Linux + Docker 환경 2차 벤치마크로 확정 필요

### gh-pages 배포

- 메인 랜딩 페이지 재구성 (프로젝트 소개 + 벤치마크/쇼케이스 링크)
- 벤치마크 허브 (버전 타임라인)
- 버전별 보고서 구조화 (v0.5.10 / v0.6.1 / v0.6.5)
