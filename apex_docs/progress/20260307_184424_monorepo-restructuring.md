# 모노레포 구조 전환 진행 현황

## 목표
BoostAsioCore 단일 프로젝트 → apex-pipeline 모노레포 전환

## 완료 작업

### 1. 디렉토리 구조 설계
- 브레인스토밍을 통한 구조 확정 (`apex_docs/plans/20260307_174528_directory-structure-design.md`)
- 모든 루트 디렉토리에 `apex_` 프리픽스 통일
- 서비스별 독립 vcpkg.json + Docker 빌드 방식 결정

### 2. GitHub 레포지토리 생성
- `Gazuua/apex_pipeline` 으로 생성 및 push
- apex_core 기존 git 히스토리 보존 (`git subtree add`)

### 3. apex_core 리네이밍 + v0.2.2 기능 복구
- `BoostAsioCore` → `apex_core` 텍스트 치환 (10개 문서 파일)
- v0.2.2 feature 브랜치 손실분 재구현 완료 (에러 전파, ProtocolBase CRTP, ADR-21)
- 빌드 18/18 테스트 통과

### 4. CMake 구조 정리
- `apex_core/core/*` → `apex_core/` 플래트닝 (core/ 중첩 제거)
- `project(apex-pipeline)` → `project(apex-core)` 프로젝트명 수정
- `apex_core/CMakeLists.txt`: standalone/subdirectory 겸용 가드 추가
- 루트 `CMakeLists.txt` + `CMakePresets.json` + `vcpkg.json` 생성
- 루트 `build.bat` + `build.sh` 생성
- `compile_commands.json`: CMake symlink → 빌드 스크립트에서 configure 후 복사로 변경
- 루트 `.gitignore` 추가
- 루트 빌드 57/57 + 테스트 18/18, standalone 빌드 57/57 + 테스트 18/18 통과

### 5. 빌드 출력 디렉토리 (bin/)
- 프로젝트별 `bin/` 디렉토리에 exe 출력 (`RUNTIME_OUTPUT_DIRECTORY`)
- 빌드 변형별 파일명: `echo_server.exe` (release), `echo_server_debug.exe`, `echo_server_asan.exe`
- `APEX_BUILD_VARIANT` 캐시 변수로 프리셋별 suffix 제어 (루트 + apex_core CMakePresets.json)
- 루트 빌드 / standalone 빌드 모두 `apex_core/bin/`으로 일관된 출력 경로
- `.clangd` 파일 루트에 추가 (apex_core 동일 설정)

### 6. 문서 정비
- `apex_docs/` 하위 문서 파일명 `YYYYMMDD_HHMMSS_` 형식 통일
- `apex_core/docs/progress/` 에 v0.2.2 완료 문서 작성
- 루트 워크스페이스 MEMORY 파일 생성 (MEMORY.md + 상세 2개)

---

## 미완료 작업 (다음 세션)

### 1. 디렉토리 스캐폴딩
설계 문서에 명시된 아래 디렉토리 미생성:
- `apex_services/` — gateway, auth-svc, chat-svc, log-svc
- `apex_shared/` — 공유 FlatBuffers 스키마 + 공유 C++ 코드
- `apex_infra/` — docker-compose.yml, k8s/
- `apex_tools/` — 서비스 스캐폴딩 스크립트 등

### 2. apex_core 문서 경로 갱신
core/ 플래트닝 후 문서 내 `core/include/...`, `core/src/...` 등 기존 경로 참조가 남아 있을 수 있음. 확인 및 수정 필요.

### 3. apex_core v0.2.2 코드 리뷰 미수정 건
Important 5건 미수정 상태. 수정 후 머지 판단 필요.

### 4. MEMORY 파일 동기화
모노레포 구조 변경 사항 (core/ 제거, 루트 CMake 추가 등) 을 MEMORY 파일에 반영 필요.
