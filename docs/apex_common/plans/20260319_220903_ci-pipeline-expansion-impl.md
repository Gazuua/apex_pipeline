# CI 파이프라인 확장 — 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** CI가 전체 코드베이스를 전방위 검증하도록 확장 (BACKLOG-7, BACKLOG-9)

**Architecture:** 모든 빌드 잡을 루트 레벨로 통합하고, UBSAN 추가, 서비스를 Docker 컨테이너로 패키징하여 E2E를 CI에 통합, Nightly Valgrind memcheck + 스트레스 테스트

**Tech Stack:** GitHub Actions, Docker multi-stage, CMake presets, GTest, Valgrind, ctest labels

**설계 문서:** `docs/apex_common/plans/20260319_215538_ci-pipeline-expansion-design.md`

**필수 사전 참조:** `docs/apex_core/apex_core_guide.md` (코어 프레임워크 설계 원칙)

---

## 파일 구조

### 신규 파일

| 파일 | 책임 |
|------|------|
| `cmake/ApexWarnings.cmake` | 이미 존재 — ubsan 관련 변경 없음 |
| `.github/workflows/nightly.yml` | Nightly Valgrind 워크플로우 |
| `apex_infra/docker/gateway.Dockerfile` | Gateway multi-stage (builder → runtime → valgrind) |
| `apex_infra/docker/auth-svc.Dockerfile` | AuthService multi-stage |
| `apex_infra/docker/chat-svc.Dockerfile` | ChatService multi-stage |
| `apex_infra/docker/docker-compose.valgrind.yml` | Valgrind entrypoint override |
| `apex_services/tests/e2e/e2e_stress_connection.cpp` | 연결 수명 스트레스 테스트 |
| `apex_services/tests/e2e/e2e_stress_protocol.cpp` | 프로토콜 에지케이스 테스트 |
| `apex_services/tests/e2e/e2e_stress_infra.cpp` | 인프라 장애 시뮬레이션 테스트 |
| `apex_services/tests/e2e/e2e_stress_concurrency.cpp` | 동시성 경로 테스트 |

### 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `CMakePresets.json` | ubsan configure/build/test preset 추가 |
| `.github/workflows/ci.yml` | build matrix 루트 전환, build-root 제거, ubsan 추가, e2e job 추가 |
| `apex_infra/docker/docker-compose.e2e.yml` | 서비스 컨테이너 3개 + Kafka advertised listener 변경 |
| `apex_services/tests/e2e/gateway_e2e.toml` | 인프라 주소 Docker 서비스명으로 변경 |
| `apex_services/tests/e2e/auth_svc_e2e.toml` | 인프라 주소 Docker 서비스명으로 변경 |
| `apex_services/tests/e2e/chat_svc_e2e.toml` | 인프라 주소 Docker 서비스명으로 변경 |
| `apex_services/tests/e2e/e2e_test_fixture.hpp` | CreateProcessW/ChildProcess 제거, 환경변수 기반 접속 |
| `apex_services/tests/e2e/e2e_test_fixture.cpp` | 프로세스 관리 제거, health check만 유지 |
| `apex_services/tests/e2e/CMakeLists.txt` | 스트레스 테스트 추가, 라벨 설정, compile definitions 정리 |
| `apex_services/tests/e2e/CLAUDE.md` | Docker 기반 실행 가이드로 갱신 |

---

## 의존 관계

```
Task 1 (UBSAN preset)  ─────────────────────┐
                                             ▼
Task 2 (CI build matrix 통합) ──────────── Task 8 (CI E2E job)
                                             ▲
Task 3 (서비스 Dockerfiles) ──┐              │
                              ▼              │
Task 4 (docker-compose.e2e  ── Task 7 (E2E fixture rework) ─┘
         + TOML 변경)            ▲
                              │
Task 5 (스트레스 테스트) ─────┘ (CMakeLists 통합)

Task 6 (Valgrind override) ──── Task 9 (Nightly workflow)

Task 10 (문서 갱신 + 정리) ── 모든 태스크 완료 후
```

**병렬 가능**: Task 1, 3, 5는 독립 — 동시 진행 가능

---

## Task 1: UBSAN CMake Preset

**Files:**
- Modify: `CMakePresets.json`

- [ ] **Step 1: ubsan configure preset 추가**

`CMakePresets.json`의 `configurePresets` 배열, `tsan` 항목 뒤에 추가:

```json
{
  "name": "ubsan",
  "displayName": "Debug + UndefinedBehaviorSanitizer (Clang/GCC only)",
  "inherits": "debug",
  "cacheVariables": {
    "CMAKE_CXX_FLAGS": "-fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=all",
    "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=undefined",
    "APEX_BUILD_VARIANT": "ubsan"
  }
}
```

- [ ] **Step 2: ubsan build preset 추가**

`buildPresets` 배열에 추가:

```json
{ "name": "ubsan", "configurePreset": "ubsan" }
```

- [ ] **Step 3: ubsan test preset 추가**

`testPresets` 배열, `tsan` 항목 뒤에 추가:

```json
{
  "name": "ubsan",
  "configurePreset": "ubsan",
  "output": { "outputOnFailure": true },
  "environment": {
    "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"
  }
}
```

Note: `apex_core/CMakePresets.json`은 루트 프리셋을 `include`로 상속하는 wrapper이므로 별도 변경 불필요 — ubsan이 자동 포함됨.

- [ ] **Step 4: 커밋**

```bash
git add CMakePresets.json
git commit -m "feat(ci): BACKLOG-7 UBSAN CMake preset 추가

configure/build/test preset 추가. -fno-sanitize-recover=all로 UB 감지 시 즉시 중단.
UBSAN_OPTIONS는 test preset에 설정 (TSAN/LSAN과 동일 패턴).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: CI Build Matrix 루트 전환

**Files:**
- Modify: `.github/workflows/ci.yml`

**핵심 변경**: 모든 matrix 잡의 `working-directory: apex_core` 제거 → 루트 빌드. `build-root` job 제거. `linux-ubsan` matrix 항목 추가. `linux-gcc` 잡에 아티팩트 업로드 추가.

- [ ] **Step 1: build matrix에 linux-ubsan 추가**

`.github/workflows/ci.yml`의 `matrix.include`에 추가:

```yaml
- name: linux-ubsan
  os: ubuntu-latest
  preset: ubsan
  container: true
```

- [ ] **Step 2: build matrix의 working-directory 제거**

Configure, Build, Test 3개 step에서 `working-directory: apex_core` 제거. 루트에서 실행하도록 변경:

```yaml
# Configure — 기존: working-directory: apex_core
- name: Configure
  run: >-
    cmake --preset ${{ matrix.preset }}
    ${{ matrix.container && '-DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed' || '' }}

# Build — 기존: working-directory: apex_core
- name: Build
  run: cmake --build build/${{ runner.os == 'Windows' && 'Windows' || 'Linux' }}/${{ matrix.preset }}

# Test — 기존: working-directory: apex_core
- name: Test
  run: ctest --preset ${{ matrix.preset }} -LE e2e
```

Note: `ctest --preset`에 `-LE e2e` 추가 — 루트 빌드 시 E2E 테스트가 포함되므로 unit test만 실행.

- [ ] **Step 3: Windows vcpkg 캐시 경로 조정**

캐시 path 변경: `apex_core/vcpkg_installed` → `vcpkg_installed`

```yaml
- name: Cache vcpkg_installed (Windows)
  if: ${{ !matrix.container }}
  uses: actions/cache@v4
  with:
    path: vcpkg_installed
    key: vcpkg-${{ runner.os }}-${{ hashFiles('vcpkg.json', 'apex_core/vcpkg.json', 'apex_shared/vcpkg.json') }}
```

- [ ] **Step 4: linux-gcc 아티팩트 업로드 추가**

Build step 직후, `linux-gcc` 잡에서만 서비스 바이너리를 업로드:

```yaml
- name: Upload service binaries
  if: matrix.name == 'linux-gcc'
  uses: actions/upload-artifact@v4
  with:
    name: service-binaries
    path: |
      build/Linux/debug/bin/apex_gateway
      build/Linux/debug/bin/auth_svc_main
      build/Linux/debug/bin/chat_svc_main
      build/Linux/debug/bin/apex_e2e_tests
    retention-days: 1
```

- [ ] **Step 5: build-root job 삭제**

`ci.yml`에서 `build-root` job 전체 삭제 (167~194행). matrix `build` job이 이미 루트 레벨 전체 빌드를 수행하므로 중복.

- [ ] **Step 6: 커밋**

```bash
git add .github/workflows/ci.yml
git commit -m "feat(ci): BACKLOG-7,9 빌드 matrix 루트 전환 + UBSAN + 아티팩트 업로드

모든 matrix 잡을 루트 레벨 전체 빌드로 전환 (apex_core+shared+services).
linux-ubsan 추가, build-root 중복 제거, linux-gcc 아티팩트 업로드.
Windows도 루트 빌드로 전환하여 apex_shared 빌드 검증 (BACKLOG-9 해결).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: 서비스 Dockerfiles

**Files:**
- Create: `apex_infra/docker/gateway.Dockerfile`
- Create: `apex_infra/docker/auth-svc.Dockerfile`
- Create: `apex_infra/docker/chat-svc.Dockerfile`

- [ ] **Step 1: gateway.Dockerfile 작성**

```dockerfile
# ── Build stage (self-contained, 로컬/외부 빌드용) ──
FROM ghcr.io/gazuua/apex-pipeline-ci:latest AS builder
COPY . /src
WORKDIR /src
RUN cmake --preset debug -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
    && cmake --build build/Linux/debug --target apex_gateway

# ── Runtime stage ──
# ARG PREBUILT_BIN: CI에서 사전 빌드된 바이너리 경로. 설정 시 builder stage 스킵.
# 미설정 시 builder stage에서 빌드된 바이너리 사용 (로컬/외부 독립 빌드).
FROM ubuntu:24.04 AS runtime
ARG PREBUILT_BIN=""
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libpq5 libsasl2-2 libzstd1 libcurl4 netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*
COPY ${PREBUILT_BIN:-/src/build/Linux/debug/bin/apex_gateway} /app/apex_gateway
COPY apex_services/tests/e2e/gateway_e2e.toml /app/config.toml
COPY apex_services/tests/keys/ /app/keys/
WORKDIR /app
ENTRYPOINT ["/app/apex_gateway", "config.toml"]

# ── Valgrind stage (nightly용) ──
FROM runtime AS valgrind
RUN apt-get update && apt-get install -y --no-install-recommends valgrind \
    && rm -rf /var/lib/apt/lists/*
ENTRYPOINT ["valgrind", "--tool=memcheck", "--leak-check=full", \
            "--error-exitcode=1", \
            "/app/apex_gateway", "config.toml"]
```

**핵심 설계: `ARG PREBUILT_BIN` 패턴**
- **로컬/독립 빌드**: `docker build -f gateway.Dockerfile .` → builder stage에서 소스 컴파일, `PREBUILT_BIN` 미설정이므로 builder 결과물 사용
- **CI 빌드**: `docker build --build-arg PREBUILT_BIN=build/Linux/debug/bin/apex_gateway --target runtime .` → builder stage 완전 스킵, linux-gcc 아티팩트 직접 COPY

Note: JWT 키 파일(`test_rs256_pub.pem`)을 Gateway 이미지에 포함. `gateway_e2e.toml`의 `public_key_file` 경로는 Task 4에서 Docker 경로로 변경. `netcat-openbsd` 추가 (healthcheck용).

- [ ] **Step 2: auth-svc.Dockerfile 작성**

동일 multi-stage 구조. 타겟: `auth_svc_main`. TOML: `auth_svc_e2e.toml`. 키: private+public 키 모두 포함.

- [ ] **Step 3: chat-svc.Dockerfile 작성**

동일 multi-stage 구조. 타겟: `chat_svc_main`. TOML: `chat_svc_e2e.toml`.

- [ ] **Step 4: 로컬 Docker 빌드 테스트**

```bash
# Gateway 이미지 빌드 (프로젝트 루트에서)
docker build -f apex_infra/docker/gateway.Dockerfile -t apex-gateway:e2e .
docker build -f apex_infra/docker/auth-svc.Dockerfile -t apex-auth-svc:e2e .
docker build -f apex_infra/docker/chat-svc.Dockerfile -t apex-chat-svc:e2e .
```

Expected: 3개 이미지 빌드 성공.

- [ ] **Step 5: 커밋**

```bash
git add apex_infra/docker/gateway.Dockerfile apex_infra/docker/auth-svc.Dockerfile apex_infra/docker/chat-svc.Dockerfile
git commit -m "feat(infra): BACKLOG-7 서비스 Docker multi-stage 이미지 추가

Gateway, AuthService, ChatService 각각 builder→runtime→valgrind 3단계.
ARG PREBUILT_BIN 패턴으로 CI에서 builder 스킵 + 아티팩트 직접 COPY.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: docker-compose.e2e.yml 확장 + TOML 변경

**Files:**
- Modify: `apex_infra/docker/docker-compose.e2e.yml`
- Modify: `apex_services/tests/e2e/gateway_e2e.toml`
- Modify: `apex_services/tests/e2e/auth_svc_e2e.toml`
- Modify: `apex_services/tests/e2e/chat_svc_e2e.toml`

- [ ] **Step 1: docker-compose.e2e.yml에 서비스 컨테이너 추가**

`postgres` 서비스 이후, `volumes:` 이전에 3개 서비스 추가:

```yaml
  # --- Application Services ---

  gateway:
    build:
      context: ../..
      dockerfile: apex_infra/docker/gateway.Dockerfile
      target: runtime
    container_name: apex-e2e-gateway
    ports:
      - "8443:8443"
      - "8444:8444"
    depends_on:
      kafka:
        condition: service_healthy
      redis-auth:
        condition: service_healthy
      redis-ratelimit:
        condition: service_healthy
      redis-pubsub:
        condition: service_healthy
    networks:
      - apex-e2e-net
    healthcheck:
      test: ["CMD-SHELL", "nc -z localhost 8443 || exit 1"]
      interval: 3s
      timeout: 2s
      retries: 10

  auth-svc:
    build:
      context: ../..
      dockerfile: apex_infra/docker/auth-svc.Dockerfile
      target: runtime
    container_name: apex-e2e-auth-svc
    depends_on:
      kafka:
        condition: service_healthy
      redis-auth:
        condition: service_healthy
      postgres:
        condition: service_healthy
    networks:
      - apex-e2e-net

  chat-svc:
    build:
      context: ../..
      dockerfile: apex_infra/docker/chat-svc.Dockerfile
      target: runtime
    container_name: apex-e2e-chat-svc
    depends_on:
      kafka:
        condition: service_healthy
      redis-chat:
        condition: service_healthy
      redis-pubsub:
        condition: service_healthy
      postgres:
        condition: service_healthy
    networks:
      - apex-e2e-net
```

- [ ] **Step 2: Kafka advertised_listeners 변경**

Docker 네트워크 내에서 서비스들이 Kafka에 접근하려면 `KAFKA_ADVERTISED_LISTENERS`를 Docker 서비스명으로 변경:

```yaml
KAFKA_ADVERTISED_LISTENERS: PLAINTEXT://kafka:9092
```

기존: `PLAINTEXT://localhost:9092`. 호스트에서 직접 Kafka에 접근할 필요가 없으므로 (E2E 테스트는 Gateway를 통해서만 통신) 이 변경이 안전.

- [ ] **Step 3: TOML 파일 Docker 서비스명으로 변경**

`gateway_e2e.toml`:
```toml
[kafka]
brokers = "kafka:9092"

[redis.auth]
host = "redis-auth"
port = 6379

[redis.pubsub]
host = "redis-pubsub"
port = 6379

[redis.ratelimit]
host = "redis-ratelimit"
port = 6379

[jwt]
public_key_file = "keys/test_rs256_pub.pem"  # Docker 이미지 내 경로
```

`auth_svc_e2e.toml`:
```toml
[jwt]
private_key_file = "keys/test_rs256.pem"
public_key_file = "keys/test_rs256_pub.pem"

[kafka]
brokers = "kafka:9092"

[redis]
host = "redis-auth"
port = 6379

[pg]
connection_string = "host=postgres port=5432 dbname=apex_db user=apex_admin password=apex_e2e_password options='-c search_path=auth_svc,public'"
```

`chat_svc_e2e.toml`:
```toml
[kafka]
brokers = "kafka:9092"

[redis_data]
host = "redis-chat"
port = 6379

[redis_pubsub]
host = "redis-pubsub"
port = 6379

[pg]
connection_string = "host=postgres port=5432 dbname=apex_db user=apex_admin password=apex_e2e_password options='-c search_path=chat_svc,public'"
```

- [ ] **Step 4: docker-compose로 전체 스택 기동 테스트**

```bash
docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d --build --wait --timeout 120
docker compose -f apex_infra/docker/docker-compose.e2e.yml ps
```

Expected: 8개 컨테이너 (Kafka, Redis×4, PG, Gateway, Auth, Chat) 전부 Up/Healthy.

```bash
docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v
```

- [ ] **Step 5: 커밋**

```bash
git add apex_infra/docker/docker-compose.e2e.yml \
        apex_services/tests/e2e/gateway_e2e.toml \
        apex_services/tests/e2e/auth_svc_e2e.toml \
        apex_services/tests/e2e/chat_svc_e2e.toml
git commit -m "feat(infra): BACKLOG-7 docker-compose.e2e.yml 서비스 컨테이너 추가 + TOML Docker 주소 전환

Gateway/Auth/Chat 서비스를 Docker 컨테이너로 기동.
TOML 설정의 인프라 주소를 Docker 서비스명(kafka, redis-auth 등)으로 변경.
Kafka advertised_listeners도 Docker 네트워크 기준으로 변경.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: 스트레스 E2E 테스트

**Files:**
- Create: `apex_services/tests/e2e/e2e_stress_connection.cpp`
- Create: `apex_services/tests/e2e/e2e_stress_protocol.cpp`
- Create: `apex_services/tests/e2e/e2e_stress_infra.cpp`
- Create: `apex_services/tests/e2e/e2e_stress_concurrency.cpp`

**필수 사전 참조:** `docs/apex_core/apex_core_guide.md` — shared-nothing, per-core 독립, intrusive_ptr 수명 관리 등 프레임워크 설계 원칙을 위배하지 않도록 작성.

**파라미터화:** 모든 부하 수준을 환경변수로 주입 가능하게 설계. Valgrind 환경에서는 낮은 값 사용.

```cpp
// 공통 패턴 — 환경변수에서 부하 파라미터 읽기
static int get_env_int(const char* name, int default_val)
{
    const char* val = std::getenv(name);
    return val ? std::atoi(val) : default_val;
}
```

- [ ] **Step 1: e2e_stress_connection.cpp 작성**

`E2EStressFixture` 테스트 클래스 (E2ETestFixture 상속). 3개 테스트:

1. `RapidConnectDisconnect` — `E2E_STRESS_CONNECTIONS`(기본 50)개 클라이언트 동시 접속/해제 반복. 모든 연결이 정상 종료되고 Gateway가 여전히 응답하는지 확인.
2. `HalfOpenConnection` — 메시지 전송 중 소켓을 강제 close. Gateway가 크래시 없이 세션을 정리하는지 확인.
3. `DisconnectDuringResponse` — 로그인 요청 전송 직후 즉시 disconnect. 응답 write 시 세션 소멸 처리 확인.

- [ ] **Step 2: e2e_stress_protocol.cpp 작성**

3개 테스트:

1. `IncompleteFrame` — WireHeader만 전송하고 body 미전송. Gateway가 타임아웃으로 정리하는지 확인.
2. `MaxSizeMessage` — `body_size`를 최대값(예: 1MB)으로 설정한 메시지 전송. 대형 할당 경로 검증.
3. `InvalidMsgIdFlood` — 미등록 msg_id를 `E2E_STRESS_MESSAGES`(기본 100)회 연속 전송. HandlerNotFound 반복 시 leak 없는지 확인.

- [ ] **Step 3: e2e_stress_infra.cpp 작성**

3개 테스트:

1. `MassTimeouts` — 존재하지 않는 서비스 msg_id로 다수 요청 전송 → PendingRequests 타임아웃 대량 발생 경로 검증.
2. `KafkaReconnect` — `docker pause/unpause apex-e2e-kafka`로 Kafka 일시 중단. 재연결 후 요청이 정상 처리되는지 확인. (Docker API 또는 `std::system` 사용)
3. `RedisReconnect` — `docker pause/unpause apex-e2e-redis-auth`로 Redis 일시 중단. 재연결 후 로그인이 정상 동작하는지 확인.

- [ ] **Step 4: e2e_stress_concurrency.cpp 작성**

3개 테스트:

1. `ConcurrentRoomJoinLeave` — 다수 클라이언트가 동시에 같은 방에 입장/퇴장. ChannelSessionMap per-core 경로 검증.
2. `ConcurrentLogin` — 동일 이메일로 동시 로그인 시도. JWT 발급 동시 접근 검증.
3. `RateLimitBurst` — rate limit 임계점에서 동시 요청. SlidingWindowCounter 동시 갱신 검증.

- [ ] **Step 5: 커밋**

```bash
git add apex_services/tests/e2e/e2e_stress_*.cpp
git commit -m "test(e2e): BACKLOG-7 스트레스/에지케이스 E2E 테스트 12개 추가

연결 수명(3), 프로토콜(3), 인프라 장애(3), 동시성(3) 시나리오.
환경변수 파라미터화로 Valgrind/CI/로컬 부하 수준 조절 가능.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Valgrind docker-compose override

**Files:**
- Create: `apex_infra/docker/docker-compose.valgrind.yml`

- [ ] **Step 1: docker-compose.valgrind.yml 작성**

```yaml
# Valgrind override for nightly E2E
# Usage: docker compose -f docker-compose.e2e.yml -f docker-compose.valgrind.yml up

services:
  gateway:
    build:
      target: valgrind
    environment:
      # Valgrind 환경 타임아웃 확대 (10-30배 느림 반영)
      - APEX_REQUEST_TIMEOUT_MS=60000
    healthcheck:
      interval: 15s
      timeout: 30s
      retries: 15
      start_period: 60s

  auth-svc:
    build:
      target: valgrind
    entrypoint: ["valgrind", "--tool=memcheck", "--leak-check=full",
                 "--error-exitcode=1",
                 "/app/auth_svc_main", "config.toml"]

  chat-svc:
    build:
      target: valgrind
    entrypoint: ["valgrind", "--tool=memcheck", "--leak-check=full",
                 "--error-exitcode=1",
                 "/app/chat_svc_main", "config.toml"]

  kafka:
    healthcheck:
      interval: 15s
      timeout: 10s
      retries: 15
```

Note: `request_timeout`은 환경변수 주입(`APEX_REQUEST_TIMEOUT_MS`)으로 오버라이드. 서비스 코드에서 환경변수 우선 적용 로직이 필요할 경우 별도 TOML override 파일(`gateway_valgrind.toml`)로 대체. 구현 시 가장 단순한 방식 선택.

- [ ] **Step 2: 커밋**

```bash
git add apex_infra/docker/docker-compose.valgrind.yml
git commit -m "feat(infra): BACKLOG-7 Valgrind docker-compose override 추가

nightly E2E에서 서비스를 Valgrind memcheck 하에 실행.
healthcheck 타임아웃 확대 + request_timeout 환경변수 오버라이드.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: E2E Fixture 리팩토링

**Files:**
- Modify: `apex_services/tests/e2e/e2e_test_fixture.hpp`
- Modify: `apex_services/tests/e2e/e2e_test_fixture.cpp`
- Modify: `apex_services/tests/e2e/CMakeLists.txt`

**핵심 변경**: `ChildProcess`, `launch_service()`, `terminate_service()`, `CreateProcessW`/`fork` 코드 전부 제거. Docker 서비스가 이미 기동되어 있다고 가정. 환경변수로 접속 주소 주입.

- [ ] **Step 1: e2e_test_fixture.hpp 수정**

제거할 항목:
- `#ifdef _WIN32 ... #include <Windows.h> ... #else ... #include <signal.h> ... #endif` 블록
- `ChildProcess` struct 전체
- `E2EEnvironment`의 `launch_service()`, `terminate_service()`, `gateway_proc_`, `auth_proc_`, `chat_proc_`
- `E2EConfig`의 `gateway_exe`, `auth_svc_exe`, `chat_svc_exe`, `gateway_config`, `auth_svc_config`, `chat_svc_config`, `project_root` 필드 + `#ifdef` 블록

추가할 항목:
- `E2EConfig` 생성자에서 환경변수 읽기:

```cpp
struct E2EConfig
{
    std::string gateway_host = "127.0.0.1";
    uint16_t gateway_tcp_port = 8443;
    uint16_t gateway_ws_port = 8444;

    // Timeouts
    std::chrono::seconds startup_timeout{30};
    std::chrono::seconds request_timeout{10};

    // 환경변수에서 오버라이드
    static E2EConfig from_env();
};
```

- [ ] **Step 2: e2e_test_fixture.cpp 수정**

`E2EConfig::from_env()` 구현:

```cpp
E2EConfig E2EConfig::from_env()
{
    E2EConfig cfg;
    if (auto* v = std::getenv("E2E_GATEWAY_HOST")) cfg.gateway_host = v;
    if (auto* v = std::getenv("E2E_GATEWAY_TCP_PORT")) cfg.gateway_tcp_port = static_cast<uint16_t>(std::atoi(v));
    if (auto* v = std::getenv("E2E_GATEWAY_WS_PORT")) cfg.gateway_ws_port = static_cast<uint16_t>(std::atoi(v));
    if (auto* v = std::getenv("E2E_STARTUP_TIMEOUT")) cfg.startup_timeout = std::chrono::seconds{std::atoi(v)};
    if (auto* v = std::getenv("E2E_REQUEST_TIMEOUT")) cfg.request_timeout = std::chrono::seconds{std::atoi(v)};
    return cfg;
}
```

`E2EEnvironment::SetUp()` 간소화:

```cpp
void E2EEnvironment::SetUp()
{
    config_ = E2EConfig::from_env();

    // Docker 서비스가 이미 기동되어 있다고 가정 — Gateway health check만 수행
    std::cout << "[E2E] Waiting for Gateway at " << config_.gateway_host
              << ":" << config_.gateway_tcp_port << "...\n";

    auto deadline = std::chrono::steady_clock::now() + config_.startup_timeout;
    bool gateway_ready = false;

    while (std::chrono::steady_clock::now() < deadline)
    {
        try
        {
            boost::asio::io_context probe_ctx;
            boost::asio::ip::tcp::socket probe_sock(probe_ctx);
            boost::asio::ip::tcp::endpoint ep(
                boost::asio::ip::make_address(config_.gateway_host),
                config_.gateway_tcp_port);
            probe_sock.connect(ep);
            probe_sock.close();
            gateway_ready = true;
            break;
        }
        catch (...)
        {
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
    }

    ASSERT_TRUE(gateway_ready) << "Gateway not reachable within timeout";

    // Kafka consumer 초기화 대기
    std::this_thread::sleep_for(std::chrono::seconds{8});

    ready_ = true;
    std::cout << "[E2E] Infrastructure ready.\n";
}
```

`E2EEnvironment::TearDown()` 간소화:

```cpp
void E2EEnvironment::TearDown()
{
    ready_ = false;
    std::cout << "[E2E] Tests complete.\n";
}
```

제거: `launch_service()`, `terminate_service()`, `docker compose up/down` 호출, 모든 프로세스 관리 코드, `#ifdef _WIN32` 분기.

- [ ] **Step 3: CMakeLists.txt 수정**

스트레스 테스트 파일 추가 + 라벨 분리 + compile definitions 정리:

```cmake
# 기존 E2E 테스트 (happy-path)
add_executable(apex_e2e_tests
    e2e_test_fixture.cpp
    e2e_auth_test.cpp
    e2e_chat_test.cpp
    e2e_ratelimit_test.cpp
    e2e_timeout_test.cpp
)

# 스트레스 E2E 테스트 (nightly/Valgrind용)
add_executable(apex_e2e_stress_tests
    e2e_test_fixture.cpp
    e2e_stress_connection.cpp
    e2e_stress_protocol.cpp
    e2e_stress_infra.cpp
    e2e_stress_concurrency.cpp
)
```

두 타겟 모두 동일한 link_libraries + warnings 적용. compile_definitions에서 `E2E_GATEWAY_EXE`, `E2E_AUTH_SVC_EXE`, `E2E_CHAT_SVC_EXE`, `E2E_PROJECT_ROOT` 제거 (더 이상 프로세스 기동 안 함).

라벨 설정:

```cmake
gtest_discover_tests(apex_e2e_tests
    PROPERTIES LABELS "e2e"
    DISCOVERY_TIMEOUT 60
)

gtest_discover_tests(apex_e2e_stress_tests
    PROPERTIES LABELS "e2e;e2e_stress"
    DISCOVERY_TIMEOUT 60
)
```

- [ ] **Step 4: 빌드 확인**

```bash
# 로컬 MSVC 빌드로 컴파일만 확인
"<프로젝트루트>/apex_tools/queue-lock.sh" build debug
```

Expected: 71개 유닛 테스트 + E2E/스트레스 바이너리 컴파일 성공.

- [ ] **Step 5: 커밋**

```bash
git add apex_services/tests/e2e/e2e_test_fixture.hpp \
        apex_services/tests/e2e/e2e_test_fixture.cpp \
        apex_services/tests/e2e/CMakeLists.txt
git commit -m "refactor(e2e): BACKLOG-7 fixture Docker 기반 전환 — 프로세스 관리 제거

CreateProcessW/fork 프로세스 기동 코드 제거.
Docker 서비스가 외부에서 기동된 상태를 전제로 health check만 수행.
환경변수(E2E_GATEWAY_HOST 등)로 접속 주소 주입.
스트레스 테스트 바이너리 분리 (apex_e2e_stress_tests), e2e_stress 라벨.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: CI E2E Job

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: e2e job 추가**

`build` job 뒤에 추가. `build` 전체 통과 의존:

```yaml
  # ── E2E integration tests ─────────────────────
  e2e:
    permissions:
      contents: read
    needs: [changes, build]
    if: >-
      always() &&
      needs.changes.outputs.source == 'true' &&
      needs.build.result == 'success'
    name: e2e
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4

      - name: Download service binaries
        uses: actions/download-artifact@v4
        with:
          name: service-binaries
          path: build/Linux/debug/bin

      - name: Make binaries executable
        run: chmod +x build/Linux/debug/bin/*

      - name: Build service Docker images (runtime only, builder stage skipped)
        run: |
          docker build -f apex_infra/docker/gateway.Dockerfile \
            --target runtime \
            --build-arg PREBUILT_BIN=build/Linux/debug/bin/apex_gateway \
            -t apex-gateway:e2e .
          docker build -f apex_infra/docker/auth-svc.Dockerfile \
            --target runtime \
            --build-arg PREBUILT_BIN=build/Linux/debug/bin/auth_svc_main \
            -t apex-auth-svc:e2e .
          docker build -f apex_infra/docker/chat-svc.Dockerfile \
            --target runtime \
            --build-arg PREBUILT_BIN=build/Linux/debug/bin/chat_svc_main \
            -t apex-chat-svc:e2e .

      - name: Start E2E infrastructure
        run: |
          docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d --wait --timeout 120

      - name: Run E2E tests
        run: |
          build/Linux/debug/bin/apex_e2e_tests --gtest_output=xml:e2e-results.xml
        env:
          E2E_GATEWAY_HOST: "127.0.0.1"
          E2E_GATEWAY_TCP_PORT: "8443"
          E2E_STARTUP_TIMEOUT: "60"

      - name: Teardown
        if: always()
        run: docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v

      - name: Upload test results
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: e2e-results
          path: e2e-results.xml
          retention-days: 7
```

Note: `--build-arg PREBUILT_BIN`로 linux-gcc 아티팩트를 직접 주입하여 Docker builder stage를 완전 스킵. Task 3에서 Dockerfile에 `ARG PREBUILT_BIN` 패턴을 구현했으므로 이 방식이 동작.

- [ ] **Step 2: 커밋**

```bash
git add .github/workflows/ci.yml
git commit -m "feat(ci): BACKLOG-7 E2E 테스트 CI job 추가

build matrix 통과 후 Docker 기반 E2E 11개 실행.
linux-gcc 아티팩트 재사용으로 빌드 중복 제거.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Nightly Valgrind Workflow

**Files:**
- Create: `.github/workflows/nightly.yml`

- [ ] **Step 1: nightly.yml 작성**

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

concurrency:
  group: nightly
  cancel-in-progress: true

jobs:
  build:
    permissions:
      contents: read
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
            build/Linux/debug/bin/
            build/Linux/debug/CTestTestfile.cmake
            build/Linux/debug/**/CTestTestfile.cmake
          retention-days: 1

  valgrind-unit:
    permissions:
      contents: read
    needs: build
    if: >-
      github.event.inputs.scope == 'all' ||
      github.event.inputs.scope == 'unit-only' ||
      github.event_name == 'schedule'
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/gazuua/apex-pipeline-ci:latest
    steps:
      - uses: actions/checkout@v4

      - name: Download build
        uses: actions/download-artifact@v4
        with:
          name: nightly-build
          path: build/Linux/debug

      - name: Install Valgrind
        run: apt-get update && apt-get install -y valgrind

      # Note: --test-dir 사용 (--preset 대신) — ctest -T MemCheck + --overwrite 조합이
      # test preset과 호환되지 않으므로 --test-dir로 직접 지정.
      - name: Run unit tests under Valgrind
        run: |
          ctest --test-dir build/Linux/debug \
            -LE "e2e|e2e_stress" \
            --overwrite MemoryCheckCommand=valgrind \
            --overwrite MemoryCheckCommandOptions="--tool=memcheck --leak-check=full --error-exitcode=1" \
            -T MemCheck \
            --output-on-failure

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
        run: chmod +x build/Linux/debug/bin/*

      - name: Start E2E with Valgrind override
        run: |
          docker compose \
            -f apex_infra/docker/docker-compose.e2e.yml \
            -f apex_infra/docker/docker-compose.valgrind.yml \
            up -d --build --wait --timeout 300

      - name: Run E2E + stress tests
        run: |
          build/Linux/debug/bin/apex_e2e_tests
          build/Linux/debug/bin/apex_e2e_stress_tests
        env:
          E2E_GATEWAY_HOST: "127.0.0.1"
          E2E_GATEWAY_TCP_PORT: "8443"
          E2E_STARTUP_TIMEOUT: "120"
          E2E_REQUEST_TIMEOUT: "60"
          E2E_STRESS_CONNECTIONS: "10"
          E2E_STRESS_MESSAGES: "20"
          E2E_STRESS_TIMEOUT_SEC: "600"

      - name: Teardown
        if: always()
        run: |
          docker compose \
            -f apex_infra/docker/docker-compose.e2e.yml \
            -f apex_infra/docker/docker-compose.valgrind.yml \
            down -v
```

- [ ] **Step 2: 커밋**

```bash
git add .github/workflows/nightly.yml
git commit -m "feat(ci): BACKLOG-7 Nightly Valgrind 워크플로우 추가

매일 KST 03:00 스케줄 + workflow_dispatch 수동 트리거.
scope 선택: all/unit-only/e2e-only.
E2E는 Valgrind override로 서비스 memcheck 실행.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: 문서 갱신 + 정리

**Files:**
- Modify: `docs/BACKLOG.md`
- Modify: `docs/BACKLOG_HISTORY.md`
- Modify: `docs/Apex_Pipeline.md`
- Modify: `CLAUDE.md`
- Modify: `README.md`
- Modify: `apex_services/tests/e2e/CLAUDE.md`
- Create: `docs/apex_common/progress/<timestamp>_ci-pipeline-expansion.md`

- [ ] **Step 1: BACKLOG-7, BACKLOG-9 완료 처리**

`docs/BACKLOG.md`에서 BACKLOG-7, BACKLOG-9 항목 삭제.
`docs/BACKLOG_HISTORY.md`에 완료 기록 추가 (FIXED).

- [ ] **Step 2: docs/Apex_Pipeline.md 로드맵 갱신**

활성 로드맵에 현재 버전 업데이트 + v0.6.2.0 부분 선행 완료 기록.

- [ ] **Step 3: CLAUDE.md 로드맵 갱신**

현재 버전 번호 갱신.

- [ ] **Step 4: README.md CI 뱃지/설명 갱신**

CI 구성 변경 사항 반영 (UBSAN 추가, E2E 포함 등).

- [ ] **Step 5: E2E CLAUDE.md 갱신**

Docker 기반 실행 가이드로 전면 갱신. CreateProcessW 관련 트러블슈팅 제거, Docker 기반 실행 순서 업데이트.

- [ ] **Step 6: progress 문서 작성**

작업 결과 요약.

- [ ] **Step 7: 커밋**

```bash
git add docs/ CLAUDE.md README.md apex_services/tests/e2e/CLAUDE.md
git commit -m "docs: BACKLOG-7,9 CI 파이프라인 확장 문서 갱신

BACKLOG-7,9 완료 처리, 로드맵 갱신, E2E 가이드 Docker 기반으로 전면 개편.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```
