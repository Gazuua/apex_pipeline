# 보안 시크릿 관리 + Blacklist 정책 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Redis 인증, PgBouncer 동적 비밀번호, SQL 마이그레이션 시크릿 주입, blacklist fail-close 기본 정책을 구현하여 보안 시크릿 하드코딩을 제거한다.

**Architecture:** Bottom-Up 접근 — expand_env 공통 추출 → 인프라(.env, Docker Compose) → 설정(TOML) → 코드(blacklist 정책) 순서. 모든 환경(로컬/CI/E2E)은 `${VAR:-dev_default}` 패턴으로 무설정 동작 유지.

**Tech Stack:** C++23, Docker Compose, TOML (toml++), Bash, PostgreSQL, Redis, PgBouncer

**설계 문서:** `docs/apex_common/plans/20260321_220742_security-secrets-hardening-design.md`

---

## Task 1: `expand_env()` 공통 유틸리티 추출

**Files:**
- Create: `apex_shared/lib/include/apex/shared/config_utils.hpp`
- Create: `apex_shared/lib/src/config_utils.cpp`
- Modify: `apex_shared/CMakeLists.txt` — `placeholder.cpp` → `config_utils.cpp`로 교체
- Modify: `apex_services/gateway/src/gateway_config_parser.cpp:18-76` — 로컬 구현 제거, 공통 유틸 사용
- Create: `apex_shared/tests/unit/test_config_utils.cpp`
- Modify: `apex_shared/tests/unit/CMakeLists.txt` — 테스트 타겟 추가

### 의존성
- 없음 (첫 번째 태스크)

### Steps

- [ ] **Step 1: 테스트 작성**

`apex_shared/tests/unit/test_config_utils.cpp`:
```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#include <gtest/gtest.h>
#include <apex/shared/config_utils.hpp>
#include <cstdlib>

class ExpandEnvTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
#ifdef _MSC_VER
        _putenv_s("TEST_VAR", "hello");
#else
        setenv("TEST_VAR", "hello", 1);
#endif
    }

    void TearDown() override
    {
#ifdef _MSC_VER
        _putenv_s("TEST_VAR", "");
#else
        unsetenv("TEST_VAR");
#endif
    }
};

TEST_F(ExpandEnvTest, PlainStringUnchanged)
{
    EXPECT_EQ(apex::shared::expand_env("no_vars_here"), "no_vars_here");
}

TEST_F(ExpandEnvTest, ExpandsSetVariable)
{
    EXPECT_EQ(apex::shared::expand_env("${TEST_VAR}"), "hello");
}

TEST_F(ExpandEnvTest, ExpandsWithDefault_VarSet)
{
    EXPECT_EQ(apex::shared::expand_env("${TEST_VAR:-fallback}"), "hello");
}

TEST_F(ExpandEnvTest, ExpandsWithDefault_VarUnset)
{
    EXPECT_EQ(apex::shared::expand_env("${NONEXISTENT_VAR:-fallback}"), "fallback");
}

TEST_F(ExpandEnvTest, UnsetVarNoDefault_KeepsOriginal)
{
    EXPECT_EQ(apex::shared::expand_env("${NONEXISTENT_VAR}"), "${NONEXISTENT_VAR}");
}

TEST_F(ExpandEnvTest, MultipleVarsInString)
{
    EXPECT_EQ(apex::shared::expand_env("host=${TEST_VAR} port=${NONEXISTENT:-8080}"),
              "host=hello port=8080");
}

TEST_F(ExpandEnvTest, EmptyInput)
{
    EXPECT_EQ(apex::shared::expand_env(""), "");
}
```

- [ ] **Step 2: 헤더 작성**

`apex_shared/lib/include/apex/shared/config_utils.hpp`:
```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#pragma once

#include <string>
#include <string_view>

namespace apex::shared
{

/// 문자열 내 ${VAR} 및 ${VAR:-default} 패턴을 환경 변수 값으로 치환한다.
/// - ${VAR}: 환경 변수가 설정되어 있으면 값으로 치환, 없으면 원본 유지.
/// - ${VAR:-default}: 환경 변수가 설정되어 있으면 값으로 치환, 없으면 default로 치환.
std::string expand_env(std::string_view value);

} // namespace apex::shared
```

- [ ] **Step 3: 구현 작성**

`apex_shared/lib/src/config_utils.cpp`:
gateway의 기존 `expand_env()` 구현(lines 22-76)을 그대로 이동. 네임스페이스만 `apex::shared`로 변경. MSVC `#pragma warning(disable : 4996)` 경고 억제(std::getenv) 포함.

- [ ] **Step 4: CMake 업데이트**

`apex_shared/CMakeLists.txt`:
- `lib/src/placeholder.cpp` → `lib/src/config_utils.cpp`로 교체
- `apex_shared/tests/unit/CMakeLists.txt`에 `test_config_utils` 타겟 추가:
  ```cmake
  apex_shared_add_unit_test(test_config_utils test_config_utils.cpp)
  ```
  `apex_shared_add_unit_test` 함수가 `apex::shared::adapters::common`을 링크하며, 이것이 `apex::shared`를 전이 포함하므로 별도 링크 불필요.

- [ ] **Step 5: gateway_config_parser.cpp 수정**

`apex_services/gateway/src/gateway_config_parser.cpp`:
- Lines 1-17: `#include <apex/shared/config_utils.hpp>` 추가
- Lines 18-76: 익명 네임스페이스의 `expand_env()` 전체 삭제
- 모든 `expand_env()` 호출을 `apex::shared::expand_env()`로 교체

- [ ] **Step 6: 빌드 검증**

```bash
"<root>/apex_tools/queue-lock.sh" build debug
```

- [ ] **Step 7: 커밋**

```bash
git add apex_shared/lib/include/apex/shared/config_utils.hpp \
        apex_shared/lib/src/config_utils.cpp \
        apex_shared/CMakeLists.txt \
        apex_shared/tests/unit/test_config_utils.cpp \
        apex_shared/tests/unit/CMakeLists.txt \
        apex_services/gateway/src/gateway_config_parser.cpp
git commit -m "refactor(shared): expand_env() 공통 유틸리티 추출 BACKLOG-5"
```

---

## Task 2: 환경 변수 파일 정리

**Files:**
- Create: `apex_infra/.env.dev`
- Create: `apex_infra/.env.prod.example`
- Delete: `apex_infra/.env.example`

### 의존성
- 없음 (독립)

### Steps

- [ ] **Step 1: `.env.dev` 생성**

`apex_infra/.env.dev`:
```env
# Redis (인스턴스별)
REDIS_AUTH_PASSWORD=dev_redis_pass
REDIS_RATELIMIT_PASSWORD=dev_redis_pass
REDIS_CHAT_PASSWORD=dev_redis_pass
REDIS_PUBSUB_PASSWORD=dev_redis_pass

# PostgreSQL
POSTGRES_PASSWORD=apex
POSTGRES_AUTH_ROLE_PASSWORD=auth_secret_change_me

# PgBouncer
PGBOUNCER_PASSWORD=apex

# PostgreSQL (서비스 역할)
PG_SERVICE_PASSWORD=apex_pass

# Grafana
GF_SECURITY_ADMIN_PASSWORD=admin
```

- [ ] **Step 2: `.env.prod.example` 생성**

`apex_infra/.env.prod.example`:
```env
# === 프로덕션 시크릿 템플릿 ===
# 이 파일을 .env로 복사하여 실제 값을 채워 사용한다.
# .env 파일은 .gitignore에 의해 VCS에 포함되지 않는다.

# Redis (인스턴스별 — 강력한 비밀번호 사용)
REDIS_AUTH_PASSWORD=
REDIS_RATELIMIT_PASSWORD=
REDIS_CHAT_PASSWORD=
REDIS_PUBSUB_PASSWORD=

# PostgreSQL (superuser)
POSTGRES_PASSWORD=

# PostgreSQL (auth_role — SQL 마이그레이션에서 사용)
POSTGRES_AUTH_ROLE_PASSWORD=

# PgBouncer
PGBOUNCER_PASSWORD=

# PostgreSQL (서비스 역할 — auth_svc, chat_svc connection_string에서 사용)
PG_SERVICE_PASSWORD=

# Grafana
GF_SECURITY_ADMIN_PASSWORD=
```

- [ ] **Step 3: `.env.example` 삭제 + `.gitignore` 확인**

```bash
git rm apex_infra/.env.example
```

`apex_infra/.gitignore` 파일이 현재 존재하지 않으므로 신규 생성:
```
.env
!.env.dev
!.env.prod.example
```

- [ ] **Step 4: 커밋**

```bash
git add apex_infra/.env.dev apex_infra/.env.prod.example apex_infra/.gitignore
git commit -m "chore(infra): .env.dev + .env.prod.example 시크릿 파일 정리 BACKLOG-5"
```

---

## Task 3: Docker Compose — Redis `--requirepass` 추가

**Files:**
- Modify: `apex_infra/docker-compose.yml:39-115` — Redis 4개 인스턴스 command에 `--requirepass` 추가

### 의존성
- Task 2 (`.env.dev` 존재)

### Steps

**중요**: Docker Compose `command`의 `${VAR:-default}` 치환은 **호스트 환경** 또는 Compose 프로젝트 루트의 `.env` 파일에서 값을 읽는다. `env_file`은 컨테이너 내부 ENV에만 적용되므로 `command` 치환에 영향 없다. 따라서 `.env.dev`를 Compose가 자동으로 읽도록 심볼릭 링크 또는 `--env-file` 플래그를 사용하거나, `command`의 기본값(`:-dev_redis_pass`)에 의존한다.

**선택 방법**: `command`에 `${VAR:-dev_redis_pass}` 패턴을 사용하면 호스트에 환경 변수가 없어도 기본값 `dev_redis_pass`로 동작하므로, `.env` 파일 연결 없이도 로컬/CI에서 정상 동작한다. 프로덕션에서는 호스트 환경 변수 또는 `docker compose --env-file .env` 으로 주입한다.

- [ ] **Step 1: redis-auth (lines 39-54)**

`command`에 `--requirepass ${REDIS_AUTH_PASSWORD:-dev_redis_pass}` 추가.

- [ ] **Step 2: redis-ratelimit (lines 57-77)**

동일 패턴. `--requirepass ${REDIS_RATELIMIT_PASSWORD:-dev_redis_pass}`.

- [ ] **Step 3: redis-chat (lines 80-95)**

동일 패턴. `--requirepass ${REDIS_CHAT_PASSWORD:-dev_redis_pass}`.

- [ ] **Step 4: redis-pubsub (lines 98-115)**

동일 패턴. `--requirepass ${REDIS_PUBSUB_PASSWORD:-dev_redis_pass}`.

- [ ] **Step 5: docker-compose config 검증**

```bash
cd apex_infra && docker compose config --quiet
```

에러 없이 통과해야 함.

- [ ] **Step 6: 커밋**

```bash
git add apex_infra/docker-compose.yml
git commit -m "fix(infra): Redis 4인스턴스 --requirepass 인증 추가 BACKLOG-8"
```

---

## Task 4: PgBouncer 동적 userlist 생성

**Files:**
- Create: `apex_infra/pgbouncer/entrypoint.sh`
- Modify: `apex_infra/pgbouncer/pgbouncer.ini:7-8` — auth_type 유지 (md5)
- Delete: `apex_infra/pgbouncer/userlist.txt` — 정적 평문 파일 삭제
- Modify: `apex_infra/docker-compose.yml` — PgBouncer 서비스에 entrypoint + env 추가

### 의존성
- Task 2 (`.env.dev` 존재)

### Steps

- [ ] **Step 1: entrypoint.sh 작성**

`apex_infra/pgbouncer/entrypoint.sh`:
```bash
#!/bin/bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
set -euo pipefail

# md5 해시 생성: "md5" + md5(password + username)
PGB_PASS="${PGBOUNCER_PASSWORD:-apex}"
PGB_USER="${PGBOUNCER_USER:-apex}"
HASH=$(echo -n "${PGB_PASS}${PGB_USER}" | md5sum | cut -d' ' -f1)
echo "\"${PGB_USER}\" \"md5${HASH}\"" > /etc/pgbouncer/userlist.txt

exec pgbouncer /etc/pgbouncer/pgbouncer.ini
```

- [ ] **Step 2: 정적 userlist.txt 삭제**

```bash
git rm apex_infra/pgbouncer/userlist.txt
```

- [ ] **Step 3: docker-compose.yml PgBouncer 서비스 수정**

PgBouncer 서비스에:
- `entrypoint: ["/entrypoint.sh"]` 추가
- `volumes`에 `./pgbouncer/entrypoint.sh:/entrypoint.sh:ro` 추가
- `environment`에 `PGBOUNCER_PASSWORD: ${PGBOUNCER_PASSWORD:-apex}` 추가
- `env_file: .env.dev` 추가

- [ ] **Step 4: 커밋**

```bash
git add apex_infra/pgbouncer/entrypoint.sh apex_infra/docker-compose.yml
git commit -m "fix(infra): PgBouncer 동적 userlist 생성으로 평문 비밀번호 제거 BACKLOG-8"
```

---

## Task 5: SQL 마이그레이션 쉘 래퍼

**Files:**
- Create: `apex_services/auth-svc/migrations/001_create_schema_and_role.sh`
- Modify: `apex_infra/docker-compose.yml` — PostgreSQL volumes에 `.sh` 마운트 경로 반영
- Modify: `apex_infra/docker/docker-compose.e2e.yml` — 동일 반영

### 의존성
- 없음 (독립)

### Steps

- [ ] **Step 1: 쉘 래퍼 작성**

`apex_services/auth-svc/migrations/001_create_schema_and_role.sh`:
기존 `001_create_schema_and_role.sql` (30줄)의 전체 내용을 heredoc으로 감싼다. `CREATE ROLE ... PASSWORD` 부분만 `${POSTGRES_AUTH_ROLE_PASSWORD:-auth_secret_change_me}`로 치환.

- [ ] **Step 2: docker-compose.yml PostgreSQL volumes 확인**

현재 migration 파일이 어떻게 마운트되는지 확인. `/docker-entrypoint-initdb.d/`에 `.sh`가 `.sql`보다 먼저 실행되므로 파일명 순서 주의 — 기존 `.sql`은 유지하되 `CREATE ROLE` 부분은 쉘 래퍼에서 처리하도록 분리하거나, `.sql`에서 `CREATE ROLE`을 제거하고 `.sh`에 통합.

- [ ] **Step 3: 기존 SQL 파일 수정**

`001_create_schema_and_role.sql`:
- `CREATE ROLE ... PASSWORD 'auth_secret_change_me'` 블록(lines 9-14) 제거
- 스키마 생성 + GRANT 부분만 유지

- [ ] **Step 4: 커밋**

```bash
git add apex_services/auth-svc/migrations/001_create_schema_and_role.sh \
        apex_services/auth-svc/migrations/001_create_schema_and_role.sql \
        apex_infra/docker-compose.yml
git commit -m "fix(auth-svc): SQL 마이그레이션 비밀번호 하드코딩 제거 BACKLOG-6"
```

---

## Task 6: E2E Docker Compose 시크릿 패턴 적용

**Files:**
- Modify: `apex_infra/docker/docker-compose.e2e.yml` — Redis `--requirepass` + PostgreSQL env var 패턴

### 의존성
- Task 3 (dev compose 패턴 참조)

### Steps

- [ ] **Step 1: E2E Redis 4개 인스턴스에 `--requirepass` 추가**

dev compose와 동일 패턴. E2E 기본값도 `dev_redis_pass` 사용 (통일).

- [ ] **Step 2: E2E PostgreSQL 하드코딩 비밀번호 패턴 전환**

```yaml
POSTGRES_PASSWORD: ${POSTGRES_PASSWORD:-apex_e2e_password}
```

- [ ] **Step 3: E2E TOML 파일에 Redis password 필드 추가**

`apex_services/tests/e2e/gateway_e2e.toml` — Redis 3개 섹션에 password 필드 추가
`apex_services/tests/e2e/auth_svc_e2e.toml` — Redis 섹션에 password 필드 추가
`apex_services/tests/e2e/chat_svc_e2e.toml` — Redis 2개 섹션에 password 필드 추가

모든 E2E TOML의 Redis password: `dev_redis_pass` (평문 기본값, E2E는 expand_env 없이 직접 값 사용)

- [ ] **Step 4: E2E PostgreSQL connection_string 패턴 전환**

E2E TOML의 `[pg]` connection_string — 기존 `password=apex_e2e_password` 유지 (E2E는 고정값).

- [ ] **Step 5: 커밋**

```bash
git add apex_infra/docker/docker-compose.e2e.yml \
        apex_services/tests/e2e/gateway_e2e.toml \
        apex_services/tests/e2e/auth_svc_e2e.toml \
        apex_services/tests/e2e/chat_svc_e2e.toml
git commit -m "fix(infra): E2E Docker Compose 시크릿 패턴 통일 BACKLOG-5,8"
```

---

## Task 7: TOML 설정 — Redis 비밀번호 + Blacklist 옵션

**Files:**
- Modify: `apex_services/gateway/gateway.toml:45-58` — Redis password 활성화
- Modify: `apex_services/gateway/gateway.toml` — `[auth]` 섹션에 `blacklist_fail_open` 추가
- Modify: `apex_services/auth-svc/auth_svc.toml` — Redis password 추가
- Modify: `apex_services/chat-svc/chat_svc.toml` — Redis password 2개 섹션 추가
- Modify: `apex_services/auth-svc/auth_svc.toml` — `[pg]` connection_string env var 적용
- Modify: `apex_services/chat-svc/chat_svc.toml` — `[pg]` connection_string env var 적용

### 의존성
- 없음 (TOML 파일 수정만)

### Steps

- [ ] **Step 1: gateway.toml Redis password 활성화**

3개 Redis 섹션(pubsub, auth, ratelimit)의 `# password = ""` 주석을 해제하고 env var 패턴 적용:
```toml
password = "${REDIS_PUBSUB_PASSWORD:-dev_redis_pass}"  # pubsub (6383)
password = "${REDIS_AUTH_PASSWORD:-dev_redis_pass}"      # auth (6380)
password = "${REDIS_RATELIMIT_PASSWORD:-dev_redis_pass}" # ratelimit (6381)
```

- [ ] **Step 2: gateway.toml `[auth]` 섹션에 blacklist_fail_open 추가**

기존 `[auth.exempt]` 위에:
```toml
[auth]
blacklist_fail_open = false
```

- [ ] **Step 3: auth_svc.toml Redis password + PG env var**

```toml
[redis]
password = "${REDIS_AUTH_PASSWORD:-dev_redis_pass}"

[pg]
connection_string = "host=localhost port=5432 dbname=apex_db user=apex_user password=${PG_SERVICE_PASSWORD:-apex_pass} ..."
```

- [ ] **Step 4: chat_svc.toml Redis password 2개 + PG env var**

```toml
[redis_data]
password = "${REDIS_CHAT_PASSWORD:-dev_redis_pass}"

[redis_pubsub]
password = "${REDIS_PUBSUB_PASSWORD:-dev_redis_pass}"

[pg]
connection_string = "host=localhost port=5432 dbname=apex_db user=apex_user password=${PG_SERVICE_PASSWORD:-apex_pass} ..."
```

- [ ] **Step 5: 커밋**

```bash
git add apex_services/gateway/gateway.toml \
        apex_services/auth-svc/auth_svc.toml \
        apex_services/chat-svc/chat_svc.toml
git commit -m "fix(gateway,auth-svc,chat-svc): TOML 시크릿 env var 패턴 적용 BACKLOG-5"
```

---

## Task 8: 서비스 config parser에 `expand_env()` 적용

**Files:**
- Modify: `apex_services/auth-svc/src/main.cpp:35-93` — `expand_env()` 호출 추가
- Modify: `apex_services/chat-svc/src/main.cpp:55-108` — `expand_env()` 호출 추가
- Modify: `apex_services/auth-svc/CMakeLists.txt` — apex_shared 의존성 확인
- Modify: `apex_services/chat-svc/CMakeLists.txt` — apex_shared 의존성 확인

### 의존성
- Task 1 (expand_env 공통 유틸 존재)

### Steps

- [ ] **Step 1: auth-svc main.cpp에 expand_env 적용**

`#include <apex/shared/config_utils.hpp>` 추가.
`parse_config()` 내에서 password/connection_string 필드에 `apex::shared::expand_env()` 적용:
- Redis password (line ~52)
- PG connection_string (line ~59)

- [ ] **Step 2: chat-svc main.cpp에 expand_env 적용**

동일 패턴:
- redis_data password (line ~75)
- redis_pubsub password (line ~83)
- PG connection_string (line ~91)

- [ ] **Step 3: CMake 의존성 확인**

auth-svc와 chat-svc가 이미 `apex_shared`를 링크하는지 확인. 안 하면 `target_link_libraries`에 추가.

- [ ] **Step 4: 빌드 검증**

```bash
"<root>/apex_tools/queue-lock.sh" build debug
```

- [ ] **Step 5: 커밋**

```bash
git add apex_services/auth-svc/src/main.cpp \
        apex_services/chat-svc/src/main.cpp
git commit -m "feat(auth-svc,chat-svc): config parser에 expand_env() 시크릿 주입 적용 BACKLOG-5"
```

---

## Task 9: GatewayError + GatewayConfig 변경

**Files:**
- Modify: `apex_services/gateway/include/apex/gateway/gateway_error.hpp:25` — `BlacklistCheckFailed = 12` 추가
- Modify: `apex_services/gateway/include/apex/gateway/gateway_config.hpp:68-71` — `AuthConfig`에 `blacklist_fail_open` 추가

### 의존성
- 없음 (독립)

### Steps

- [ ] **Step 1: GatewayError 열거형에 BlacklistCheckFailed 추가**

`gateway_error.hpp` line 25 이후:
```cpp
BlacklistCheckFailed = 12,
```

`gateway_error_name()` constexpr 함수에 case 추가:
```cpp
case GatewayError::BlacklistCheckFailed: return "BlacklistCheckFailed";
```

- [ ] **Step 2: AuthConfig에 blacklist_fail_open 필드 추가**

`gateway_config.hpp` AuthConfig struct:
```cpp
struct AuthConfig
{
    std::unordered_set<uint32_t> auth_exempt_msg_ids;
    bool blacklist_fail_open = false;
};
```

- [ ] **Step 3: 커밋**

```bash
git add apex_services/gateway/include/apex/gateway/gateway_error.hpp \
        apex_services/gateway/include/apex/gateway/gateway_config.hpp
git commit -m "feat(gateway): BlacklistCheckFailed 에러 + blacklist_fail_open 설정 필드 추가 BACKLOG-100"
```

---

## Task 10: gateway_config_parser — blacklist_fail_open 읽기

**Files:**
- Modify: `apex_services/gateway/src/gateway_config_parser.cpp:198-208` — `[auth]` 최상위 필드 파싱 추가

### 의존성
- Task 9 (GatewayConfig 변경)

### Steps

- [ ] **Step 1: [auth] 최상위 필드 파싱 추가**

기존 `[auth.exempt]` 파싱 코드 앞에:
```cpp
// [auth] — blacklist fail-open 정책
cfg.auth.blacklist_fail_open = tbl["auth"]["blacklist_fail_open"].value_or(false);
```

- [ ] **Step 2: 커밋**

```bash
git add apex_services/gateway/src/gateway_config_parser.cpp
git commit -m "feat(gateway): gateway_config_parser blacklist_fail_open 파싱 추가 BACKLOG-100"
```

---

## Task 11: jwt_blacklist.cpp — 사실 보고 역할로 변경

**Files:**
- Modify: `apex_services/gateway/src/jwt_blacklist.cpp:45-51` — Redis 실패 시 failed Result 반환

### 의존성
- 없음 (독립)

### Steps

- [ ] **Step 1: Redis 실패 시 failed Result 반환으로 변경**

기존 (lines 45-51):
```cpp
if (!result)
{
    // Redis failure: fail-open (availability priority).
    // Conservative: treat as blacklisted for sensitive path.
    spdlog::warn("JWT blacklist check failed for jti={}, assuming blacklisted", jti);
    co_return true;
}
```

변경:
```cpp
if (!result)
{
    spdlog::warn("JWT blacklist Redis check failed for jti={}", jti);
    co_return std::unexpected(apex::core::ErrorCode::AdapterNotReady);
}
```

`ErrorCode::AdapterNotReady`를 사용하여 Redis 어댑터 장애임을 명시. `result.error()`를 직접 전달하지 않는 이유: Redis 커맨드 에러 타입과 `Result<bool>`의 에러 타입 호환성 검증이 필요하므로, 이미 정의된 ErrorCode를 사용하는 것이 안전하다.

jwt_blacklist은 사실만 보고하고, 정책 판단(fail-open/fail-close)은 호출자(gateway_pipeline)가 결정한다.

- [ ] **Step 2: 커밋**

```bash
git add apex_services/gateway/src/jwt_blacklist.cpp
git commit -m "refactor(gateway): jwt_blacklist Redis 실패 시 사실 보고로 변경 BACKLOG-100"
```

---

## Task 12: gateway_pipeline.cpp — 설정 기반 blacklist 정책

**Files:**
- Modify: `apex_services/gateway/src/gateway_pipeline.cpp:107-122` — 설정 기반 분기

### 의존성
- Task 9 (GatewayError::BlacklistCheckFailed)
- Task 10 (config 파싱)
- Task 11 (jwt_blacklist 변경)

### Steps

- [ ] **Step 1: blacklist 체크 분기 수정**

기존 (lines 107-122):
```cpp
if (blacklist_ && jwt_verifier_.is_sensitive(header.msg_id))
{
    auto bl_result = co_await blacklist_->is_blacklisted(claims_result->jti);
    if (bl_result && *bl_result)
    {
        spdlog::info("authenticate: blacklisted JWT jti={} for msg_id={}", claims_result->jti, header.msg_id);
        co_return GatewayResult{std::unexpected(GatewayError::JwtBlacklisted)};
    }
    // Redis error on blacklist check — fail-open for resilience
    if (!bl_result)
    {
        spdlog::warn("authenticate: blacklist check failed (Redis error), allowing jti={}", claims_result->jti);
    }
}
```

변경:
```cpp
if (blacklist_ && jwt_verifier_.is_sensitive(header.msg_id))
{
    auto bl_result = co_await blacklist_->is_blacklisted(claims_result->jti);
    if (bl_result && *bl_result)
    {
        spdlog::info("authenticate: blacklisted JWT jti={} for msg_id={}", claims_result->jti, header.msg_id);
        co_return GatewayResult{std::unexpected(GatewayError::JwtBlacklisted)};
    }
    if (!bl_result)
    {
        if (config_.auth.blacklist_fail_open)
        {
            spdlog::warn("authenticate: blacklist check failed (Redis error), allowing jti={}", claims_result->jti);
        }
        else
        {
            spdlog::warn("authenticate: blacklist check failed (Redis error), rejecting jti={}", claims_result->jti);
            co_return GatewayResult{std::unexpected(GatewayError::BlacklistCheckFailed)};
        }
    }
}
```

- [ ] **Step 2: 커밋**

```bash
git add apex_services/gateway/src/gateway_pipeline.cpp
git commit -m "feat(gateway): blacklist Redis 장애 시 설정 기반 fail-open/fail-close 정책 BACKLOG-100"
```

---

## Task 13: 빌드 + 포맷팅 + 테스트 검증

**Files:**
- 전체 코드베이스

### 의존성
- 모든 이전 태스크

### Steps

- [ ] **Step 1: clang-format**

```bash
find apex_core apex_shared apex_services \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) ! -name '*_generated.h' | xargs clang-format -i
```

- [ ] **Step 2: 빌드**

```bash
"<root>/apex_tools/queue-lock.sh" build debug
```

빌드 성공 + 테스트 통과 확인.

- [ ] **Step 3: 포맷팅 diff 커밋 (있는 경우)**

변경된 파일을 명시적으로 스테이징:
```bash
git add <변경된 .cpp/.hpp 파일들>
git commit -m "style: clang-format 적용"
```

---

## 실행 순서 요약

```
Task 1 (expand_env 추출)  ──→  Task 8 (서비스 parser 적용)
                                    ↓
Task 2 (env 파일) ──→ Task 3 (Redis requirepass)
                  ──→ Task 4 (PgBouncer entrypoint)
                                    ↓
Task 5 (SQL migration)              Task 6 (E2E compose)
Task 7 (TOML 설정)
                                    ↓
Task 9 (Error+Config)  ──→ Task 10 (parser) ──→ Task 12 (pipeline)
Task 11 (jwt_blacklist)  ──────────────────────→
                                    ↓
                            Task 13 (빌드+포맷+테스트)
```

독립 태스크(Task 2~7, 9, 11)는 병렬 실행 가능.
