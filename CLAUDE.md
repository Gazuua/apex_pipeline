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
│   ├── examples/                 ← echo_server, chat_server
│   ├── schemas/                  ← FlatBuffers 스키마
│   ├── docs/                     ← 설계 문서, ADR
│   ├── bin/                      ← 빌드 출력
│   ├── build.bat / build.sh
│   └── CMakePresets.json
├── apex_docs/                    ← 전체 프로젝트 문서
│   ├── plans/                    ← 구현 계획서
│   ├── progress/                 ← 체크포인트
│   └── review/                   ← 코드 리뷰 보고서
├── apex_services/                ← MSA 서비스
├── apex_shared/                  ← FlatBuffers 스키마 + 공유 코드
├── apex_infra/                   ← Docker, K8s 인프라
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

boost-asio, boost-beast, flatbuffers, gtest, spdlog, fmt, tomlplusplus, benchmark

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
- 코루틴 프레임 할당: mimalloc/jemalloc + HALO (ADR-21)
- 서비스별 독립 vcpkg.json (Docker 독립 빌드용)
- `apex_` prefix로 모든 프로젝트 디렉토리 통일
- 설계 문서: `apex_core/docs/design-decisions.md`, `apex_core/docs/design-rationale.md` (ADR 23개)

## 워크플로우 규칙

### 브랜치 전략
- **작업별 feature branch 분리** — main에 직접 커밋하지 않음
- 병렬 세션에서 각각 독립 브랜치로 동시 작업 가능
- 완료 후 main에 머지 (fast-forward 또는 PR)

### 문서
- **필수 작성**: 계획서(`plans/`), 체크포인트(`progress/`), 리뷰 보고서(`review/`)
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
- **리뷰 후 워크플로우** (이슈 0건 될 때까지 반복):
  1. 리뷰 보고서 즉시 커밋
  2. 발견된 이슈 전량 수정 (에이전트 병렬 작업)
  3. 재리뷰 → 설계 결정 필요 시만 사용자 호출, 명확한 수정은 즉시 진행

### 에이전트 작업
- **모든 작업은 에이전트 팀 병렬 실행** — 작업 분리 가능한 범위 내에서
- 병렬 작업 시 수정 가능 파일 목록을 명시해서 충돌 방지
