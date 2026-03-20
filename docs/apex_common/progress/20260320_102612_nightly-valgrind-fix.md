# v0.5.8.2 Nightly Valgrind 수정 + CI E2E 타이밍 안정화

**PR**: #50
**백로그**: BACKLOG-98, BACKLOG-99
**브랜치**: feature/nightly-valgrind-fix

---

## 작업 요약

v0.5.8.0에서 도입한 Nightly Valgrind workflow의 첫 수동 실행 실패 2건과 CI E2E 타이밍 민감 테스트 안정화를 동시에 해결.

## BACKLOG-99: Nightly Valgrind 수정

### 원인 분석

1. **valgrind-unit**: `ctest -T MemCheck`가 `DartConfiguration.tcl`을 요구하지만, 루트 `CMakeLists.txt`에 `include(CTest)`가 없어 파일 미생성
2. **valgrind-e2e**: Gateway `request_timeout_ms=5000`이 Valgrind 10-20x 감속 하에서 bcrypt + Kafka 라운드트립 시간 초과

### 수정 내용

| 파일 | 변경 |
|------|------|
| `CMakeLists.txt` | `include(CTest)` 추가 (DartConfiguration.tcl 생성) |
| `.github/workflows/nightly.yml` | 3-job 병렬 구조로 재설계: valgrind-unit(자체 빌드), build(아티팩트), valgrind-e2e(호스트 실행) |
| `apex_services/tests/e2e/gateway_e2e_valgrind.toml` | 신규 — `request_timeout_ms=30000` (기본 5s → 30s) |

### nightly.yml 주요 변경

- **valgrind-unit**: CI 컨테이너 내 자체 configure+build+MemCheck (아티팩트 의존 제거)
- **valgrind-e2e**: Gateway TCP 포트 대기 180초, Kafka rebalance 30초 대기, 스트레스 테스트 3연결/5메시지로 축소
- **필터링**: RefreshTokenRenewal, ServiceRecoveryAfterTimeout, KafkaReconnect, RedisReconnect 제외 (Valgrind 환경 특성)

## BACKLOG-98: CI E2E 타이밍 안정화

### 원인 분석

1. **RefreshTokenRenewal**: `access_token_ttl_sec=30` → 31초 sleep 필요 → CI 환경 지연 시 타임아웃
2. **ServiceRecoveryAfterTimeout**: `recv()` 기본 타임아웃 10초가 Kafka 라운드트립에 불충분

### 수정 내용

| 파일 | 변경 |
|------|------|
| `apex_services/tests/e2e/auth_svc_e2e.toml` | `access_token_ttl_sec` 30 → 10 |
| `apex_services/tests/e2e/e2e_auth_test.cpp` | RefreshTokenRenewal sleep 31s → 11s |
| `apex_services/tests/e2e/e2e_test_fixture.hpp` | TcpClient `recv()` 기본 타임아웃 10s → 30s |
| `.github/workflows/ci.yml` | E2E `--gtest_filter` 제거 — 11개 전체 실행 |

## 변경 파일 총 8개

- `CMakeLists.txt`
- `.github/workflows/nightly.yml`
- `.github/workflows/ci.yml`
- `apex_services/tests/e2e/gateway_e2e_valgrind.toml` (신규)
- `apex_services/tests/e2e/auth_svc_e2e.toml`
- `apex_services/tests/e2e/e2e_auth_test.cpp`
- `apex_services/tests/e2e/e2e_test_fixture.hpp`
- `docs/apex_common/plans/20260320_100509_nightly-valgrind-fix-plan.md` (신규)

## 테스트 결과

- 71/71 유닛 테스트 통과 (로컬 MSVC 빌드)
- CI 전체 통과 (GCC debug + ASAN + TSAN + UBSAN + Windows MSVC + E2E)
