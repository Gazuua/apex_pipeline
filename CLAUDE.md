# Apex Pipeline — 프로젝트 가이드

## 개요

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.
자체 네트워크 프레임워크 + MSA 아키텍처 (Gateway → Kafka → Services → Redis/PostgreSQL).

## 모노레포 구조

```
D:\.workspace/
├── apex_core/          ← 코어 프레임워크 (C++23, Boost.Asio 코루틴)
│   ├── include/apex/core/  ← 헤더
│   ├── src/ tests/ examples/ schemas/
│   ├── bin/            ← 빌드 출력
│   └── build.bat / build.sh / CMakePresets.json
├── docs/               ← 전체 프로젝트 문서 (중앙 집중)
│   ├── Apex_Pipeline.md  ← 마스터 설계서
│   ├── apex_common/      ← 공통 (plans/progress/review)
│   └── apex_core/ apex_infra/ apex_shared/
├── apex_services/      ← MSA 서비스
├── apex_shared/        ← FlatBuffers 스키마 + 공유 C++ 라이브러리 (apex::shared)
├── apex_infra/         ← Docker, K8s 인프라 (Kafka/Redis/PG + Prometheus/Grafana)
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
- **의존성 (vcpkg)**: boost-asio, boost-beast (Phase 8a WebSocket용), flatbuffers, gtest

### MSVC 주의사항

- `std::aligned_alloc` 미지원 → `_aligned_malloc` / `_aligned_free` 분기 필요
- CRTP에서 `using FrameType = Derived::FrameType` → 불완전 타입 에러 (템플릿 파라미터로 우회)
- `MessageDispatcher`: `std::array<std::function, 65536>` ~2MB → 힙 할당 필수
- Windows TCP: ws2_32 + mswsock 링크, `_WIN32_WINNT=0x0A00`, IOCP 클라이언트 소켓은 별도 io_context

## 아키텍처 결정

- gRPC 제거 → 자체 프레임워크가 모든 네트워킹 담당
- 코루틴 프레임 할당: mimalloc/jemalloc + HALO (ADR-21) — 설계 결정, 미구현
- 서비스별 독립 vcpkg.json (Docker 독립 빌드용)
- 상세: `docs/apex_core/design-decisions.md`, `docs/apex_core/design-rationale.md` (ADR 23개)

## 워크플로우 규칙

### Git / 브랜치
- **초기 설정** (클론 후 1회): `git config core.hooksPath apex_tools/git-hooks`
- **main 직접 커밋 절대 금지** (pre-commit hook 강제) — feature/* 또는 bugfix/* 에서 작업
- **git worktree**: `.worktrees/` 하위에 생성, 에이전트별 독립 작업
  - 생성: `git worktree add .worktrees/<name> -b <branch-name>`
  - 삭제: `git worktree remove .worktrees/<name>`
- **머지**: 리뷰 이슈 0건 → squash merge → 브랜치+워크트리 삭제

### 문서
- **필수 작성**: 계획서(`plans/`), 체크포인트(`progress/`), 리뷰 보고서(`review/`)
- **문서 위치**: 프로젝트 전용 → `docs/<project>/`, 공통 → `docs/apex_common/`, 걸치는 문서 → 양쪽에 관점 조정하여 작성 (단순 복사 금지)
- 파일명: `YYYYMMDD_HHMMSS_<topic>.md` — 타임스탬프는 실제 작성 시간

### 코드 리뷰
- clangd LSP + superpowers:requesting-code-review 스킬 활용, 멀티 에이전트 병렬 리뷰
- **clangd LSP 효율 전략**: `documentSymbol` 병렬 → 핵심 API `hover` → 의심 패턴 `findReferences`/`incomingCalls`. 전수 분석 금지, 10분 타임아웃.
- **설계 문서 정합성**: 아키텍처 영향 변경 시 `Apex_Pipeline.md` 일치 확인 필수
- **리뷰 후**: 보고서 커밋 → 이슈 전량 수정 (병렬) → 재리뷰 (0건까지 반복)

### 브레인스토밍
- 1단계에서 반드시 `docs/Apex_Pipeline.md` 읽고 관련 섹션 식별, 설계 후 업데이트 포함

### 에이전트 작업
- **모든 작업은 에이전트 팀 병렬 실행** — 수정 가능 파일 목록 명시해서 충돌 방지
