# 빌드 스크립트 사전 체크 동기화 + Dockerfile vcpkg 범위 확장 — 완료 기록

- **브랜치**: `feature/build-preflight-check`
- **기간**: 2026-03-11

## 구현 내용

### P1 — 루트 빌드 스크립트 사전 체크 동기화

- `apex_tools/build-preflight.sh` 공용 헬퍼 생성: cmake/ninja/gcc 버전 검증, VCPKG_ROOT 유효성 체크, VCPKG_BINARY_SOURCES 설정
- `apex_tools/build-preflight.bat` 공용 헬퍼 생성: vcvarsall 탐색/호출/검증, cmake 버전 체크, ninja 존재 체크, VCPKG_ROOT 유효성 체크
- 루트 `build.sh` — `source apex_tools/build-preflight.sh` 추가
- 루트 `build.bat` — `call apex_tools\build-preflight.bat` 추가, 인라인 vcvarsall/vcpkg 로직 제거
- `apex_core/build.sh` — 인라인 체크 56줄 → `source ../apex_tools/build-preflight.sh` 1줄로 교체
- `apex_core/build.bat` — 인라인 체크 33줄 → `call ..\apex_tools\build-preflight.bat` 1줄로 교체

### P2 — Dockerfile vcpkg.json COPY 범위 확장

- `apex_infra/docker/ci.Dockerfile`에 `apex_core/vcpkg.json` COPY 추가
- 루트 + apex_core 양쪽 매니페스트로 vcpkg install 실행
- 현재 의존성은 동일하나, 모노레포 확장 시 서브프로젝트별 의존성 분기에 대비

## 설계 결정

- **bash 헬퍼는 `source` 방식**: 호출자의 `set -e` 컨텍스트에서 실행, `die()` → `exit 1`로 즉시 중단
- **bat 헬퍼에 `setlocal` 미사용**: vcvarsall이 설정하는 PATH/INCLUDE/LIB 환경변수가 호출자에게 전파되어야 함
- **hostSystemName 매핑은 각 스크립트에 유지**: 빌드 디렉토리 결정 로직은 스크립트별 책임

## 변경 파일

| 파일 | 변경 |
|------|------|
| `apex_tools/build-preflight.sh` | 신규 — 공용 bash 사전 체크 헬퍼 |
| `apex_tools/build-preflight.bat` | 신규 — 공용 bat 사전 체크 헬퍼 |
| `build.sh` | source 추가 |
| `build.bat` | call 추가, 인라인 로직 제거 |
| `apex_core/build.sh` | 인라인 → source 교체 |
| `apex_core/build.bat` | 인라인 → call 교체 |
| `apex_infra/docker/ci.Dockerfile` | apex_core/vcpkg.json COPY + 이중 install |
