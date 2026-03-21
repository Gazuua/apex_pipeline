# CI Docker Hardening — 작업 결과

**브랜치**: `feature/ci-docker-hardening`
**PR**: #85
**백로그**: BACKLOG-49, BACKLOG-122, BACKLOG-121

---

## 완료 사항

### BACKLOG-49: Docker 이미지 버전 감사 + pgbouncer 교체

- `edoburu/pgbouncer:1.23.1` → `bitnami/pgbouncer:1.23.1` (환경변수 기반 설정 전환)
- `redis:7-alpine` → `redis:7.4.2-alpine` (dev + e2e compose)
- `postgres:16-alpine` → `postgres:16.8-alpine` (dev + e2e compose)
- pgbouncer.ini / userlist.txt 삭제 (bitnami가 env vars에서 자동 생성)
- bitnami pgbouncer 환경변수 매핑: pool_mode, default_pool_size, min_pool_size, max_client_conn, max_db_connections, query_wait_timeout, server_idle_timeout, client_idle_timeout

### BACKLOG-122: CI Docker :latest → immutable 태그 전환

- `.github/ci-image-tag` 파일 도입 (sha 태그 고정)
- `ci.yml` changes job에서 태그 읽기 → build job container 참조
- `nightly.yml` read-tag 선행 job 추가 → valgrind-unit/build job 참조
- `docker-build.yml` 빌드 후 ci-image-tag 자동 갱신 (main push 시만, `[skip ci]`)
- 서비스 Dockerfile 3개: `ARG CI_IMAGE_TAG` 필수화 (default 없음)
- e2e compose: build args로 `CI_IMAGE_TAG` 주입 (`${CI_IMAGE_TAG:?}`)
- `:latest` 태그는 캐시 소스로만 push 유지, 사용처 전무

### BACKLOG-121: .dockerignore 서비스 빌드 호환

- whitelist에 `apex_core/`, `apex_shared/`, `apex_services/`, `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `build.bat` 추가
- 서비스 Dockerfile `COPY . /src` 정상 동작 보장
- non-root ci.Dockerfile은 WONTFIX — CI 빌드 이미지 root 실행이 업계 표준, GitHub Actions workspace 마운트 호환성 문제

## Auto-Review 결과

- CRITICAL 1건 수정 (CMakePresets.json .dockerignore 누락)
- MAJOR 2건 수정 (docker-build PR push 실패, pgbouncer 타임아웃 누락)
- MINOR 2건 수정 (pgbouncer 버전, 고아 파일 삭제)

## 변경 통계

- 변경 파일: 13개 (삭제 2개 포함)
- 커밋: 2개 (구현 + 리뷰 수정)
