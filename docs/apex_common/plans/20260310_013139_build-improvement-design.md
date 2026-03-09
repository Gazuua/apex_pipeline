# 빌드 환경 개선 설계서

## 개요

로컬 개발 환경 안정화 + CI 최적화를 위한 빌드 인프라 전면 개선.
Phase 1 (로컬) → Phase 2 (CI Docker) 순서로 진행하며, PR을 분리한다.
(→ 실제 구현 시 단일 PR로 변경: Phase 1+2가 밀접하게 연관되어 분리 시 CI 검증이 불완전해지므로)

## 결정 사항 요약

| 항목 | 결정 |
|------|------|
| 스코프 | Phase 1 + 2, 단일 PR (설계 시 PR 분리 계획 → 구현 시 단일 PR로 변경: Phase 1+2가 밀접하게 연관되어 분리 시 CI 검증이 불완전해지므로) |
| build-root CI 잡 | Docker로 같이 전환 |
| 사전 체크 수준 | 존재 + 버전 체크, 패키지명 + 버전번호 명시 (설치 커맨드 X) |
| Docker 베이스 | ubuntu:24.04 + GCC 14 |
| Linux 개발환경 | Phase 2 Docker 이미지를 로컬 Linux 빌드/디버깅에도 재활용 |
| `${hostSystemName}` | 비용 제로로 포함 (WSL 대비) |
| vcpkg 전략 | VCPKG_INSTALLED_DIR 공유 + 기존 binary cache 활용 + CI 캐싱 |
| 최소 요구 버전 | cmake ≥ 3.25, g++ ≥ 14, ninja ≥ 1.11 (ubuntu:24.04 기본 레포 기준) |
| CMake 4.x | 하위 호환 유지, 최소 3.25로 충분. 4.x 전용 기능 없음 |

---

## Phase 1 — 로컬 개발 환경 안정화

### 1-1. `.gitattributes` (CRLF → LF 정규화)

루트에 `.gitattributes` 신규 생성:
```
* text=auto eol=lf
*.bat text eol=crlf
*.cmd text eol=crlf
```

적용 절차:
1. `.gitattributes` 커밋
2. `git add --renormalize .` → 기존 파일 EOL 일괄 변환 커밋

### 1-2. vcpkg 재설치 완전 제거

5가지 조치로 vcpkg 재설치가 일어날 수 있는 모든 경로를 차단:

**A. `VCPKG_INSTALLED_DIR` 공유 (CMakePresets.json)**

루트 `CMakePresets.json`의 `default` 프리셋 cacheVariables에 추가:
```json
"VCPKG_INSTALLED_DIR": "${sourceDir}/vcpkg_installed"
```

모든 프리셋이 단일 `vcpkg_installed/`를 공유. triplet별(`x64-windows/`, `x64-linux/`) 자동 분리.
`.gitignore`에 이미 `vcpkg_installed/` 등록 완료.

**B. 로컬 Windows binary cache**

이미 `$LOCALAPPDATA/vcpkg/archives/`로 동작 중. 추가 조치 불필요.

**C. build.sh에 Linux binary cache 경로 명시**
```bash
export VCPKG_BINARY_SOURCES="clear;files,${HOME}/.cache/vcpkg/archives,readwrite"
```

**D. CI: `vcpkg_installed/` 디렉토리 캐싱 (Phase 2 Docker 전까지)**

`ci.yml`에 `actions/cache@v4` 추가:
```yaml
- uses: actions/cache@v4
  with:
    path: apex_core/vcpkg_installed
    key: vcpkg-${{ matrix.preset }}-${{ hashFiles('apex_core/vcpkg.json') }}
```

**E. Phase 2에서 Docker 이미지에 사전 설치 (후속)**

Docker 이미지 빌드 시 `vcpkg install`까지 완료 → CI 설치 단계 자체 제거.

### 1-3. 빌드 스크립트 리팩토링

**A. 사전 체크 + 버전 검증**

- build.sh: cmake ≥ 3.25, g++ ≥ 14, ninja ≥ 1.11, VCPKG_ROOT 유효성
- build.bat: cmake, ninja PATH 확인, vcvarsall.bat 호출 결과 검증, VCPKG_ROOT 경로 유효성
- 미충족 시: `"Error: cmake 3.20 found, but >= 3.25 required"` 형태로 패키지명 + 버전번호 안내
- 자동 설치는 하지 않음 (Dockerfile / 개발자가 담당)

**B. `${hostSystemName}` 빌드 디렉토리 분리**

`CMakePresets.json` default 프리셋:
```json
"binaryDir": "${sourceDir}/build/${hostSystemName}/${presetName}"
```

`cmakeMinimumRequired`를 3.20 → 3.25로 상향 (${hostSystemName} 매크로 요구).

결과 디렉토리 구조:
```
vcpkg_installed/        ← 공유 (triplet별 자동 분리)
build/
  Windows/debug/
  Linux/debug/
  Linux/asan/
  Linux/tsan/
```

**C. 빌드 스크립트 경로 반영**
- build.sh: `BUILD_DIR="build/$(uname -s)/$PRESET"`
- build.bat: `set BUILD_DIR=build\Windows\%PRESET%`
- `mkdir`, `cmake --build`, `cp compile_commands.json` 등을 `BUILD_DIR` 변수로 통일

**D. CI workflow 경로 반영**

`cmake --build --preset` 사용을 검토하여 경로 직접 지정 제거 가능.
또는 OS별 경로 명시: `build/Linux/${{ matrix.preset }}`, `build/Windows/${{ matrix.preset }}`.

---

## Phase 2 — CI 최적화

### 2-1. Docker 이미지 (CI + 로컬 Linux 빌드 겸용)

파일: `apex_infra/docker/ci.Dockerfile`

```dockerfile
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    g++-14 cmake ninja-build git curl zip unzip tar pkg-config \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT \
    && cd $VCPKG_ROOT \
    && git checkout b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01 \
    && ./bootstrap-vcpkg.sh -disableMetrics

COPY vcpkg.json /tmp/vcpkg.json
RUN $VCPKG_ROOT/vcpkg install --x-manifest-root=/tmp \
    && rm /tmp/vcpkg.json
```

vcpkg 커밋 해시는 `vcpkg.json`의 `builtin-baseline`과 동일하게 유지.

용도별 사용법:
```bash
# CI: container 옵션
container:
  image: ghcr.io/gazuua/apex-pipeline-ci:latest

# 로컬 Linux 빌드 검증
docker run -v D:/.workspace:/workspace -w /workspace \
    ghcr.io/gazuua/apex-pipeline-ci:latest ./apex_core/build.sh debug

# 로컬 Linux 디버깅
docker run -it --cap-add=SYS_PTRACE \
    -v D:/.workspace:/workspace -w /workspace \
    ghcr.io/gazuua/apex-pipeline-ci:latest bash
```

GHCR 태깅:
- `ghcr.io/gazuua/apex-pipeline-ci:latest` — 항상 최신
- `ghcr.io/gazuua/apex-pipeline-ci:sha-<short-hash>` — 재현 가능한 태그

### 2-2. CI workflow 분리

**`docker-build.yml`** — Docker 이미지 빌드 전용
- 트리거: Dockerfile / vcpkg.json 변경 시 + workflow_call
- 이미지 빌드 → GHCR push (latest + sha 태그)

**`ci.yml`** — 소스 빌드/테스트 리팩토링
- check-image: Dockerfile/vcpkg.json 변경 감지
- build-image: 변경 시 workflow_call로 docker-build.yml 호출
- build: Linux 잡은 container 사용, Windows 잡은 기존 방식 유지
- build-root: 루트 빌드 정합성 검증 (동일 container 사용)
- 안전장치: build-image 실패 시 build 잡도 중단 (`success || skipped` 조건)

Phase 2 적용 후:
- Linux CI 잡에서 apt-get / vcpkg 설치 단계 완전 제거
- vcpkg_installed/ CI 캐싱(1-2-D)은 Windows 잡에만 유지

---

## Future — ubuntu:26.04 + GCC 15 마이그레이션

지금 구현하지 않음. 트리거 조건 및 예상 변경 범위만 기록.

### 트리거 조건
- Ubuntu 26.04 LTS 출시 (2026년 4월 23일 예정)

### 변경 범위
- ci.Dockerfile: `FROM ubuntu:26.04`, `g++-15` 설치
- CMakePresets.json: `cmakeMinimumRequired` 검토 (CMake 4.x 전용 기능 필요 시)
- 빌드 스크립트: 최소 요구 버전 업데이트
- CI workflow: vcpkg baseline 업데이트 검토

### 기대 효과
- C++23 코루틴 코드젠 개선 → 런타임 성능 향상
- C++26 실험적 기능 사용 가능
- 보안 패치 지원 2031년까지 (LTS 5년)
