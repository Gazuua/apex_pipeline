# 보안 시크릿 관리 + Blacklist 정책 설계

- **백로그**: #5, #6, #8, #100
- **브랜치**: `feature/security-secrets-hardening`
- **스코프**: infra, gateway, auth-svc, chat-svc
- **버전**: v0.5.10.0 시점

---

## 배경

현재 코드베이스에 다음 보안 이슈가 존재한다:

1. **#5** — gateway.toml Redis 비밀번호가 주석 처리됨. `expand_env()` 코드는 존재하나 Docker Compose에서 연결되지 않음
2. **#6** — SQL 마이그레이션에 `auth_secret_change_me` 평문 하드코딩
3. **#8** — Redis 4개 인스턴스 전부 무인증, PgBouncer `md5` + 평문 `userlist.txt`
4. **#100** — `gateway_pipeline.cpp`에서 Redis 장애 시 블랙리스트 체크를 fail-open으로 하드코딩

## 설계 원칙

- **개발 편의 최우선** — 로컬, CI, E2E, 벤치마킹 모두 별도 설정 없이 기존과 동일하게 동작
- **`${VAR:-dev_default}` 패턴** — 환경 변수 미설정 시 dev 기본값 자동 적용
- **Docker Compose 환경 중심** — K8s는 v0.6 운영 인프라 마일스톤에서 다룸
- **Redis `--requirepass`** — ACL은 v0.6에서 도입, 현 단계는 단일 비밀번호

---

## 섹션 1: 인프라 레이어

### 1-1. `.env.dev` 신규 생성 (VCS 커밋)

모든 시크릿의 dev 기본값을 한 곳에 모은다. 기존 `apex_infra/.env.example`은 삭제하고 이 파일로 대체한다.

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
```

Redis 환경 변수 이름은 docker-compose.yml의 서비스 이름과 1:1 대응한다:
- `REDIS_AUTH_PASSWORD` → redis-auth (포트 6380)
- `REDIS_RATELIMIT_PASSWORD` → redis-ratelimit (포트 6381)
- `REDIS_CHAT_PASSWORD` → redis-chat (포트 6382)
- `REDIS_PUBSUB_PASSWORD` → redis-pubsub (포트 6383)

### 1-2. `docker-compose.yml` 변경

- Redis 4개 인스턴스: `command`에 각 인스턴스별 `--requirepass` 추가
  - redis-auth: `--requirepass ${REDIS_AUTH_PASSWORD:-dev_redis_pass}`
  - redis-ratelimit: `--requirepass ${REDIS_RATELIMIT_PASSWORD:-dev_redis_pass}`
  - redis-chat: `--requirepass ${REDIS_CHAT_PASSWORD:-dev_redis_pass}`
  - redis-pubsub: `--requirepass ${REDIS_PUBSUB_PASSWORD:-dev_redis_pass}`
- PgBouncer: `userlist.txt`를 entrypoint 스크립트에서 환경 변수 기반 동적 생성으로 교체. `auth_type`은 현재 `md5` 유지 (SCRAM-SHA-256은 TLS와 함께 v0.6에서 전환)
- PostgreSQL: 기존 `${POSTGRES_PASSWORD:-apex}` 패턴 유지

### 1-3. `docker-compose.e2e.yml` 동일 패턴 적용

E2E도 같은 환경 변수 + dev 기본값 패턴으로 통일. 현재 하드코딩된 `POSTGRES_PASSWORD: apex_e2e_password` 등을 `${POSTGRES_PASSWORD:-apex_e2e_password}` 패턴으로 전환. E2E 고유 기본값은 E2E compose 파일 내 인라인 기본값으로 유지한다 (별도 `.env.e2e.dev` 파일 불필요).

### 1-4. `.env.prod.example` 신규 생성 (VCS 커밋)

값 없이 필수 변수 목록만 나열. 프로덕션 배포 시 참고용 템플릿.

### 1-5. PgBouncer entrypoint 스크립트

`userlist.txt`를 정적 파일 대신 컨테이너 시작 시 동적 생성한다. PgBouncer 이미지의 entrypoint를 래핑하는 스크립트를 작성:

```bash
#!/bin/bash
# md5 해시 생성: "md5" + md5(password + username)
HASH=$(echo -n "${PGBOUNCER_PASSWORD:-apex}apex" | md5sum | cut -d' ' -f1)
echo "\"apex\" \"md5${HASH}\"" > /etc/pgbouncer/userlist.txt
exec pgbouncer /etc/pgbouncer/pgbouncer.ini
```

기존 정적 `userlist.txt`(평문 비밀번호 포함)는 삭제한다.

---

## 섹션 2: 설정/TOML 레이어

### 2-1. `gateway.toml`

주석 처리된 Redis password 필드를 활성화하고 env var 패턴 적용. 기존 host/port는 변경하지 않는다:

```toml
[redis.pubsub]
host = "localhost"
port = 6383
password = "${REDIS_PUBSUB_PASSWORD:-dev_redis_pass}"

[redis.auth]
host = "localhost"
port = 6380
password = "${REDIS_AUTH_PASSWORD:-dev_redis_pass}"

[redis.ratelimit]
host = "localhost"
port = 6381
password = "${REDIS_RATELIMIT_PASSWORD:-dev_redis_pass}"
```

`gateway_config_parser.cpp`의 `expand_env()`가 이 문법을 이미 처리하므로 파서 코드 변경 불필요.

### 2-2. `auth_svc.toml`

PostgreSQL connection string + Redis password:

```toml
[redis]
host = "localhost"
port = 6380
password = "${REDIS_AUTH_PASSWORD:-dev_redis_pass}"

[pg]
connection_string = "host=localhost port=5432 dbname=apex_db user=apex_user password=${PG_SERVICE_PASSWORD:-apex_pass}"
```

### 2-3. `chat_svc.toml`

PostgreSQL connection string + Redis password (2개 섹션):

```toml
[redis_data]
host = "localhost"
port = 6382
password = "${REDIS_CHAT_PASSWORD:-dev_redis_pass}"

[redis_pubsub]
host = "localhost"
port = 6383
password = "${REDIS_PUBSUB_PASSWORD:-dev_redis_pass}"

[pg]
connection_string = "host=localhost port=5432 dbname=apex_db user=apex_user password=${PG_SERVICE_PASSWORD:-apex_pass}"
```

### 2-4. 서비스 config parser에 `expand_env()` 추가

현재 `expand_env()`는 `gateway_config_parser.cpp`의 익명 네임스페이스에만 존재한다. auth-svc와 chat-svc의 config parser에는 미구현이므로, 공통 유틸리티로 추출한다:

- `apex_shared/include/apex/shared/config_utils.hpp`에 `apex::shared::expand_env()` 선언
- `apex_shared/src/config_utils.cpp`에 구현 (gateway의 기존 구현 이동)
- gateway/auth-svc/chat-svc 파서에서 공통 유틸 사용

### 2-5. `gateway.toml` Blacklist 설정 추가

기존 `[auth]` 섹션 아래에 최상위 키로 추가. 현재 `[auth.exempt]`만 파싱하고 있으므로 `[auth]` 최상위 필드 읽기 로직을 파서에 추가해야 한다 (기존 `[auth.exempt]` 파싱과는 독립적).

```toml
[auth]
blacklist_fail_open = false
```

---

## 섹션 3: SQL 마이그레이션

### 3-1. `001_create_schema_and_role.sql` → 쉘 래퍼

SQL은 env var 치환을 지원하지 않으므로 Docker entrypoint 방식을 사용한다. PostgreSQL 공식 이미지의 `/docker-entrypoint-initdb.d/`에 `.sh` 파일을 넣으면 쉘 스크립트로 실행된다.

기존 `.sql`을 `.sh`로 감싸되, 비밀번호가 들어간 `CREATE ROLE` 부분만 쉘 래퍼로 감싸고 나머지 스키마/테이블 생성은 그대로 유지한다.

```bash
#!/bin/bash
psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" <<-EOSQL
  DO \$\$
  BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'auth_role') THEN
      CREATE ROLE auth_role WITH LOGIN PASSWORD '${POSTGRES_AUTH_ROLE_PASSWORD:-auth_secret_change_me}';
    END IF;
  END \$\$;
EOSQL
```

---

## 섹션 4: 코드 레이어 (Blacklist 정책)

### 4-1. `jwt_blacklist.cpp` 수정 — 사실 보고 역할로 단순화

현재 `jwt_blacklist.cpp`은 Redis 실패 시 `co_return true`(블랙리스트 처리)를 반환한다. 이는 `Result<bool>` 성공값이므로, `gateway_pipeline.cpp`의 `!bl_result` 분기(fail-open 코드)는 실질적으로 도달 불가능한 dead code이다.

**두 레이어의 책임을 정리한다:**
- `jwt_blacklist.cpp`: Redis 실패 시 **실패 Result를 반환** (사실만 보고, 정책 판단하지 않음)
- `gateway_pipeline.cpp`: 실패 Result를 받아 **설정에 따라 정책 결정**

변경:
```cpp
// 기존: co_return true; (성공 Result, 값 true)
// 변경: co_return std::unexpected(apex::core::ErrorCode::AdapterNotReady);
```

주석도 "availability priority" / "treat as blacklisted" 등 정책 판단 문구를 제거하고, "Redis 명령 실패를 호출자에게 전파"로 수정.

### 4-2. `GatewayConfig` 구조체

`AuthConfig`에 `blacklist_fail_open` 필드 추가 (기본값 `false`).

### 4-3. `gateway_config_parser.cpp`

`[auth]` 최상위 필드 읽기 로직 추가. 기존 `[auth.exempt]` 파싱과 별도로 `tbl["auth"]["blacklist_fail_open"]`을 읽는다.

### 4-4. `gateway_pipeline.cpp`

`jwt_blacklist.cpp`가 이제 실패 Result를 반환하므로 `!bl_result` 분기가 활성화된다. 설정 값에 따라 분기:

- `true` (fail-open): 경고 로그 + 요청 허용
- `false` (fail-close, 기본값): 경고 로그 + 요청 거부

```cpp
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
```

### 4-5. `GatewayError` 열거형

`BlacklistCheckFailed = 12` 추가 (현재 마지막 값: `SubscriptionLimitExceeded = 11`). 클라이언트에게는 일반 인증 실패로 응답.

---

## 스코프 외 (v0.6 DEFERRED)

- Redis ACL (역할 기반 접근 제어)
- PgBouncer SCRAM-SHA-256 (TLS와 함께 전환)
- K8s Secret / ConfigMap 매니페스트
- 프로덕션 기동 시 시크릿 검증 스크립트
- Docker Secrets 마운트 (`docker-compose.prod.yml`)
