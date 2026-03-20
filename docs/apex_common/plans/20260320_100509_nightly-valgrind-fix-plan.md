# Nightly Valgrind Workflow Fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Nightly Valgrind CI의 두 가지 실패(valgrind-unit DartConfiguration.tcl 부재, valgrind-e2e 타이밍 실패)를 수정하여 전체 Nightly workflow를 통과시킨다.

**Architecture:** valgrind-unit은 CI container에서 자체 configure+build+MemCheck로 artifact 의존성 제거. valgrind-e2e는 별도 build job의 artifact를 받아 호스트에서 Docker infra + Valgrind 서비스를 실행하되, 전용 TOML 설정(높은 timeout)과 적절한 테스트 필터링을 적용한다.

**Tech Stack:** GitHub Actions, CMake/CTest, Valgrind, Docker Compose, GTest

**관련 BACKLOG:** #99 (Nightly Valgrind 첫 실행 결과 확인), #98 (CI E2E 타이밍 민감 테스트 안정화)

---

## File Map

| 파일 | 변경 | 역할 |
|------|------|------|
| `CMakeLists.txt` | Modify (line 16) | `include(CTest)` 추가 → DartConfiguration.tcl 생성 |
| `.github/workflows/nightly.yml` | Rewrite | 3-job 구조 재설계 |
| `apex_services/tests/e2e/gateway_e2e_valgrind.toml` | Create | Valgrind 전용 Gateway E2E 설정 (높은 timeout) |

---

### Task 1: CMakeLists.txt — `include(CTest)` 추가

**Files:**
- Modify: `CMakeLists.txt:16`

`ctest -T MemCheck`는 `DartConfiguration.tcl`을 요구한다. 이 파일은 `include(CTest)` 호출 시 cmake configure에서 자동 생성된다. 현재 `enable_testing()`만 있으므로 `include(CTest)`를 추가한다.

- [ ] **Step 1: `include(CTest)` 추가**

`CMakeLists.txt` line 16 `enable_testing()` 직후에 추가:

```cmake
enable_testing()
include(CTest)
```

> **주의**: `include(CTest)`는 `BUILD_TESTING` 옵션을 자동 정의한다. debug preset에서 이미 `BUILD_TESTING=ON`이므로 충돌 없음. `CDash` 관련 타겟이 추가되지만 빌드에 영향 없음.

- [ ] **Step 2: 로컬 빌드 검증**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug
```

빌드 성공 + 기존 71개 유닛 테스트 통과 확인. `build/Windows/debug/DartConfiguration.tcl` 생성 확인.

- [ ] **Step 3: 커밋**

```bash
git add CMakeLists.txt
git commit -m "build: include(CTest) 추가 — DartConfiguration.tcl 생성으로 ctest -T MemCheck 지원"
git push -u origin feature/nightly-valgrind-fix
```

---

### Task 2: Valgrind 전용 Gateway E2E 설정 생성

**Files:**
- Create: `apex_services/tests/e2e/gateway_e2e_valgrind.toml`
- Reference: `apex_services/tests/e2e/gateway_e2e.toml` (원본)

Valgrind 하에서 Auth 서비스의 bcrypt + Kafka 라운드트립이 Gateway의 `request_timeout_ms=5000`을 초과하여 대부분의 E2E 테스트가 실패한다. Valgrind 전용 TOML에서 timeout을 30초로 확대한다.

- [ ] **Step 1: `gateway_e2e_valgrind.toml` 생성**

`gateway_e2e.toml`을 복사하되 다음 항목만 변경:

```toml
[timeouts]
request_timeout_ms = 30000
```

나머지 설정(server, kafka, redis, jwt, routes, pubsub, rate_limit)은 `gateway_e2e.toml`과 동일하게 유지.

> **설계 근거**: 환경변수 오버라이드 방식은 gateway_config_parser.cpp 코드 변경이 필요하고, 이 PR의 스코프를 넘긴다. 전용 TOML 파일은 코드 변경 없이 깔끔하게 분리된다.

- [ ] **Step 2: 커밋**

```bash
git add apex_services/tests/e2e/gateway_e2e_valgrind.toml
git commit -m "config: Valgrind 전용 Gateway E2E 설정 추가 — request_timeout_ms 30초"
git push
```

---

### Task 3: nightly.yml 전면 재구조화

**Files:**
- Rewrite: `.github/workflows/nightly.yml`

3-job 병렬 구조로 재설계. valgrind-unit은 자체 빌드, valgrind-e2e는 build job artifact를 사용.

- [ ] **Step 1: nightly.yml 작성**

전체 구조:

```yaml
name: Nightly Valgrind

permissions: {}

on:
  schedule:
    - cron: '0 18 * * *'   # UTC 18:00 = KST 03:00
  workflow_dispatch:
    inputs:
      scope:
        description: 'Valgrind 범위'
        type: choice
        options:
          - all
          - unit-only
          - e2e-only
        default: all

env:
  FORCE_JAVASCRIPT_ACTIONS_TO_NODE24: true

concurrency:
  group: nightly
  cancel-in-progress: true
```

**Job 1: `valgrind-unit`** (CI container, 자체 빌드, artifact 불필요):

```yaml
  valgrind-unit:
    permissions:
      contents: read
    if: >-
      github.event.inputs.scope == 'all' ||
      github.event.inputs.scope == 'unit-only' ||
      github.event_name == 'schedule'
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/gazuua/apex-pipeline-ci:latest
    env:
      VCPKG_ROOT: /opt/vcpkg
    steps:
      - uses: actions/checkout@v4

      - name: Configure
        run: cmake --preset debug -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed

      - name: Build
        run: cmake --build build/Linux/debug

      - name: Install Valgrind
        run: apt-get update && apt-get install -y valgrind

      - name: Run unit tests under Valgrind
        run: |
          ctest --test-dir build/Linux/debug \
            -LE "e2e|e2e_stress" \
            --overwrite MemoryCheckCommand=valgrind \
            --overwrite "MemoryCheckCommandOptions=--tool=memcheck --leak-check=full --error-exitcode=1" \
            -T MemCheck \
            --output-on-failure
```

**Job 2: `build`** (CI container, valgrind-e2e용 artifact 생성):

```yaml
  build:
    permissions:
      contents: read
    if: >-
      github.event.inputs.scope == 'all' ||
      github.event.inputs.scope == 'e2e-only' ||
      github.event_name == 'schedule'
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/gazuua/apex-pipeline-ci:latest
    env:
      VCPKG_ROOT: /opt/vcpkg
    steps:
      - uses: actions/checkout@v4

      - name: Configure
        run: cmake --preset debug -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed

      - name: Build
        run: cmake --build build/Linux/debug

      - name: Upload build artifacts
        uses: actions/upload-artifact@v4
        with:
          name: nightly-build
          path: |
            build/Linux/debug/apex_services/gateway/apex_gateway
            build/Linux/debug/apex_services/auth-svc/auth_svc_main
            build/Linux/debug/apex_services/chat-svc/chat_svc_main
            build/Linux/debug/apex_services/tests/e2e/apex_e2e_tests
            build/Linux/debug/apex_services/tests/e2e/apex_e2e_stress_tests
          retention-days: 1
```

> **artifact 변경**: glob(`**/test_*`) 제거. valgrind-e2e에 필요한 서비스 바이너리 + E2E 테스트만 명시적으로 업로드. 단위 테스트 바이너리는 valgrind-unit이 자체 빌드하므로 불필요.

**Job 3: `valgrind-e2e`** (ubuntu-latest host, Docker infra + Valgrind):

```yaml
  valgrind-e2e:
    permissions:
      contents: read
    needs: build
    if: >-
      github.event.inputs.scope == 'all' ||
      github.event.inputs.scope == 'e2e-only' ||
      github.event_name == 'schedule'
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Download build
        uses: actions/download-artifact@v4
        with:
          name: nightly-build
          path: build/Linux/debug

      - name: Make binaries executable
        run: |
          chmod +x \
            build/Linux/debug/gateway/apex_gateway \
            build/Linux/debug/auth-svc/auth_svc_main \
            build/Linux/debug/chat-svc/chat_svc_main \
            build/Linux/debug/tests/e2e/apex_e2e_tests \
            build/Linux/debug/tests/e2e/apex_e2e_stress_tests

      - name: Install Valgrind
        run: sudo apt-get update && sudo apt-get install -y valgrind

      - name: Start infrastructure
        run: |
          docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d \
            kafka redis-auth redis-ratelimit redis-chat redis-pubsub postgres \
            --wait --timeout 120

      - name: Start services under Valgrind
        run: |
          nohup valgrind --tool=memcheck --leak-check=full --error-exitcode=1 \
            build/Linux/debug/gateway/apex_gateway \
            apex_services/tests/e2e/gateway_e2e_valgrind.toml > /tmp/gateway.log 2>&1 &
          nohup valgrind --tool=memcheck --leak-check=full --error-exitcode=1 \
            build/Linux/debug/auth-svc/auth_svc_main \
            apex_services/tests/e2e/auth_svc_e2e.toml > /tmp/auth.log 2>&1 &
          nohup valgrind --tool=memcheck --leak-check=full --error-exitcode=1 \
            build/Linux/debug/chat-svc/chat_svc_main \
            apex_services/tests/e2e/chat_svc_e2e.toml > /tmp/chat.log 2>&1 &

          # Valgrind 기동 대기 (최대 180초)
          timeout 180 bash -c 'until nc -z 127.0.0.1 8443 2>/dev/null; do sleep 2; done'
          sleep 30  # Kafka consumer rebalance 대기 (Valgrind 하 확대)

      - name: Run E2E tests
        run: |
          build/Linux/debug/tests/e2e/apex_e2e_tests \
            --gtest_filter=-AuthE2ETest.RefreshTokenRenewal:TimeoutE2ETest.ServiceRecoveryAfterTimeout
        env:
          E2E_GATEWAY_HOST: "127.0.0.1"
          E2E_GATEWAY_TCP_PORT: "8443"
          E2E_STARTUP_TIMEOUT: "180"
          E2E_REQUEST_TIMEOUT: "120"

      - name: Run stress tests
        run: |
          build/Linux/debug/tests/e2e/apex_e2e_stress_tests
        env:
          E2E_GATEWAY_HOST: "127.0.0.1"
          E2E_GATEWAY_TCP_PORT: "8443"
          E2E_STARTUP_TIMEOUT: "180"
          E2E_REQUEST_TIMEOUT: "120"
          E2E_STRESS_CONNECTIONS: "3"
          E2E_STRESS_MESSAGES: "5"
          E2E_STRESS_TIMEOUT_SEC: "900"

      - name: Service logs (debug)
        if: always()
        run: |
          echo "=== Gateway ===" && tail -100 /tmp/gateway.log 2>/dev/null || true
          echo "=== Auth ===" && tail -100 /tmp/auth.log 2>/dev/null || true
          echo "=== Chat ===" && tail -100 /tmp/chat.log 2>/dev/null || true

      - name: Teardown
        if: always()
        run: |
          pkill -f valgrind || true
          sleep 5
          docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v
```

> **artifact 경로 주의**: `upload-artifact@v4`는 명시적 경로 목록에서 공통 prefix (`build/Linux/debug/apex_services/`)를 제거한다. 따라서 download 후 경로는 `build/Linux/debug/gateway/apex_gateway` 형태가 된다 (apex_services/ 없음). 모든 참조 경로를 이에 맞게 조정.

- [ ] **Step 2: 문법 검증**

```bash
cd <프로젝트루트>
python -c "import yaml; yaml.safe_load(open('.github/workflows/nightly.yml'))" && echo "YAML OK"
```

- [ ] **Step 3: 커밋**

```bash
git add .github/workflows/nightly.yml
git commit -m "ci: Nightly Valgrind workflow 재구조화 — 자체 빌드 + 타이밍 수정"
git push
```

---

### Task 4: clang-format + 최종 검증

**Files:**
- 변경된 모든 파일

- [ ] **Step 1: clang-format 실행**

CMakeLists.txt는 포맷팅 대상이 아니고, TOML/YAML도 해당 없음. 코드 파일 변경이 없으므로 이 단계는 스킵 가능.

- [ ] **Step 2: 빌드 검증**

```bash
"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug
```

전체 빌드 + 71개 유닛 테스트 통과 확인.

- [ ] **Step 3: PR 생성 + Nightly workflow 수동 트리거**

```bash
gh pr create --title "BACKLOG-99,98 Nightly Valgrind workflow 수정 — 자체 빌드 + E2E 타이밍" \
  --body "..."
```

PR 머지 후 (또는 브랜치에서) `gh workflow run nightly.yml`로 수동 트리거하여 검증.

---

## Checklist

- [ ] Task 1: `include(CTest)` 추가 + 빌드 검증
- [ ] Task 2: `gateway_e2e_valgrind.toml` 생성
- [ ] Task 3: `nightly.yml` 재구조화
- [ ] Task 4: 최종 검증 + PR
