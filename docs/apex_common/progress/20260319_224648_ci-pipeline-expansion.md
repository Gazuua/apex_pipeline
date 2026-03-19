# Progress: CI 파이프라인 확장 (v0.5.8.0)

**작업 일시**: 2026-03-19 22:46:48
**버전**: v0.5.8.0
**백로그**: BACKLOG-7, BACKLOG-9

---

## 작업 요약

CI 파이프라인 전면 확장. build matrix 루트 빌드 통합, UBSAN preset, Docker 기반 E2E 통합, Nightly Valgrind 워크플로우 신설.

---

## 구현 결과

### Task 1 — UBSAN CMake Preset

- `CMakePresets.json`에 `linux-ubsan` preset 추가
- `-fsanitize=undefined -fno-sanitize-recover=all` 플래그 적용
- ci.yml build matrix에 `linux-ubsan` 추가

### Task 2 — CI Build Matrix 루트 빌드 전환

- ci.yml의 Windows/Linux build job을 루트 레벨 빌드로 전환
- 기존 `build-root` 중복 job 제거
- `apex_core` + `apex_shared` 전체 동시 검증 (BACKLOG-9 해결)
- MSVC Windows 빌드도 루트 단위로 통합

### Task 3 — 서비스 Dockerfile 3개

- `apex_infra/docker/gateway.Dockerfile` 신설 (multistage: builder + runtime)
- `apex_infra/docker/auth-svc.Dockerfile` 신설
- `apex_infra/docker/chat-svc.Dockerfile` 신설
- 모두 `--target runtime` + `--build-arg PREBUILT_BIN` 패턴 사용 (CI 사전 빌드 바이너리 주입)

### Task 4 — docker-compose.e2e.yml 서비스 컨테이너 추가

- `docker-compose.e2e.yml`에 `apex-gateway`, `apex-auth-svc`, `apex-chat-svc` 서비스 추가
- 인프라(Redis×4, Kafka, PostgreSQL) + 서비스 3개 단일 compose 파일로 통합
- `--wait` 플래그로 모든 healthcheck 통과까지 대기
- TOML 설정 파일 마운트 (gateway_e2e.toml 등)

### Task 5 — 스트레스 E2E 테스트 12개

- `e2e_stress_connection.cpp`: RapidConnectDisconnect, HalfOpenConnection, DisconnectDuringResponse
- `e2e_stress_concurrency.cpp`: ConcurrentRoomJoinLeave, ConcurrentLogin, RateLimitBurst
- `e2e_stress_infra.cpp`: MassTimeouts, KafkaReconnect, RedisReconnect
- `e2e_stress_protocol.cpp`: IncompleteFrame, MaxSizeMessage, InvalidMsgIdFlood
- 별도 바이너리 `apex_e2e_stress_tests`로 빌드
- 환경변수 `E2E_STRESS_CONNECTIONS`, `E2E_STRESS_MESSAGES`로 파라미터 제어

### Task 6 — Valgrind docker-compose override

- `apex_infra/docker/docker-compose.valgrind.yml` override 파일 신설
- 서비스 컨테이너에 Valgrind memcheck 래핑 옵션 적용
- nightly.yml에서 override 구성으로 활성화

### Task 7 — E2E Fixture 리팩토링 (CreateProcessW → Docker)

- `E2EServiceFixture`: `CreateProcessW` 기반 프로세스 기동 코드 제거
- Docker 기반 실행으로 전환 — 테스트 바이너리는 이미 기동된 서비스에 접속만
- `E2E_GATEWAY_HOST`, `E2E_GATEWAY_TCP_PORT`, `E2E_STARTUP_TIMEOUT` 환경변수로 접속 정보 주입
- 좀비 프로세스 문제 근본 해결

### Task 8 — CI E2E Job

- `ci.yml`에 `e2e` job 추가 (build job 성공 시 실행)
- 사전 빌드된 바이너리로 서비스 이미지 빌드 → docker compose up → apex_e2e_tests → compose down
- 테스트 결과 XML artifact 업로드 (7일 보존)

### Task 9 — Nightly Valgrind Workflow

- `.github/workflows/nightly.yml` 신설
- cron 스케줄: 매일 02:00 UTC + `workflow_dispatch` 수동 트리거
- job 구성: `build` → `valgrind-unit` → `e2e` (unit + E2E + stress 순차 실행)
- valgrind-unit: `ctest -LE "e2e|e2e_stress"` 단위 테스트만 memcheck
- e2e: docker compose + apex_e2e_tests + apex_e2e_stress_tests

---

## 변경 파일

| 범주 | 파일 수 | 주요 파일 |
|------|---------|-----------|
| CMake | 1 | `CMakePresets.json` |
| CI workflow | 2 | `.github/workflows/ci.yml`, `.github/workflows/nightly.yml` |
| Dockerfile | 3 | `gateway.Dockerfile`, `auth-svc.Dockerfile`, `chat-svc.Dockerfile` |
| Docker Compose | 2 | `docker-compose.e2e.yml`, `docker-compose.valgrind.yml` |
| E2E 테스트 | 5 | `e2e_stress_*.cpp` 4개 + fixture 리팩토링 |
| CMake (E2E) | 1 | `apex_services/tests/e2e/CMakeLists.txt` |

총 약 14개 파일 변경/신설.

---

## 테스트 결과

- 유닛 테스트: 71/71 통과
- E2E 테스트: 11/11 통과
- 스트레스 테스트: 12/12 통과
- CI: GCC debug, ASAN, TSAN, MSVC, linux-ubsan, format-check 전체 통과
- Nightly Valgrind: unit + E2E + stress 무결성 확인
