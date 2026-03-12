# Apex Pipeline — 프로젝트 가이드

## 개요

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.
자체 네트워크 프레임워크 + MSA 아키텍처 (Gateway → Kafka → Services → Redis/PostgreSQL).

## 모노레포 구조

```
D:\.workspace/
├── CMakeLists.txt / CMakePresets.json  ← 모노레포 루트 빌드
├── build.bat / build.sh               ← 루트 빌드 스크립트
├── apex_core/          ← 코어 프레임워크 (C++23, Boost.Asio 코루틴)
│   ├── include/apex/core/  ← 헤더
│   ├── src/ tests/ examples/ benchmarks/ schemas/
│   ├── bin/            ← 빌드 출력
│   └── build.bat / build.sh / CMakePresets.json
├── docs/               ← 전체 프로젝트 문서 (중앙 집중)
│   ├── Apex_Pipeline.md  ← 마스터 설계서
│   ├── apex_common/      ← 공통 (plans/progress/review)
│   └── apex_core/ apex_infra/ apex_shared/ apex_tools/
├── apex_services/      ← MSA 서비스
├── apex_shared/        ← FlatBuffers 스키마 + 공유 C++ 라이브러리 (apex::shared)
├── apex_infra/         ← Docker, K8s 인프라 (Kafka/Redis/PG + Prometheus/Grafana)
│   └── docker/ci.Dockerfile  ← CI + 로컬 Linux 빌드 겸용 Docker 이미지
└── apex_tools/         ← CLI, 스크립트, git-hooks
```

## 빌드 환경

- **OS/컴파일러**: Windows 10 Pro, VS2022 Community (MSVC 19.44), C++23
- **빌드 시스템**: CMake + Ninja, vcpkg (`C:\Users\JHG\vcpkg`)
- **빌드 명령**:
  ```bash
  # MSYS bash에서 실행 (//c 필수 — /c는 MSYS가 경로로 변환함)
  cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"
  ```
- **빌드 변형**: `APEX_BUILD_VARIANT` = debug / asan / tsan
- **출력**: `apex_core/bin/{target}_{variant}.exe` (예: `echo_server_debug.exe`)
- **compile_commands.json**: configure 후 빌드 스크립트에서 루트로 복사 (symlink 아님)
- **의존성 (vcpkg)**: benchmark, boost-asio, boost-unordered, flatbuffers, gtest, spdlog, tomlplusplus — 향후: boost-beast (Phase 8a)

### MSVC 주의사항

- `std::aligned_alloc` 미지원 → `_aligned_malloc` / `_aligned_free` 분기 필요
- CRTP에서 `using FrameType = Derived::FrameType` → 불완전 타입 에러 (템플릿 파라미터로 우회)
- `MessageDispatcher`: `boost::unordered_flat_map` 기반 (기존 `std::array<std::function, 65536>` ~2MB 이슈 해결됨)
- Windows TCP: ws2_32 + mswsock 링크, `_WIN32_WINNT=0x0A00`, IOCP 클라이언트 소켓은 별도 io_context

## 아키텍처 결정

- gRPC 제거 → 자체 프레임워크가 모든 네트워킹 담당
- 코루틴 프레임 할당: mimalloc/jemalloc + HALO (ADR-21) — 설계 결정, 미구현
- 서비스별 독립 vcpkg.json (Docker 독립 빌드용)
- 상세: `docs/apex_core/design-decisions.md`, `docs/apex_core/design-rationale.md` (ADR 23개)

## 로드맵

- Phase 1~4.7 레거시 넘버링 동결, 5+ 순차 정수 (5.5/8a/8b 허용)
- Phase 5 완료 → **Phase 5.5 (코어 성능 완성)** → Phase 6~10 의존성 기반 재분할
- Phase 5.5 상세: `docs/apex_common/plans/20260311_204613_phase5_5_v6.md`
- 버전: v0.3.0(P6+7), v0.4.0(P8b), v0.5.0(P9), v1.0.0(P10)

## 워크플로우 규칙

### Git / 브랜치
- **초기 설정** (클론 후 1회): `git config core.hooksPath apex_tools/git-hooks`
- **main 직접 커밋 절대 금지** (pre-commit hook 강제) — feature/* 또는 bugfix/* 에서 작업
- **git worktree**: `.worktrees/` 하위에 생성, 에이전트별 독립 작업
  - 생성: `git worktree add .worktrees/<name> -b <branch-name>`
  - 삭제: `git worktree remove .worktrees/<name>`
- **머지**: 리뷰 이슈 0건 → squash merge → 브랜치+워크트리 삭제

### 문서
- **필수 작성**: 계획서(`plans/`), 완료 기록(`progress/`), 리뷰 보고서(`review/`)
- **작성 타이밍**: plans → 구현 전, review → 리뷰 완료 후, progress → CI 통과 후 merge 전
- **문서 위치**: 프로젝트 전용 → `docs/<project>/`, 공통 → `docs/apex_common/`, 걸치는 문서 → 양쪽에 관점 조정하여 작성 (단순 복사 금지)
- 파일명: `YYYYMMDD_HHMMSS_<topic>.md` — 타임스탬프는 실제 작성 시간

### 코드 리뷰
- **clangd LSP + superpowers:code-reviewer 병행** — LSP 정적 분석(타입/참조/호출 추적)과 AI 코드 리뷰를 함께 사용해야 품질이 높아진다
- **clangd LSP 효율 전략**: `documentSymbol` 병렬 → 핵심 API `hover` → 의심 패턴 `findReferences`/`incomingCalls`. 전수 분석 금지, 10분 타임아웃.
- **설계 문서 정합성**: 아키텍처 영향 변경 시 `Apex_Pipeline.md` 일치 확인 필수
- **태스크 완료 후 `/auto-review task` 묻지 말고 자동 실행** — 리뷰 → 수정 → 재리뷰 → Clean → PR+CI 전 과정 자동화

### 브레인스토밍
- 1단계에서 반드시 `docs/Apex_Pipeline.md` 읽고 관련 섹션 식별, 설계 후 업데이트 포함

### 벤치마크 실행
- **순차 실행 필수** — 병렬 실행 시 CPU 경합으로 결과 오염. 절대 동시에 돌리지 않는다
- **백그라운드 서브에이전트 1개**에서 11개를 순차 실행 (Bash 타임아웃 회피)
- **Release 빌드**(`default` 프리셋)로 측정 — Debug는 참고용
- 결과 JSON 저장: `apex_core/benchmark_results/`

### 에이전트 작업
- **모든 작업은 에이전트 팀 병렬 실행** — 수정 가능 파일 목록 명시해서 충돌 방지

## 프로젝트 정보

- GitHub: `Gazuua/apex_pipeline`

## CI/CD 트러블슈팅

- **TSAN**: Boost.Asio `atomic_thread_fence` false positive → `tsan_suppressions.txt` (루트+apex_core 양쪽 배치)
- **ASAN/LSAN**: spdlog 글로벌 레지스트리 leak → `lsan_suppressions.txt`
- **ASAN aligned_alloc**: size는 alignment 배수여야 함. `max(capacity, alignment)`로 보정
- **CMakePresets ${sourceDir}**: 루트+하위 양쪽에 suppressions 파일 배치 (include 시 `${sourceDir}` 변환 대응)
- **[[nodiscard]]**: GCC에서 EXPECT_THROW 내 반환값 경고 → `(void)` 캐스트
- **test preset**: TSAN_OPTIONS/LSAN_OPTIONS는 configure preset이 아닌 **test preset**에 설정
- **CI workflow**: `ctest --preset <name>` 사용 (--test-dir 대신)
- **vcpkg 다운로드 실패**: GitHub CDN 간헐적 HTTP 502 → `gh run rerun --failed`
- **CI 실패 분석 원칙**: 한 잡만 보고 판단하지 말고 **모든 실패 잡의 로그를 확인** — 잡마다 실패 원인이 다를 수 있음
