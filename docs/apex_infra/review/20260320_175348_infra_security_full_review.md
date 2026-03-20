# Infra/Security Full Review (v0.5.9.0)

**날짜**: 2026-03-20
**범위**: 전체 코드베이스 (CMake, vcpkg, CI, Docker, 크로스컴파일러, 입력검증, 보안)
**브랜치**: chore/auto-review-full

## 통계

- 검토 파일: 60+
- 발견 이슈: MAJOR 2건, MINOR 5건 (CRITICAL 0건)
- 직접 수정: 2건
- BACKLOG 등록 필요: 3건 (담당 밖)
- 보고만: 2건

---

## 발견 이슈

### [MAJOR] PasswordHasher::verify()에서 bcrypt 해시값 로그 노출

- **파일**: `apex_services/auth-svc/src/password_hasher.cpp:46-49`
- **내용**: verify 실패 시 디버그 목적의 코드가 프로덕션에 잔류. stored hash와 computed hash를 `spdlog::error`로 출력하고 있었음. bcrypt 해시는 salt를 포함하므로 로그가 유출되면 오프라인 크래킹 공격에 활용 가능. 또한 verify 실패 시 불필요하게 `apex_bcrypt_hashpw`를 한 번 더 호출하여 CPU를 소모 (bcrypt cost=12 기준 ~250ms).
- **조치**: 직접 수정함 -- 디버그 코드 전체 제거. verify 실패는 caller(AuthService)가 이미 적절히 로깅하므로 PasswordHasher에서 추가 로깅 불필요.

### [MAJOR] 서비스 Dockerfile에서 non-root 실행 누락

- **파일**: `apex_infra/docker/gateway.Dockerfile`, `auth-svc.Dockerfile`, `chat-svc.Dockerfile`
- **내용**: runtime 스테이지에서 root 사용자로 서비스 프로세스를 실행. 컨테이너 탈출 취약점 발생 시 호스트 루트 권한을 획득할 위험이 있고, Docker 보안 best practice 위반.
- **조치**: 직접 수정함 -- 3개 Dockerfile 모두 `apex` 시스템 유저를 생성하고 `USER apex`로 전환. Valgrind 스테이지는 apt 설치를 위해 임시 `USER root` 후 다시 `USER apex`로 복귀.

### [MINOR] Redis 인스턴스 AUTH 미설정 (로컬 dev + E2E)

- **파일**: `apex_infra/docker-compose.yml`, `apex_infra/docker/docker-compose.e2e.yml`
- **내용**: Redis 4개 인스턴스 모두 `--requirepass` 없이 실행. Docker 네트워크 내부이므로 로컬 개발에서는 수용 가능하지만, docker-compose 포트가 호스트에 바인딩되어 있어(6380-6383) 같은 머신의 다른 프로세스가 접근 가능. 프로덕션 배포 시 반드시 AUTH 설정 필요.
- **조치**: 보고만 -- 현재 로컬 개발 전용이므로 수용 가능. 프로덕션 배포 시 반드시 설정 필요 (BACKLOG-100 등으로 추적 권장).

### [MINOR] E2E 설정에 PostgreSQL 패스워드 하드코딩

- **파일**: `apex_services/tests/e2e/auth_svc_e2e.toml:26`, `chat_svc_e2e.toml:31`
- **내용**: `password=apex_e2e_password`가 TOML 파일에 평문으로 기록. E2E 전용이라 실질적 위험은 낮으나, Gateway 설정 파서(`gateway_config_parser.cpp`)는 이미 `${VAR:-default}` 환경변수 치환을 지원하고 있어 패턴 불일치.
- **조치**: 보고만 -- E2E 전용 설정이고 해당 패스워드는 docker-compose.e2e.yml에도 동일하게 기재. 향후 서비스 설정 파서에 환경변수 치환 일원화 시 함께 처리.

### [MINOR] vcpkg.json version-semver 불일치

- **파일**: `vcpkg.json:4`
- **내용**: `"version-semver": "0.5.7"`이지만 루트 CMakeLists.txt의 `VERSION 0.5.8`. 빌드에 실질적 영향은 없으나 버전 추적이 혼란될 수 있음.
- **조치**: 보고만 -- 담당 밖 (비빌드 메타데이터). 다음 버전 업데이트 시 자연스럽게 해소.

### [MINOR] apex_core 독립 빌드 시 VERSION 불일치

- **파일**: `apex_core/CMakeLists.txt:8`
- **내용**: `VERSION 0.5.7`이 루트 `VERSION 0.5.8`과 불일치. standalone 빌드 시에만 적용되므로 CI에는 영향 없으나 혼란 가능.
- **조치**: 보고만 -- 다음 버전 업데이트 시 함께 갱신.

### [MINOR] CI 이미지 latest 태그 의존

- **파일**: `.github/workflows/ci.yml:126`
- **내용**: `ghcr.io/gazuua/apex-pipeline-ci:latest` 사용. Docker 이미지가 변경되면 캐시 없이 새 이미지를 풀링하여 CI가 재현 불가능해질 수 있음. 다만 `build-image` job이 Docker 변경 시에만 재빌드하고, vcpkg baseline으로 고정하고 있어 실질적 위험은 낮음.
- **조치**: 보고만 -- sha 태그(`sha-{commit}`) 기반 고정은 build-image와 build 간의 job 순서로 자연스럽게 해소되는 구조. 현행 유지 가능.

---

## 검증 완료 (이슈 없음)

### CMake 품질
- 모든 타겟에 `apex_set_warnings()` 적용 확인 (apex_bcrypt, INTERFACE 라이브러리 제외)
- 타겟 기반 빌드 일관적 사용 (`target_link_libraries`, `target_include_directories`)
- 하드코딩 경로 없음 (빌드 디렉토리는 `${CMAKE_CURRENT_*}` 변수 사용)
- FlatBuffers 스키마 컴파일이 각 서브프로젝트에서 독립적으로 설정

### vcpkg 의존성
- `builtin-baseline` 커밋으로 버전 고정 (`b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01`)
- CI Docker 이미지와 Windows vcpkg 모두 동일 커밋 사용
- 선언된 의존성이 실제 `find_package()`와 일치

### CI 워크플로우
- `permissions: {}` (최소 권한) 루트 레벨 설정, job별 필요 권한만 추가
- `concurrency` + `cancel-in-progress` 설정으로 중복 빌드 방지
- 5잡 매트릭스 (MSVC, GCC-14, ASAN, TSAN, UBSAN) 완비
- `paths-ignore`로 문서 전용 변경 시 빌드 스킵
- E2E job이 build 성공 시에만 실행 (의존성 체인 정상)
- Nightly Valgrind이 MemCheck + E2E + 스트레스 테스트 커버

### Docker
- 멀티스테이지 빌드 (builder -> runtime) 적용
- runtime 이미지에 빌드 도구 미포함 (최소 이미지)
- `.dockerignore`는 별도 확인 불필요 (COPY context가 전체 소스이지만 builder 스테이지에서만 사용)
- ci.Dockerfile은 빌드 전용으로 보안 민감 데이터 미포함

### 크로스컴파일러 호환
- `#ifdef _MSC_VER` / `#ifdef _WIN32` 분기 일관적 사용
- `_aligned_malloc`/`_aligned_free` 분기 (slab_allocator 등)
- `/W4 /WX` (MSVC) + `-Wall -Wextra -Wpedantic -Werror` (GCC/Clang) 3개 컴파일러 경고 0건
- `<cstdint>` 명시적 include (wire_header.hpp 등)

### 입력 검증
- WireHeader::parse()에서 body_size > MAX_BODY_SIZE(16MB) 검증
- FlatBuffers 역직렬화 후 필드 null 체크 (auth_service.cpp의 req->email(), req->password())
- 설정 파싱에서 `checked_narrow<>()` 템플릿으로 정수 오버플로 방지 (config.cpp)
- Gateway 설정에서 환경변수 치환 시 regex 기반 안전한 파싱

### SQL 인젝션 방지
- 모든 PG 쿼리가 `PQsendQueryParams` (파라미터 바인딩) 사용
- auth_service.cpp의 모든 쿼리: `$1`, `$2` 등 placeholder 사용, 문자열 연결 없음

### JWT/인증
- RS256 비대칭 알고리즘 사용 (Auth가 서명, Gateway가 검증)
- clock_skew 설정으로 시간 허용치 제어
- issuer 검증 필수
- 토큰 블랙리스트 (Redis) + refresh token rotation + token family 기반 재사용 탐지
- bcrypt work_factor=12 (적정 수준)

### Rate Limiting
- 3계층 rate limit (Per-IP local -> Per-User Redis -> Per-Endpoint Redis)
- IP rate limit에 max_entries 제한 (65536)으로 메모리 DoS 방지
- Redis 오류 시 fail-open 정책 (가용성 우선) -- 문서화됨

### 보안 기본값
- heartbeat_timeout_ticks=300 (0이면 disabled, 무제한 아님)
- drain_timeout=25s (적정)
- max_pending_per_core=65536 (DoS 방어)
- max_subscriptions_per_session=50 (0=unlimited이지만 기본값이 50)

### 크레덴셜 보안
- `.env` gitignore 확인
- pgbouncer/userlist.txt에 경고 주석 포함 ("로컬 개발 전용")
- Gateway 설정 파서가 `${ENV_VAR:-default}` 환경변수 치환 지원
- 테스트 키(test_rs256*.pem)가 tests/keys/에 격리

---

## 수정 파일

| 파일 | 변경 내용 |
|------|-----------|
| `apex_services/auth-svc/src/password_hasher.cpp` | bcrypt 해시 로그 노출 제거 |
| `apex_infra/docker/gateway.Dockerfile` | non-root 실행 (USER apex) |
| `apex_infra/docker/auth-svc.Dockerfile` | non-root 실행 (USER apex) |
| `apex_infra/docker/chat-svc.Dockerfile` | non-root 실행 (USER apex) |
