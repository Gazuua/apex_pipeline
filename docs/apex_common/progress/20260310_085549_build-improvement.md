# 빌드 환경 개선 — 완료 기록

- **PR**: #6 (squash merged)
- **브랜치**: `feature/build-improvement`
- **기간**: 2026-03-10
- **설계서**: `docs/apex_common/plans/20260310_025750_build-improvement-design.md`

## 구현 내용

### Phase 1 — 로컬 개발 환경 안정화
- `.gitattributes` CRLF/LF 정규화 (`* text=auto eol=lf`, `*.sh eol=lf`, `*.bat eol=crlf`)
- `VCPKG_INSTALLED_DIR` 프리셋 공유 (`${sourceDir}/vcpkg_installed`)
- `${hostSystemName}` 기반 빌드 디렉토리 분리 (`build/Windows/debug`, `build/Linux/debug`)
- 빌드 스크립트 리팩토링: cmake/ninja/gcc 버전 검증, VCPKG_ROOT 유효성 체크
- `uname -s` → `hostSystemName` 매핑 (Windows Git Bash 호환)

### Phase 2 — CI Docker 최적화
- `ci.Dockerfile`: ubuntu:24.04 + GCC 14 + vcpkg 사전 설치 (`/opt/vcpkg_installed`)
- `docker-build.yml`: `workflow_call` 기반 재사용 가능 Docker 빌드 워크플로우
- `ci.yml` Docker 컨테이너 전환: Linux 잡 apt-get/vcpkg 설치 완전 제거
- 조건부 Docker 이미지 리빌드 (`dorny/paths-filter` — Dockerfile/vcpkg.json 변경 시만)
- `-DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed` 오버라이드로 Docker 사전 설치 패키지 재사용 (708μs)

### 추가 개선
- `.dockerignore`: Docker build context 최소화
- shell scripts 실행 권한 (`100755`)
- Windows vcpkg_installed 캐싱 (`actions/cache@v4`)

## CI 결과
- 7/7 jobs 전체 통과 (linux-gcc, linux-asan, linux-tsan, windows-msvc, root-linux-gcc, check-image, build-image)
- Linux 컨테이너 잡: vcpkg install 708μs (사전 설치 패키지 재사용)

## auto-review
- 총 4라운드 (2 세션)
- Round 1 (세션1): 16건 → 10건 수정, 6건 스킵
- Round 2 (세션1): 4/5 Clean
- Round 1 (세션2): 22건 → 6건 수정, 나머지 스킵
- Round 2 (세션2): 5/5 Clean ✅
