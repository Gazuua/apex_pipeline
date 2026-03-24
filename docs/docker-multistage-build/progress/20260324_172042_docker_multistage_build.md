# v0.6.2 Docker 멀티스테이지 빌드 고도화 — 완료 기록

**일자**: 2026-03-24
**백로그**: BACKLOG-176
**브랜치**: feature/docker-multistage-build

## 작업 결과

### CI 이미지 2단계 분리
- `tools-base.Dockerfile`: gcc-14, cmake, sccache, vcpkg CLI (변경 빈도 월 1회 이하)
- `ci.Dockerfile`: tools-base 위에 vcpkg install만 수행 (vcpkg.json 변경 시만 재빌드)
- `.github/tools-image-tag` + `docker-images.yml`로 태그 자동 관리

### Docker Bake 서비스 통합
- 3개 개별 Dockerfile → 1개 `service.Dockerfile` + `docker-bake.hcl` 매트릭스
- `CMAKE_TARGET`, `SERVICE_DIR`, `CONFIG_FILE` ARG로 서비스 차이 흡수
- ENTRYPOINT: `ENV APP_BINARY` + shell form `exec`로 PID 1 시그널 처리 유지
- 서비스 추가 시 Bake 타겟 1블록만 추가

### sccache 컴파일 캐시
- `debug-sccache`, `release-sccache` CMake 프리셋 추가 (기존 프리셋 불변)
- Docker 빌드: `--mount=type=cache,target=/root/.cache/sccache`
- CI: `mozilla-actions/sccache-action` + `actions/cache`로 영속화
- 소스 1파일 변경 시 증분 빌드 예상: ~3분 → ~15초

### MSVC CI 빌드 도입
- `build-msvc` 잡: windows-latest, `release-sccache` 프리셋
- 초기 `continue-on-error: true` → 안정화 후 필수 전환
- `[skip-msvc]` 커밋 메시지로 문서 전용 PR 스킵

### 보안 강화
- RSA 키: 이미지에서 완전 제거, docker-compose volumes로 런타임 마운트 (BACKLOG-147)
- Trivy: 서비스 이미지 빌드 후 CRITICAL/HIGH 자동 차단

### CI 워크플로우 재편
- `docker-build.yml` → `docker-images.yml` 대체 (tools-base + ci 2단계)
- `ci.yml`: build-msvc + bake-services + scan 잡 추가

## 변경 파일

### 신규 (5)
- `apex_infra/docker/tools-base.Dockerfile`
- `apex_infra/docker/service.Dockerfile`
- `apex_infra/docker/docker-bake.hcl`
- `.github/workflows/docker-images.yml`
- `.github/tools-image-tag`

### 수정 (5)
- `apex_infra/docker/ci.Dockerfile`
- `.dockerignore`
- `CMakePresets.json`
- `.github/workflows/ci.yml`
- `apex_infra/docker/docker-compose.e2e.yml`
- `apex_infra/docker/docker-compose.valgrind.yml`

### 삭제 (4)
- `apex_infra/docker/gateway.Dockerfile`
- `apex_infra/docker/auth-svc.Dockerfile`
- `apex_infra/docker/chat-svc.Dockerfile`
- `.github/workflows/docker-build.yml`
