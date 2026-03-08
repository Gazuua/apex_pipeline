# Apex Pipeline — 프로젝트 가이드

## 개요

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.
자체 네트워크 프레임워크 + MSA 아키텍처 (Gateway → Kafka → Services → Redis/PostgreSQL).

## 모노레포 구조

```
D:\.workspace/
├── apex_core/                    ← 코어 프레임워크 (C++23, Boost.Asio 코루틴)
│   ├── include/apex/core/        ← 헤더 (ProtocolBase, ServiceBase, Server 등)
│   ├── src/                      ← 구현체
│   ├── tests/                    ← unit/ + integration/
│   ├── examples/                 ← echo_server, chat_server, multicore_echo_server
│   ├── schemas/                  ← FlatBuffers 스키마
│   ├── bin/                      ← 빌드 출력
│   ├── build.bat / build.sh
│   └── CMakePresets.json
├── docs/                         ← 전체 프로젝트 문서 (중앙 집중)
│   ├── Apex_Pipeline.md          ← 마스터 설계서
│   ├── apex_common/              ← 프로젝트 공통 (plans/progress/review)
│   ├── apex_core/                ← 코어 프레임워크 문서
│   ├── apex_infra/               ← 인프라 문서
│   └── apex_shared/              ← 공유 라이브러리 문서
├── apex_services/                ← MSA 서비스
├── apex_shared/                  ← FlatBuffers 스키마 + 공유 C++ 라이브러리 (apex::shared)
│   ├── schemas/                  ← 공유 FlatBuffers 스키마 (.fbs)
│   ├── lib/include/apex/shared/  ← 공유 헤더
│   ├── lib/src/                  ← 구현
│   └── CMakeLists.txt            ← FlatBuffers 코드젠 + STATIC 라이브러리
├── apex_infra/                   ← Docker, K8s 인프라
│   ├── docker-compose.yml        ← Kafka/Redis/PG (기본) + Prometheus/Grafana (observability)
│   ├── postgres/init.sql         ← 서비스별 스키마 초기화
│   ├── prometheus/               ← Prometheus 설정
│   └── grafana/provisioning/     ← Grafana 데이터소스 자동 프로비저닝
└── apex_tools/                   ← CLI, 스크립트
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

### 의존성 (vcpkg)

boost-asio, boost-beast, flatbuffers, gtest, spdlog, tomlplusplus, benchmark

### MSVC 주의사항

- `std::aligned_alloc` 미지원 → `_aligned_malloc` / `_aligned_free` 분기 필요
- CRTP에서 `using FrameType = Derived::FrameType` → 불완전 타입 에러 (템플릿 파라미터로 우회)
- `MessageDispatcher`: `std::array<std::function, 65536>` ~2MB → 힙 할당 필수
- Windows TCP: ws2_32 + mswsock 링크, `_WIN32_WINNT=0x0A00`, IOCP 클라이언트 소켓은 별도 io_context

## apex_core 컴포넌트

### 기반
- `MpscQueue<T>` — 락프리 bounded MPSC 큐 (header-only)
- `SlabPool` / `TypedSlabPool<T>` — O(1) 슬랩 메모리 풀
- `RingBuffer` — zero-copy 수신 버퍼 (linearize 지원)
- `TimingWheel` — O(1) 타임아웃 관리

### 프레임워크
- `CoreEngine` — io_context-per-core + MPSC 코어 간 메시징
- `MessageDispatcher` — O(1) 디스패치, 코루틴 핸들러 (`awaitable<Result<void>>`)
- `ServiceBase<T>` — CRTP 서비스 베이스 (handle/route 등록)
- `WireHeader` — 10바이트 고정 헤더 (big-endian)
- `FrameCodec` — RingBuffer 기반 프레임 코덱

### 프로토콜
- `ProtocolBase<Derived>` — CRTP 프로토콜 추상화 (try_decode / consume_frame)
- `TcpBinaryProtocol` — FrameCodec 래핑 구현체

### 네트워크/세션
- `Session` — TCP 세션 (Connected → Active → Closed)
- `SessionManager` — 세션 관리 + TimingWheel 하트비트
- `TcpAcceptor` — 코루틴 accept_loop (IPv4/IPv6)
- `Server` — 최상위 통합 클래스 (ServerConfig 구조체)

### 에러 처리
- `Result<T>` = `std::expected<T, ErrorCode>`
- `ErrorSender` — WireHeader + FlatBuffers ErrorResponse 프레임 빌더
- 에러 전파: 핸들러 `co_return error(code)` → Server가 ErrorResponse 전송

## 아키텍처 결정

- gRPC 제거 → 자체 프레임워크가 모든 네트워킹 담당
- MSA: Gateway(WS/HTTP) → Kafka(중앙 버스) → Services → Redis/PostgreSQL
- 코루틴 프레임 할당: mimalloc/jemalloc + HALO (ADR-21) (설계 결정, 미구현 — 기본 allocator 사용 중)
- 서비스별 독립 vcpkg.json (Docker 독립 빌드용)
- `apex_` prefix로 모든 프로젝트 디렉토리 통일
- 설계 문서: `docs/apex_core/design-decisions.md`, `docs/apex_core/design-rationale.md` (ADR 23개)

## 워크플로우 규칙

### 브랜치 전략
- **main 직접 커밋 절대 금지** — 모든 작업은 feature/bugfix 브랜치에서 진행
- **브랜치 생성 기준**: 계획서가 필요한 수준의 신규 기능 → `feature/*`, 버그/개선 → `bugfix/*`
- **git worktree로 병렬 작업**: `.worktrees/` 하위에 워크트리 생성, 에이전트별 독립 작업
  ```
  .workspace (main, 읽기 전용 레퍼런스)
  └── .worktrees/
      ├── feature_docker/       ← feature/docker-compose 브랜치
      └── feature_schema/       ← feature/shared-schema 브랜치
  ```
  - 생성: `git worktree add .worktrees/<name> -b <branch-name>`
  - 삭제: `git worktree remove .worktrees/<name>`
- **머지 조건**: 반복 코드 리뷰에서 이슈 0건 Clean 통과
- **머지 전략**: squash merge — feature 브랜치 커밋은 main에 하나의 깨끗한 커밋으로 합침
- **머지 후**: feature 브랜치 삭제 + 워크트리 정리

### 문서
- **필수 작성**: 계획서(`plans/`), 체크포인트(`progress/`), 리뷰 보고서(`review/`)
- **문서 위치**: 프로젝트 전용 → `docs/<project>/`, 공통 → `docs/apex_common/`, 걸치는 문서 → 관련 프로젝트 양쪽에 복사
- 파일명: `YYYYMMDD_HHMMSS_<topic>.md` — 타임스탬프는 실제 파일 작성 시간으로 정확히 맞출 것

### 코드 리뷰
- clangd LSP와 superpowers:requesting-code-review 스킬을 함께 활용할 것
- 멀티 에이전트 병렬 리뷰 (LSP / 코어 / 네트워크 / 테스트)
- **clangd LSP 효율 전략** (호출 비용이 크므로):
  1. **Phase 1**: `documentSymbol`을 여러 파일에 병렬 호출하여 심볼 맵 빠르게 구축
  2. **Phase 2**: 핵심 클래스 public API에 `hover`로 타입 확인 (클래스당 2-3개 핵심 메서드만)
  3. **Phase 3**: 의심 패턴(raw pointer 캡처, shared_ptr, 코루틴 수명 등)에 `findReferences`/`incomingCalls`로 추적
  - 모든 심볼 전수 분석 금지 — 핵심 위주 분석, 병렬 호출 적극 활용
  - 10분 타임아웃 기준, 초과 시 타겟팅 방식으로 전환
- **설계 문서 정합성 검토**: 아키텍처/컴포넌트/기술 스택/로드맵에 영향을 주는 변경 시 `Apex_Pipeline.md`와 실제 구현의 일치 여부를 반드시 확인. 불일치 시 문서 또는 코드를 수정하여 정합성 확보. (테스트만 추가하는 변경은 스킵 가능, 리팩토링은 규모에 따라 판단)
- **리뷰 후 워크플로우** (이슈 0건 될 때까지 반복):
  1. 리뷰 보고서 즉시 커밋
  2. 발견된 이슈 전량 수정 (에이전트 병렬 작업)
  3. 재리뷰 → 설계 결정 필요 시만 사용자 호출, 명확한 수정은 즉시 진행

### 브레인스토밍
- **Apex_Pipeline.md 필수 참조**: 브레인스토밍 1단계(컨텍스트 탐색)에서 반드시 `docs/Apex_Pipeline.md`를 읽고 관련 섹션 식별. 설계 완료 후 해당 섹션의 업데이트 내용을 설계 문서에 포함.

### 에이전트 작업
- **모든 작업은 에이전트 팀 병렬 실행** — 작업 분리 가능한 범위 내에서
- 병렬 작업 시 수정 가능 파일 목록을 명시해서 충돌 방지
