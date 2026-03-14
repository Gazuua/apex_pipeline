# 빌드 환경 개선 구현 계획

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 로컬 개발 환경 안정화(Phase 1) + CI Docker 최적화(Phase 2)를 통해 빌드 인프라를 전면 개선한다.

**Architecture:** Phase 1에서 .gitattributes, vcpkg 공유, 빌드 스크립트 리팩토링으로 로컬 환경을 안정화한 뒤, Phase 2에서 Docker 이미지 + CI workflow 분리로 CI를 최적화한다. Phase별 PR을 분리하며, 동일 브랜치(`feature/build-improvement`)에서 작업한다.

**Tech Stack:** CMake 3.25+ (프리셋 v6), vcpkg, Docker (ubuntu:24.04), GitHub Actions, GHCR

**설계서:** `docs/apex_common/plans/20260310_025750_build-improvement-design.md`

**워크트리:** `.worktrees/build-improvement` (브랜치: `feature/build-improvement`)

---

## Phase 1 — 로컬 개발 환경 안정화

### Task 1: .gitattributes 생성 + EOL 정규화

**Files:**
- Create: `.gitattributes`

**Step 1: .gitattributes 파일 생성**

```
* text=auto eol=lf
*.bat text eol=crlf
*.cmd text eol=crlf
```

**Step 2: 커밋**

```bash
git add .gitattributes
git commit -m "chore: add .gitattributes for CRLF/LF normalization"
```

**Step 3: 기존 파일 EOL 일괄 정규화**

```bash
git add --renormalize .
git status  # 변경된 파일 확인
```

**Step 4: 정규화 결과 커밋**

```bash
git commit -m "chore: renormalize line endings via .gitattributes"
```

변경된 파일이 없으면 커밋 스킵.

---

### Task 2: CMakePresets.json 수정

**Files:**
- Modify: `CMakePresets.json` (루트)

3가지를 한 번에 수정:

**Step 1: `cmakeMinimumRequired` 버전 상향**

```json
"cmakeMinimumRequired": { "major": 3, "minor": 25, "patch": 0 }
```

기존 3.20 → 3.25 (`${hostSystemName}` 매크로 요구).

**Step 2: `default` 프리셋에 `VCPKG_INSTALLED_DIR` 추가**

`cacheVariables`에 추가:
```json
"VCPKG_INSTALLED_DIR": "${sourceDir}/vcpkg_installed"
```

**Step 3: `default` 프리셋의 `binaryDir` 변경**

```json
"binaryDir": "${sourceDir}/build/${hostSystemName}/${presetName}"
```

기존: `"${sourceDir}/build/${presetName}"`

**Step 4: 변경 후 전체 CMakePresets.json 확인**

변경된 `default` 프리셋은 다음과 같아야 함:
```json
{
  "name": "default",
  "displayName": "Default (Release)",
  "generator": "Ninja",
  "binaryDir": "${sourceDir}/build/${hostSystemName}/${presetName}",
  "environment": {
    "VCPKG_ROOT": "$penv{VCPKG_ROOT}"
  },
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Release",
    "CMAKE_CXX_STANDARD": "23",
    "CMAKE_CXX_STANDARD_REQUIRED": "ON",
    "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
    "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
    "VCPKG_INSTALLED_DIR": "${sourceDir}/vcpkg_installed"
  }
}
```

**Step 5: 로컬 Windows 빌드로 검증**

```bash
cmd.exe //c "apex_core\\build.bat debug"
```

Expected: configure 단계에서 `build/Windows/debug/`에 빌드 파일 생성.
이 시점에서 build.bat의 경로가 옛날 경로(`build/%PRESET%`)를 참조하므로 **실패할 수 있음** — Task 4에서 수정. configure만 성공하면 OK.

**Step 6: 커밋**

```bash
git add CMakePresets.json
git commit -m "build: VCPKG_INSTALLED_DIR sharing + hostSystemName build dir separation

- cmakeMinimumRequired 3.20 → 3.25 (${hostSystemName} macro)
- VCPKG_INSTALLED_DIR: ${sourceDir}/vcpkg_installed (preset sharing)
- binaryDir: ${sourceDir}/build/${hostSystemName}/${presetName}"
```

---

### Task 3: build.sh 리팩토링

**Files:**
- Modify: `apex_core/build.sh`

**Step 1: 전체 build.sh 재작성**

```bash
#!/bin/bash
set -e

# ── Minimum required versions ──────────────────────
REQUIRED_CMAKE_MAJOR=3
REQUIRED_CMAKE_MINOR=25
REQUIRED_GCC_MAJOR=14
REQUIRED_NINJA_MAJOR=1
REQUIRED_NINJA_MINOR=11

# ── Helper functions ───────────────────────────────
die() { echo "Error: $*" >&2; exit 1; }

check_command() {
    command -v "$1" >/dev/null 2>&1 || die "$1 not found (required: $2)"
}

version_ge() {
    # Returns 0 if $1.$2 >= $3.$4
    [ "$1" -gt "$3" ] && return 0
    [ "$1" -eq "$3" ] && [ "$2" -ge "$4" ] && return 0
    return 1
}

# ── Pre-flight checks ─────────────────────────────
check_command cmake "cmake >= ${REQUIRED_CMAKE_MAJOR}.${REQUIRED_CMAKE_MINOR}"
check_command ninja "ninja >= ${REQUIRED_NINJA_MAJOR}.${REQUIRED_NINJA_MINOR}"
check_command g++-14 "g++ >= ${REQUIRED_GCC_MAJOR}"

# cmake version check
CMAKE_VERSION=$(cmake --version | head -1 | grep -oP '\d+\.\d+' | head -1)
CMAKE_MAJ=${CMAKE_VERSION%%.*}
CMAKE_MIN=${CMAKE_VERSION##*.}
version_ge "$CMAKE_MAJ" "$CMAKE_MIN" "$REQUIRED_CMAKE_MAJOR" "$REQUIRED_CMAKE_MINOR" \
    || die "cmake ${CMAKE_VERSION} found, but >= ${REQUIRED_CMAKE_MAJOR}.${REQUIRED_CMAKE_MINOR} required"

# ninja version check
NINJA_VERSION=$(ninja --version)
NINJA_MAJ=${NINJA_VERSION%%.*}
NINJA_REST=${NINJA_VERSION#*.}
NINJA_MIN=${NINJA_REST%%.*}
version_ge "$NINJA_MAJ" "$NINJA_MIN" "$REQUIRED_NINJA_MAJOR" "$REQUIRED_NINJA_MINOR" \
    || die "ninja ${NINJA_VERSION} found, but >= ${REQUIRED_NINJA_MAJOR}.${REQUIRED_NINJA_MINOR} required"

# gcc version check
GCC_VERSION=$(g++-14 -dumpversion)
GCC_MAJ=${GCC_VERSION%%.*}
[ "$GCC_MAJ" -ge "$REQUIRED_GCC_MAJOR" ] \
    || die "g++ ${GCC_VERSION} found, but >= ${REQUIRED_GCC_MAJOR} required"

# VCPKG_ROOT check
[ -n "$VCPKG_ROOT" ] || die "VCPKG_ROOT is not set"
[ -d "$VCPKG_ROOT" ] || die "VCPKG_ROOT path does not exist: $VCPKG_ROOT"

# ── vcpkg binary caching ──────────────────────────
export VCPKG_BINARY_SOURCES="clear;files,${HOME}/.cache/vcpkg/archives,readwrite"

# ── Build ──────────────────────────────────────────
PRESET="${1:-debug}"
BUILD_DIR="build/$(uname -s)/$PRESET"

cd "$(dirname "$0")"

# Ensure build dir and compile_commands.json exist for first configure (clangd)
mkdir -p "$BUILD_DIR"
[ -f "$BUILD_DIR/compile_commands.json" ] || touch "$BUILD_DIR/compile_commands.json"

# Configure
echo "[build.sh] Configuring preset: $PRESET"
cmake --preset "$PRESET"

# Copy compile_commands.json to project root for clangd
cp "$BUILD_DIR/compile_commands.json" compile_commands.json 2>/dev/null || true

# Build
cmake --build "$BUILD_DIR"

# Test
ctest --preset "$PRESET" --output-on-failure
```

**Step 2: 실행 권한 확인**

```bash
chmod +x apex_core/build.sh
```

**Step 3: 커밋**

```bash
git add apex_core/build.sh
git commit -m "build(sh): add pre-flight checks + version validation + hostSystemName paths

- Required: cmake >= 3.25, g++ >= 14, ninja >= 1.11
- VCPKG_ROOT existence + path validation
- VCPKG_BINARY_SOURCES for Linux binary caching
- BUILD_DIR updated for \${hostSystemName} layout"
```

---

### Task 4: build.bat 리팩토링

**Files:**
- Modify: `apex_core/build.bat`

**Step 1: 전체 build.bat 재작성**

```bat
@echo off
setlocal enabledelayedexpansion

set PRESET=%~1
if "%PRESET%"=="" set PRESET=debug

:: ── Pre-flight checks ─────────────────────────────

:: cmake check
where cmake >nul 2>&1 || (echo Error: cmake not found ^(required: cmake ^>= 3.25^) & exit /b 1)
for /f "tokens=3" %%v in ('cmake --version 2^>^&1 ^| findstr /c:"cmake version"') do set CMAKE_VER=%%v
for /f "tokens=1,2 delims=." %%a in ("%CMAKE_VER%") do (
    if %%a LSS 3 (echo Error: cmake %CMAKE_VER% found, but ^>= 3.25 required & exit /b 1)
    if %%a EQU 3 if %%b LSS 25 (echo Error: cmake %CMAKE_VER% found, but ^>= 3.25 required & exit /b 1)
)

:: ninja check
where ninja >nul 2>&1 || (echo Error: ninja not found ^(required: ninja ^>= 1.11^) & exit /b 1)

:: VS2022 vcvarsall.bat
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTALL=%%i"
) else (
    set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\2022\Community"
)
if not exist "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo Error: vcvarsall.bat not found at %VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo Error: vcvarsall.bat failed & exit /b 1)

:: VCPKG_ROOT check
if "%VCPKG_ROOT%"=="" set VCPKG_ROOT=%USERPROFILE%\vcpkg
if not exist "%VCPKG_ROOT%" (
    echo Error: VCPKG_ROOT path does not exist: %VCPKG_ROOT%
    exit /b 1
)

:: ── Build ──────────────────────────────────────────
set BUILD_DIR=build\Windows\%PRESET%

cd /d %~dp0

:: Ensure build dir and compile_commands.json exist for first configure (clangd)
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%BUILD_DIR%\compile_commands.json" type nul > "%BUILD_DIR%\compile_commands.json"

:: Configure
echo [build.bat] Configuring preset: %PRESET%
cmake --preset %PRESET%
if errorlevel 1 exit /b 1

:: Copy compile_commands.json to project root for clangd
copy /Y "%BUILD_DIR%\compile_commands.json" compile_commands.json >nul 2>&1

:: Build
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1

:: Test
ctest --preset %PRESET% --output-on-failure
```

**Step 2: Windows에서 빌드 검증**

```bash
cmd.exe //c "apex_core\\build.bat debug"
```

Expected: 사전 체크 통과 → configure → build → test 모두 성공.
빌드 출력이 `apex_core/build/Windows/debug/`에 생성되는지 확인.

**Step 3: 커밋**

```bash
git add apex_core/build.bat
git commit -m "build(bat): add pre-flight checks + vcvarsall validation + hostSystemName paths

- Required: cmake >= 3.25, ninja (existence only on Windows)
- vcvarsall.bat call result validation
- VCPKG_ROOT path existence check
- BUILD_DIR updated for hostSystemName layout"
```

---

### Task 5: CI workflow 경로 업데이트 + vcpkg 캐싱

**Files:**
- Modify: `.github/workflows/ci.yml`

**Step 1: build 잡에 vcpkg_installed 캐싱 추가 + 경로 업데이트**

`build` 잡의 steps에서 변경할 부분:

```yaml
    steps:
      - uses: actions/checkout@v4

      - name: Setup vcpkg
        uses: lukka/run-vcpkg@v11
        with:
          vcpkgGitCommitId: 'b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01'

      - name: Cache vcpkg_installed
        uses: actions/cache@v4
        with:
          path: apex_core/vcpkg_installed
          key: vcpkg-${{ runner.os }}-${{ hashFiles('apex_core/vcpkg.json') }}

      - name: Setup Ninja
        uses: seanmiddleditch/gha-setup-ninja@v5

      - name: Setup GCC 14
        if: runner.os == 'Linux'
        run: |
          sudo apt-get update
          sudo apt-get install -y g++-14
          echo "CC=gcc-14" >> $GITHUB_ENV
          echo "CXX=g++-14" >> $GITHUB_ENV

      - name: Setup MSVC (Windows)
        if: runner.os == 'Windows'
        uses: ilammy/msvc-dev-cmd@v1

      - name: Configure
        working-directory: apex_core
        run: cmake --preset ${{ matrix.preset }}

      - name: Build
        working-directory: apex_core
        run: cmake --build build/${{ runner.os == 'Windows' && 'Windows' || 'Linux' }}/${{ matrix.preset }}

      - name: Test
        working-directory: apex_core
        run: ctest --preset ${{ matrix.preset }}
```

**Step 2: build-root 잡도 동일하게 경로 업데이트**

```yaml
  build-root:
    # ... 기존 strategy/matrix/env 유지 ...
    steps:
      - uses: actions/checkout@v4

      - name: Setup vcpkg
        uses: lukka/run-vcpkg@v11
        with:
          vcpkgGitCommitId: 'b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01'

      - name: Cache vcpkg_installed
        uses: actions/cache@v4
        with:
          path: vcpkg_installed
          key: vcpkg-root-${{ runner.os }}-${{ hashFiles('vcpkg.json') }}

      - name: Setup Ninja
        uses: seanmiddleditch/gha-setup-ninja@v5

      - name: Setup GCC 14
        run: |
          sudo apt-get update
          sudo apt-get install -y g++-14
          echo "CC=gcc-14" >> $GITHUB_ENV
          echo "CXX=g++-14" >> $GITHUB_ENV

      - name: Configure
        run: cmake --preset ${{ matrix.preset }}

      - name: Build
        run: cmake --build build/Linux/${{ matrix.preset }}

      - name: Test
        run: ctest --preset ${{ matrix.preset }}
```

**Step 3: 커밋**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: update build paths for hostSystemName + add vcpkg_installed caching

- Build paths: build/{Linux,Windows}/{preset}
- actions/cache for vcpkg_installed directory
- Bridges until Phase 2 Docker migration"
```

---

### Task 6: Phase 1 통합 검증

**Step 1: 로컬 Windows 빌드 전체 검증**

```bash
cmd.exe //c "apex_core\\build.bat debug"
```

Expected: 사전 체크 → configure → build → test 전체 통과.

**Step 2: vcpkg_installed 공유 검증**

```bash
ls apex_core/vcpkg_installed/
```

Expected: triplet 디렉토리(`x64-windows/` 등)가 `build/` 하위가 아닌 `vcpkg_installed/` 직접 하위에 존재.

**Step 3: 빌드 디렉토리 구조 확인**

```bash
ls apex_core/build/Windows/
```

Expected: `debug/` 디렉토리 존재.

**Step 4: CI 푸시 + PR 생성**

```bash
git push -u origin feature/build-improvement
gh pr create --title "build: Phase 1 — local dev environment stabilization" --body "..."
```

CI 통과 확인 후 squash merge.

---

## Phase 2 — CI Docker 최적화

### Task 7: Dockerfile 작성

**Files:**
- Create: `apex_infra/docker/ci.Dockerfile`

**Step 1: 디렉토리 생성 + Dockerfile 작성**

```dockerfile
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Build tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-14 cmake ninja-build git curl zip unzip tar pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Set GCC 14 as default
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

# vcpkg (pinned to builtin-baseline)
ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_FORCE_SYSTEM_BINARIES=1
RUN git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT \
    && cd $VCPKG_ROOT \
    && git checkout b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01 \
    && ./bootstrap-vcpkg.sh -disableMetrics

# Pre-install vcpkg dependencies
COPY vcpkg.json /tmp/vcpkg-manifest/vcpkg.json
RUN $VCPKG_ROOT/vcpkg install \
    --x-manifest-root=/tmp/vcpkg-manifest \
    --x-install-root=/opt/vcpkg_installed \
    && rm -rf /tmp/vcpkg-manifest

ENV CC=gcc-14 CXX=g++-14

WORKDIR /workspace
```

**Step 2: 로컬 Docker 빌드 테스트**

```bash
cd apex_infra/docker
docker build -f ci.Dockerfile -t apex-ci:local ../../
```

참고: `COPY vcpkg.json`이 루트의 vcpkg.json을 참조하므로 context는 repo 루트.
또는 Dockerfile에서 경로를 조정하거나, 빌드 시 `--build-context`를 사용.

**Step 3: Docker 이미지로 빌드 검증**

```bash
docker run --rm -v D:/.workspace:/workspace -w /workspace \
    apex-ci:local ./apex_core/build.sh debug
```

Expected: 사전 체크 통과 → configure → build → test 성공.

**Step 4: 커밋**

```bash
git add apex_infra/docker/ci.Dockerfile
git commit -m "build(docker): add CI Docker image (ubuntu:24.04 + GCC 14 + vcpkg)

- Pre-installed vcpkg dependencies for zero-install CI
- Pinned to vcpkg builtin-baseline commit
- Also usable for local Linux build verification"
```

---

### Task 8: docker-build.yml 작성

**Files:**
- Create: `.github/workflows/docker-build.yml`

**Step 1: workflow 작성**

```yaml
name: Docker CI Image

on:
  push:
    branches: [main]
    paths:
      - 'apex_infra/docker/ci.Dockerfile'
      - '**/vcpkg.json'
  workflow_call:

permissions:
  contents: read
  packages: write

jobs:
  build-and-push:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Log in to GHCR
        uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}

      - name: Build and push
        uses: docker/build-push-action@v6
        with:
          context: .
          file: apex_infra/docker/ci.Dockerfile
          push: true
          tags: |
            ghcr.io/gazuua/apex-pipeline-ci:latest
            ghcr.io/gazuua/apex-pipeline-ci:sha-${{ github.sha }}
```

**Step 2: 커밋**

```bash
git add .github/workflows/docker-build.yml
git commit -m "ci: add Docker image build workflow (GHCR push)

- Triggered on Dockerfile/vcpkg.json changes
- Supports workflow_call for ci.yml integration
- Tags: latest + sha-<commit>"
```

---

### Task 9: ci.yml Docker 전환

**Files:**
- Modify: `.github/workflows/ci.yml`

**Step 1: ci.yml 전면 리팩토링**

```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

concurrency:
  group: ${{ github.workflow }}-${{ github.head_ref || github.ref }}
  cancel-in-progress: true

jobs:
  # ── Docker image rebuild (if needed) ─────────────
  check-image:
    runs-on: ubuntu-latest
    outputs:
      changed: ${{ steps.filter.outputs.docker }}
    steps:
      - uses: actions/checkout@v4
      - uses: dorny/paths-filter@v3
        id: filter
        with:
          filters: |
            docker:
              - 'apex_infra/docker/ci.Dockerfile'
              - '**/vcpkg.json'

  build-image:
    needs: check-image
    if: needs.check-image.outputs.changed == 'true'
    uses: ./.github/workflows/docker-build.yml
    permissions:
      contents: read
      packages: write

  # ── Source build & test ──────────────────────────
  build:
    needs: [check-image, build-image]
    if: >-
      always() &&
      (needs.build-image.result == 'success' || needs.build-image.result == 'skipped')
    strategy:
      fail-fast: false
      matrix:
        include:
          - name: linux-gcc
            os: ubuntu-latest
            preset: debug
            container: true
          - name: linux-asan
            os: ubuntu-latest
            preset: asan
            container: true
          - name: linux-tsan
            os: ubuntu-latest
            preset: tsan
            container: true
          - name: windows-msvc
            os: windows-latest
            preset: debug
            container: false

    name: ${{ matrix.name }}
    runs-on: ${{ matrix.os }}
    container: ${{ matrix.container && 'ghcr.io/gazuua/apex-pipeline-ci:latest' || '' }}

    env:
      VCPKG_ROOT: ${{ matrix.container && '/opt/vcpkg' || format('{0}/vcpkg', github.workspace) }}

    steps:
      # ── Non-container setup (Windows only) ───────
      - uses: actions/checkout@v4

      - name: Setup vcpkg
        if: ${{ !matrix.container }}
        uses: lukka/run-vcpkg@v11
        with:
          vcpkgGitCommitId: 'b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01'

      - name: Cache vcpkg_installed (Windows)
        if: ${{ !matrix.container }}
        uses: actions/cache@v4
        with:
          path: apex_core/vcpkg_installed
          key: vcpkg-${{ runner.os }}-${{ hashFiles('apex_core/vcpkg.json') }}

      - name: Setup Ninja
        if: ${{ !matrix.container }}
        uses: seanmiddleditch/gha-setup-ninja@v5

      - name: Setup MSVC
        if: runner.os == 'Windows'
        uses: ilammy/msvc-dev-cmd@v1

      # ── Build & test ─────────────────────────────
      - name: Configure
        working-directory: apex_core
        run: cmake --preset ${{ matrix.preset }}

      - name: Build
        working-directory: apex_core
        run: cmake --build build/${{ runner.os == 'Windows' && 'Windows' || 'Linux' }}/${{ matrix.preset }}

      - name: Test
        working-directory: apex_core
        run: ctest --preset ${{ matrix.preset }}

  # ── Root build integrity check ───────────────────
  build-root:
    needs: [check-image, build-image]
    if: >-
      always() &&
      (needs.build-image.result == 'success' || needs.build-image.result == 'skipped')
    name: root-linux-gcc
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/gazuua/apex-pipeline-ci:latest

    env:
      VCPKG_ROOT: /opt/vcpkg

    steps:
      - uses: actions/checkout@v4

      - name: Configure
        run: cmake --preset debug

      - name: Build
        run: cmake --build build/Linux/debug

      - name: Test
        run: ctest --preset debug
```

**Step 2: 커밋**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: migrate Linux jobs to Docker container + conditional image rebuild

- Linux jobs use ghcr.io/gazuua/apex-pipeline-ci:latest container
- Windows job retains existing setup (vcpkg action + MSVC)
- check-image + build-image for conditional Docker rebuild
- build-root uses same container for monorepo integrity check"
```

---

### Task 10: Phase 2 통합 검증

**Step 1: 로컬 Docker 이미지 빌드 + 테스트**

```bash
docker build -f apex_infra/docker/ci.Dockerfile -t apex-ci:local .
docker run --rm -v D:/.workspace:/workspace -w /workspace \
    apex-ci:local ./apex_core/build.sh debug
```

Expected: 전체 통과.

**Step 2: CI 푸시 + PR 생성**

Phase 1 PR이 머지된 후, 동일 브랜치에서:

```bash
git push origin feature/build-improvement
gh pr create --title "build: Phase 2 — CI Docker optimization" --body "..."
```

CI 전체 잡(Linux container + Windows native + root) 통과 확인.

**Step 3: 머지 후 정리**

```bash
git checkout main && git pull
git worktree remove .worktrees/build-improvement
git branch -d feature/build-improvement
```

---

## 검증 체크리스트

### Phase 1 완료 조건
- [ ] `.gitattributes` 존재, `*.sh` 파일이 LF
- [ ] `vcpkg_installed/`가 `build/` 외부에 생성됨
- [ ] `build/Windows/debug/` 경로에 빌드 출력 생성
- [ ] `build.bat` 사전 체크: cmake 없으면 에러 메시지 출력
- [ ] `build.sh` 사전 체크: VCPKG_ROOT 없으면 에러 메시지 출력
- [ ] CI 5개 잡 전체 통과

### Phase 2 완료 조건
- [ ] Docker 이미지 로컬 빌드 성공
- [ ] Docker 이미지로 `build.sh debug` 실행 성공
- [ ] CI Linux 잡에서 apt-get/vcpkg 설치 단계 없음 (container 사용)
- [ ] CI Windows 잡 정상 동작 (기존 방식 유지)
- [ ] CI build-root 잡 container로 전환 + 통과
- [ ] GHCR에 이미지 push 성공
