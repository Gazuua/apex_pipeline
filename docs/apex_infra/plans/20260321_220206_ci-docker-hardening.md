# CI Docker Hardening — 설계 문서

**브랜치**: `feature/ci-docker-hardening`
**백로그**: BACKLOG-49, BACKLOG-122, BACKLOG-121
**스코프**: infra, ci

---

## 목표

CI/Docker 인프라의 재현성·보안·호환성 강화. 3개 백로그 항목을 하나의 번들로 처리.

| 항목 | 핵심 | 분류 |
|------|------|------|
| BACKLOG-49 | Docker 이미지 버전 감사 + pgbouncer 교체 | 재현성 |
| BACKLOG-122 | CI Docker `:latest` → immutable 태그 전환 | 재현성 + 캐시 |
| BACKLOG-121 | .dockerignore 서비스 빌드 호환 | 호환성 |

---

## 1. BACKLOG-49 — Docker 이미지 버전 감사 + pgbouncer 교체

### 현재 상태

| 이미지 | 위치 | 현재 태그 | 문제 |
|--------|------|-----------|------|
| `edoburu/pgbouncer` | dev compose | `1.23.1` | pull 실패 (비활성 maintainer) |
| `redis` | dev + e2e compose | `7-alpine` | patch 미고정 |
| `postgres` | dev + e2e compose | `16-alpine` | patch 미고정 |
| `apache/kafka` | dev + e2e compose | `4.2.0` | 고정 ✅ |
| `prom/prometheus` | dev compose | `v3.10.0` | 고정 ✅ |
| `grafana/grafana` | dev compose | `12.4.0` | 고정 ✅ |
| `ubuntu` | ci/서비스 Dockerfile | `24.04` | 고정 ✅ |

### 변경 사항

**pgbouncer 교체**: `edoburu/pgbouncer:1.23.1` → `bitnami/pgbouncer:<구현 시점 최신 안정 태그>`

- bitnami는 환경변수 기반 설정이므로 기존 `pgbouncer.ini` 마운트 방식에서 전환
- 기존 `pgbouncer.ini` 설정을 bitnami 환경변수로 매핑:
  - `pool_mode`, `default_pool_size`, `max_client_conn` 등 → 대응 env vars
  - bitnami가 미지원하는 설정(`query_timeout`, `client_idle_timeout` 등) → `PGBOUNCER_EXTRA_FLAGS` 또는 config override ini 마운트로 처리
- `userlist.txt`는 bitnami 자동 생성으로 별도 마운트 불필요
- 구현 시점에 Docker Hub에서 정확한 태그(예: `1.23.1-debian-12-rN`) 확인 후 고정

**redis/postgres patch 버전 고정**:

- `redis:7-alpine` → `redis:7.4.3-alpine`
- `postgres:16-alpine` → `postgres:16.9-alpine`
- dev compose + e2e compose 양쪽 동일 적용

### 변경 파일

- `apex_infra/docker-compose.yml`
- `apex_infra/docker/docker-compose.e2e.yml`
- `apex_infra/pgbouncer/pgbouncer.ini` (삭제 또는 참조용 보존)

---

## 2. BACKLOG-122 — CI Docker `:latest` → immutable 태그 전환

### 현재 흐름

```
docker-build.yml (ci.Dockerfile 변경 시 트리거)
  → push ghcr.io/gazuua/apex-pipeline-ci:latest
  → push ghcr.io/gazuua/apex-pipeline-ci:sha-<commit>

ci.yml / nightly.yml
  → container: ghcr.io/gazuua/apex-pipeline-ci:latest  ← 불확정
```

### 문제

- `:latest`는 mutable — 동일 태그가 다른 이미지를 가리킬 수 있음
- 빌드 재현 불가능: 과거 커밋 재빌드 시 다른 환경으로 실행
- 의도치 않은 전파: CI 이미지 변경이 모든 브랜치에 즉시 영향
- 캐시 비효율: 매번 원격 확인 필요, 변경 시 레이어 캐시 무효화

### 변경 사항

**태그 고정 파일 도입**: `.github/ci-image-tag`

- 단일 라인, 현재 유효한 sha 태그 기록
- ci.yml / nightly.yml이 이 파일에서 태그를 읽어 컨테이너 이미지로 사용

**워크플로우 변경 — ci.yml / nightly.yml**:

- 기존 `changes` job (ci.yml) 또는 신규 선행 job (nightly.yml)에서 `ci-image-tag` 파일을 읽는 step 추가
- 해당 step의 output을 후속 job에서 `needs.<job>.outputs.ci_image_tag`로 참조
- `container:` 필드에 `ghcr.io/gazuua/apex-pipeline-ci:${{ needs.<job>.outputs.ci_image_tag }}` 사용

**워크플로우 변경 — docker-build.yml**:

- 이미지 빌드+push 성공 후 `ci-image-tag` 파일을 새 sha 태그로 갱신하는 자동 커밋
- 커밋 메시지: `chore(ci): update ci-image-tag to sha-<hash> [skip ci]` — `[skip ci]`로 순환 트리거 방지
- **permissions 추가**: `contents: write` (현재 `contents: read`만 있음)
- ci.yml `build-image` job도 `contents: write`로 변경 필요 (reusable workflow는 caller 권한이 상한)

**서비스 Dockerfile builder stage 고정**:

```dockerfile
ARG CI_IMAGE_TAG
FROM ghcr.io/gazuua/apex-pipeline-ci:${CI_IMAGE_TAG} AS builder
```

- default 없이 필수 인자로 강제 — 태그 미전달 시 빌드 실패
- e2e compose: `args:` 필드에 `CI_IMAGE_TAG: ${CI_IMAGE_TAG}` 선언, 호출 시 환경변수로 주입 (`CI_IMAGE_TAG=$(cat .github/ci-image-tag) docker compose build`)

**`:latest` 태그**: push는 계속 (Docker build 레이어 캐시 소스 전용), 모든 사용처는 고정 태그로 통일.

### 변경 파일

- `.github/ci-image-tag` (신규)
- `.github/workflows/ci.yml`
- `.github/workflows/nightly.yml`
- `.github/workflows/docker-build.yml` (permissions 포함)
- `apex_infra/docker/gateway.Dockerfile`
- `apex_infra/docker/auth-svc.Dockerfile`
- `apex_infra/docker/chat-svc.Dockerfile`
- `apex_infra/docker/docker-compose.e2e.yml` (build args 추가)

---

## 3. BACKLOG-121 — .dockerignore 서비스 빌드 호환

### ci.Dockerfile non-root — 미적용 (설계 결정)

BACKLOG-121 원본은 ci.Dockerfile에 non-root 사용자 추가를 요구했으나, 다음 이유로 미적용:

1. **GitHub Actions 호환성**: container job에서 workspace 볼륨이 root 소유로 마운트되므로 non-root 유저가 쓰기 불가
2. **서비스 Dockerfile 호환성**: builder stage에서 `/src`, `/out` 디렉토리 생성 시 권한 문제
3. **업계 관행**: CI 빌드 이미지는 root 실행이 표준 — non-root는 런타임 이미지에서 적용
4. **이미 적용됨**: 서비스 Dockerfile의 runtime stage는 `apex` non-root 유저를 사용 중

→ BACKLOG-121의 non-root 항목은 WONTFIX 처리. `.dockerignore` 수정만 진행.

### .dockerignore whitelist 확장

**현재**:

```
*
!vcpkg.json
!apex_core/vcpkg.json
!apex_shared/vcpkg.json
```

`*`로 전부 차단, vcpkg.json만 허용 → 서비스 Dockerfile `COPY . /src`에서 소스 코드 복사 불가.

**변경**:

```
*
!vcpkg.json
!apex_core/
!apex_shared/
!apex_services/
!CMakeLists.txt
!cmake/
!build.bat
```

빌드에 필요한 소스 + 설정만 허용. 불필요한 것(.git, docs, node_modules 등)은 계속 차단.

> **참고**: `apex_services/tests/keys/` 디렉토리가 서비스 Dockerfile runtime stage에서 COPY됨. 이 디렉토리는 테스트 전용 키이며, 프로덕션 배포 시에는 별도 시크릿 주입 경로를 사용해야 함 (BACKLOG-5 스코프).

### 변경 파일

- `.dockerignore`
