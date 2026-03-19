# CI 파이프라인 확장 — 설계 문서

**이슈**: BACKLOG-7 (Linux CI 확장), BACKLOG-9 (Windows apex_shared 미검증)
**브랜치**: `feature/7-9-ci-pipeline`
**작성일**: 2026-03-19

---

## 1. 목표

CI가 전체 코드베이스를 전방위 검증하는 구조를 확립한다.

- 모든 빌드 잡이 모노레포 전체를 빌드+테스트 (apex_core + apex_shared + apex_services)
- UBSAN 추가로 sanitizer 3종 완비 (ASAN + TSAN + UBSAN)
- 서비스를 Docker 컨테이너로 패키징하여 E2E 테스트를 CI에 통합
- Nightly에서 Valgrind memcheck 실행 (unit + E2E + 스트레스 시나리오)
- Windows CI에서 apex_shared/services 빌드 검증

## 2. 설계 원칙

- **코어 프레임워크 가이드 준수**: 모든 테스트 코드는 `docs/apex_core/apex_core_guide.md`의 설계 원칙(shared-nothing, per-core 독립, intrusive_ptr 수명 관리 등)을 위배하지 않는다
- **로컬은 빌드, CI는 검증**: 로컬(MSVC)에서 빠른 빌드 피드백, CI에서 전방위 검증 — 역할 분리. E2E 테스트는 CI(Docker) 전용, 로컬에서는 실행하지 않음
- **Multi-stage Dockerfile**: 서비스 이미지는 빌드+런타임 스테이지를 포함하는 자급자족 구조 (v0.6.2.0 로드맵 중 CI용 이미지 빌드 부분을 선행. 프로덕션 이미지 최적화·레지스트리 배포·Helm 연동은 v0.6 본작업에서 수행)
- **PR CI는 빠르게, Nightly는 철저하게**: 빠른 검증(sanitizer + E2E)은 PR 게이트, 느린 검증(Valgrind)은 nightly로 분리
- **빌드 중복 방지**: PR CI에서 동일 소스의 CMake 빌드가 반복되지 않도록, linux-gcc 잡의 빌드 아티팩트를 E2E job에서 재사용

## 3. CI 아키텍처

### 3.1 PR/Push CI (`ci.yml`)

```
changes (path filter)
    │
    ├── format-check (clang-format 21.1.8)
    │
    ├── build-image (Docker 파일 변경 시만)
    │
    ▼
build (matrix — 전부 루트 레벨 전체 빌드)
    ├── linux-gcc      ── unit test ── 빌드 아티팩트 업로드
    ├── linux-asan     ── unit test
    ├── linux-tsan     ── unit test
    ├── linux-ubsan    ── unit test  ← 신규
    └── windows-msvc   ── unit test
    │
    ▼ (전부 통과 시)
e2e (신규 job)
    ├── linux-gcc 아티팩트 다운로드
    ├── 서비스 바이너리를 런타임 이미지에 COPY (빌드 스테이지 스킵)
    ├── docker-compose up (인프라 + 서비스)
    └── ctest -L e2e (기존 11개)
```

**빌드 아티팩트 재사용 전략**: linux-gcc 잡에서 빌드된 서비스 바이너리(`build/Linux/debug/bin/`)를 `actions/upload-artifact`로 업로드. e2e 잡에서 다운로드 후 런타임 이미지에 직접 COPY — Docker multi-stage의 build stage를 CI에서 스킵하여 빌드 중복 제거.

서비스 Dockerfile은 여전히 self-contained multi-stage를 유지 (로컬·외부에서 독립 빌드 가능). CI에서만 `--target runtime` + 외부 바이너리 COPY로 최적화.

### 3.2 Nightly CI (`nightly.yml`, 매일 KST 03:00 + 수동 트리거)

```
build (root, debug) ── 빌드 아티팩트 업로드
    │
    ├── valgrind-unit
    │     └── 아티팩트 다운로드 → Valgrind 하에서 unit test
    │
    └── valgrind-e2e
          ├── 아티팩트 다운로드 → 런타임+Valgrind 이미지에 COPY
          ├── docker-compose up (valgrind override)
          └── ctest -L e2e (전체: 기존 11개 + 스트레스)
```

수동 트리거 시 scope 선택 가능: `all` / `unit-only` / `e2e-only`

## 4. 주요 변경 사항

### 4.1 빌드 Matrix 통합

**변경**: `build` matrix의 모든 잡에서 `working-directory: apex_core` 제거 → 루트에서 빌드.

- linux-gcc, linux-asan, linux-tsan: 기존 컨테이너 기반, 루트 빌드로 전환
- linux-ubsan: 신규 추가
- windows-msvc: 루트 빌드로 전환

**Windows vcpkg 캐시 경로 변경**: 루트 빌드 전환에 따라:
- 캐시 경로: `apex_core/vcpkg_installed` → `vcpkg_installed` (루트 기준)
- 캐시 key 변경으로 기존 캐시 자동 무효화 — 초회 빌드만 느리고, 이후 새 key로 캐시됨
- `CMakePresets.json`의 `VCPKG_INSTALLED_DIR`은 `${sourceDir}/vcpkg_installed`이므로 루트에서 실행 시 자동으로 루트 기준 적용

**제거**: `build-root` job — matrix가 전부 루트 빌드하므로 중복.

### 4.2 UBSAN 프리셋

`CMakePresets.json`에 `ubsan` configure preset + test preset 추가:

Configure preset:
```json
{
  "name": "ubsan",
  "inherits": "debug",
  "cacheVariables": {
    "CMAKE_CXX_FLAGS": "-fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=all",
    "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=undefined"
  }
}
```

Test preset:
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

`-fno-sanitize-recover=all`: UB 감지 시 즉시 중단. `UBSAN_OPTIONS`는 test preset에 설정 (TSAN/LSAN과 동일 패턴).

### 4.3 서비스 Dockerfiles (multi-stage, 3개)

```
apex_infra/docker/
    ├── ci.Dockerfile              ← 기존 (빌드 환경)
    ├── gateway.Dockerfile         ← 신규
    ├── auth-svc.Dockerfile        ← 신규
    └── chat-svc.Dockerfile        ← 신규
```

각 Dockerfile 구조 (gateway 예시):

```dockerfile
# ── Build stage (self-contained, 로컬/외부 빌드용) ──
FROM ghcr.io/gazuua/apex-pipeline-ci:latest AS builder
COPY . /src
WORKDIR /src
RUN cmake --preset debug -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
    && cmake --build build/Linux/debug --target gateway

# ── Runtime stage ──
FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libpq5 libsasl2-2 && rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/build/Linux/debug/bin/gateway /app/gateway
COPY apex_services/tests/e2e/gateway_e2e.toml /app/config.toml
ENTRYPOINT ["/app/gateway", "--config", "/app/config.toml"]

# ── Valgrind stage (nightly용) ──
FROM runtime AS valgrind
RUN apt-get update && apt-get install -y --no-install-recommends valgrind \
    && rm -rf /var/lib/apt/lists/*
```

서비스별 TOML 설정 파일:
- Gateway: `apex_services/tests/e2e/gateway_e2e.toml`
- AuthService: `apex_services/tests/e2e/auth_svc_e2e.toml`
- ChatService: `apex_services/tests/e2e/chat_svc_e2e.toml`

CI에서는 build stage를 스킵하고 linux-gcc 아티팩트를 직접 COPY하는 별도 경로 사용 (§3.1 참조).

### 4.4 docker-compose.e2e.yml 확장 + 네트워크 토폴로지

기존 인프라(Kafka, Redis×4, PostgreSQL)에 서비스 컨테이너 3개 추가.

**Docker 네트워크 토폴로지**:

```
┌─ docker-compose e2e network ─────────────────────────┐
│                                                       │
│  kafka:9092   redis-auth:6379   postgres:5432         │
│  redis-ratelimit:6379  redis-chat:6379                │
│  redis-pubsub:6379                                    │
│                                                       │
│  gateway:8443 (TCP) / 8444 (WS)                       │
│    → kafka, redis-*, postgres 는 서비스명으로 접근     │
│  auth-svc (내부 포트만, 외부 노출 불필요)              │
│  chat-svc (내부 포트만, 외부 노출 불필요)              │
│                                                       │
├───────────────────────────────────────────────────────┤
│  ports: gateway:8443→host:8443, gateway:8444→host:8444│
└───────────────────────────────────────────────────────┘

Host (CI runner / E2E 테스트 바이너리)
  → localhost:8443 / localhost:8444 로 Gateway 접속
```

- **서비스 간 통신**: Docker 네트워크 내 서비스명 사용 (`kafka`, `redis-auth`, `postgres` 등)
- **서비스 TOML 설정**: 인프라 주소를 Docker 서비스명으로 변경 (기존 `localhost` → `kafka`, `redis-auth` 등). E2E 전용 TOML이므로 기존 로컬 TOML에 영향 없음
- **E2E 테스트 바이너리**: 호스트(CI runner)에서 실행, `localhost:8443`/`localhost:8444`로 Gateway에 접속
- **Gateway 포트**: 기존 코드베이스의 `8443`(TCP)/`8444`(WS) 유지, Docker port mapping으로 호스트에 노출

### 4.5 Valgrind docker-compose override

`docker-compose.valgrind.yml`:
- 서비스 컨테이너의 build target을 `valgrind` stage로 변경
- entrypoint를 `valgrind --tool=memcheck --leak-check=full --error-exitcode=1 /app/<서비스>`로 오버라이드

**Valgrind 모드 타임아웃 확대** (10~30배 느림 반영):

| 타임아웃 항목 | 기본값 | Valgrind 모드 |
|--------------|--------|---------------|
| 서비스 TOML `request_timeout` | 5s | 60s |
| docker-compose healthcheck `interval` | 3s | 15s |
| docker-compose healthcheck `timeout` | 2s | 30s |
| docker-compose healthcheck `retries` | 5 | 15 |
| E2E fixture startup 대기 | 10s | 120s |
| ctest `TIMEOUT` property (스트레스 테스트) | 60s | 600s |

Valgrind 전용 TOML override 파일 또는 환경변수 주입으로 적용.

### 4.6 E2E Fixture 변경

**제거**:
- `CreateProcessW` 기반 프로세스 관리 코드 (LaunchService/StopService)
- 서비스 바이너리 경로 설정 로직

**변경**:
- Docker 서비스가 이미 기동되어 있다고 가정
- 접속 주소를 환경변수로 주입 (기본값 `localhost` — CI와 동일):
  - `E2E_GATEWAY_HOST` (default: `localhost`)
  - `E2E_GATEWAY_TCP_PORT` (default: `8443`)
  - `E2E_GATEWAY_WS_PORT` (default: `8444`)
- health check 대기 로직만 유지 (TCP 연결 시도로 서비스 ready 확인)

**로컬 E2E 실행**: Docker 통일. `docker compose -f docker-compose.e2e.yml up` 후 `ctest -L e2e` 실행. 로컬 Windows에서 서비스를 직접 기동하는 경로는 제거.

### 4.7 Nightly Workflow (`nightly.yml`)

```yaml
on:
  schedule:
    - cron: '0 18 * * *'   # UTC 18:00 = KST 03:00
  workflow_dispatch:
    inputs:
      scope:
        description: 'Valgrind 범위'
        type: choice
        options: [all, unit-only, e2e-only]
        default: all
```

Job 구조: `build` → `valgrind-unit` + `valgrind-e2e` (병렬). 빌드 아티팩트는 `actions/upload-artifact` / `actions/download-artifact`로 전달.

## 5. 스트레스/에지케이스 E2E 시나리오

기존 11개 E2E는 happy-path 위주. Valgrind이 메모리 이슈를 탐지하려면 에지케이스 경로를 의도적으로 실행해야 한다.

모든 스트레스 테스트는 `docs/apex_core/apex_core_guide.md`의 설계 원칙을 준수한다.

### 5.1 연결 수명 스트레스 (`e2e_stress_connection.cpp`)

| 시나리오 | 검증 포인트 |
|----------|------------|
| 다수 클라이언트 동시 접속/해제 (rapid connect/disconnect) | Session SlabAllocator 할당/해제 사이클, intrusive_ptr refcount |
| 메시지 수신 중 클라이언트 강제 종료 (half-open) | 코루틴 중단 시 자원 정리, RingBuffer 잔여 데이터 |
| 서비스 처리 중 클라이언트 disconnect | 응답 write 시점에 세션 소멸, dangling pointer |

### 5.2 프로토콜 에지케이스 (`e2e_stress_protocol.cpp`)

| 시나리오 | 검증 포인트 |
|----------|------------|
| 불완전 프레임 전송 (헤더만, body 일부) | FrameCodec 파싱 중단, 버퍼 정리 |
| 최대 크기 메시지 전송 | ArenaAllocator 블록 체이닝, 대형 할당 fallback |
| 잘못된 msg_id 연속 전송 | HandlerNotFound 경로 반복 시 leak 여부 |

### 5.3 인프라 장애 시뮬레이션 (`e2e_stress_infra.cpp`)

| 시나리오 | 검증 포인트 |
|----------|------------|
| 서비스 응답 지연 시 타임아웃 대량 발생 | PendingRequests 타임아웃 정리, 코루틴 취소 |
| Kafka 일시 중단 후 재연결 | KafkaConsumer/Producer 재연결 경로의 자원 관리 |
| Redis 일시 중단 후 재연결 | RedisMultiplexer 재연결, pending 요청 cancel |

### 5.4 동시성 경로 (`e2e_stress_concurrency.cpp`)

| 시나리오 | 검증 포인트 |
|----------|------------|
| 같은 채팅방에 다수 동시 입장/퇴장 | ChannelSessionMap per-core 경로, broadcast fanout |
| 동일 유저 다중 로그인 시도 | JWT 발급/블랙리스트 동시 접근 |
| Rate limit 임계점 동시 요청 | SlidingWindowCounter 동시 갱신 |

### 5.5 라벨 및 파라미터화

**라벨 전략**: 스트레스 테스트에 `e2e` 라벨을 함께 부여. `ctest -L e2e`로 전체(기존 + 스트레스) 실행. PR CI에서는 `-LE e2e_stress`로 스트레스만 제외.

```
ctest -L e2e -LE e2e_stress     # 기존 11개만 (PR CI용)
ctest -L e2e_stress             # 스트레스만
ctest -L e2e                    # 전부 (nightly 전체)
```

**파라미터화**: 스트레스 테스트의 부하 수준(연결 수, 메시지 수 등)을 환경변수로 주입 가능하게 설계. Valgrind 하에서 10~30배 느려지므로 CI와 로컬에서 다른 값 사용 가능.

| 환경변수 | 기본값 | Valgrind 모드 | 설명 |
|----------|--------|---------------|------|
| `E2E_STRESS_CONNECTIONS` | 50 | 10 | 동시 접속 수 |
| `E2E_STRESS_MESSAGES` | 100 | 20 | 메시지 반복 수 |
| `E2E_STRESS_TIMEOUT_SEC` | 60 | 600 | 테스트 타임아웃 |

## 6. 파일 변경 목록

### 신규 파일

| 파일 | 설명 |
|------|------|
| `.github/workflows/nightly.yml` | Nightly Valgrind 워크플로우 (schedule + workflow_dispatch) |
| `apex_infra/docker/gateway.Dockerfile` | Gateway multi-stage (builder → runtime → valgrind) |
| `apex_infra/docker/auth-svc.Dockerfile` | AuthService multi-stage |
| `apex_infra/docker/chat-svc.Dockerfile` | ChatService multi-stage |
| `apex_infra/docker/docker-compose.valgrind.yml` | Valgrind override (entrypoint + 타임아웃 확대) |
| `apex_services/tests/e2e/e2e_stress_connection.cpp` | 연결 수명 스트레스 |
| `apex_services/tests/e2e/e2e_stress_protocol.cpp` | 프로토콜 에지케이스 |
| `apex_services/tests/e2e/e2e_stress_infra.cpp` | 인프라 장애 시뮬레이션 |
| `apex_services/tests/e2e/e2e_stress_concurrency.cpp` | 동시성 경로 |

### 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `.github/workflows/ci.yml` | build matrix 루트 전환, build-root 제거, ubsan 추가, e2e job 추가, linux-gcc 아티팩트 업로드 |
| `CMakePresets.json` | ubsan configure preset + test preset 추가 |
| `apex_core/CMakePresets.json` | ubsan 상속 (필요 시) |
| `apex_infra/docker/docker-compose.e2e.yml` | 서비스 컨테이너 3개 추가, 네트워크 토폴로지 설정 |
| `apex_services/tests/e2e/e2e_test_fixture.cpp` | CreateProcessW 제거, Docker 서비스 접속 방식 전환, 환경변수 기반 주소 주입 |
| `apex_services/tests/e2e/e2e_test_fixture.hpp` | 인터페이스 변경 (프로세스 관리 → 접속 전용) |
| `apex_services/tests/e2e/CMakeLists.txt` | 스트레스 테스트 파일 추가, e2e/e2e_stress 라벨 설정 |
| `apex_services/tests/e2e/gateway_e2e.toml` | 인프라 주소를 Docker 서비스명으로 변경 |
| `apex_services/tests/e2e/auth_svc_e2e.toml` | 인프라 주소를 Docker 서비스명으로 변경 |
| `apex_services/tests/e2e/chat_svc_e2e.toml` | 인프라 주소를 Docker 서비스명으로 변경 |
| `CLAUDE.md` | 설계 원칙에 코어 프레임워크 가이드 필독 추가 |

### 삭제

| 항목 | 사유 |
|------|------|
| `ci.yml`의 `build-root` job | matrix가 전부 루트 빌드하므로 중복 |

## 7. 베이스라인

본 설계는 아래 main 커밋 위에서 작업한다.

| 커밋 | 내용 |
|------|------|
| `76be827` | BACKLOG-73 Docker 로컬 빌드 스크립트 항목 SUPERSEDED 처리 (PR #48) |
| `d4e72c6` | auto-review v0.5.7.0 — 보안 수정(Gateway rate limit bypass, ResponseDispatcher 세션 검증 등) + 설계 부채 8건 백로그 등록 (PR #47) |
| `de9a353` | 코드 위생 확립 — clang-format + 경고 전수 소탕, `-Werror`/`/WX` (PR #46) |

PR #47에서 CI workflow에 `ci.yml` 변경(vcpkg 버전 업), CMake 버전 범프가 포함되어 있으므로 이번 CI 확장 작업 시 해당 변경 기준으로 진행.

## 8. 백로그 처리

| 백로그 | 상태 |
|--------|------|
| BACKLOG-7 Linux CI 확장 (E2E + UBSAN + Valgrind) | 이번에 해결 |
| BACKLOG-9 Windows apex_shared 빌드 미검증 | 이번에 해결 |
| BACKLOG-73 Docker 로컬 빌드 스크립트 | SUPERSEDED — 이번 Docker 기반 CI 통합으로 대체됨 (main에서 처리 완료) |
