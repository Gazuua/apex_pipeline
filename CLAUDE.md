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
│   ├── bin/{variant}/  ← 빌드 출력 (debug/, release/)
│   └── build.bat / build.sh / CMakePresets.json
├── docs/               ← 전체 프로젝트 문서 (중앙 집중)
│   ├── Apex_Pipeline.md  ← 마스터 설계서
│   ├── apex_common/      ← 공통 (plans/progress/review)
│   └── apex_core/ apex_infra/ apex_shared/ apex_tools/
├── apex_services/      ← MSA 서비스
├── apex_shared/        ← 공유 C++ 라이브러리 + 외부 어댑터
│   ├── lib/adapters/     ← 외부 어댑터 (common, kafka, redis, pg)
│   └── tests/            ← unit/ + integration/ 테스트
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
- **빌드 변형**: `APEX_BUILD_VARIANT` = release / debug / asan / tsan
- **출력**: `apex_core/bin/{variant}/{target}.exe` (예: `bin/debug/echo_server.exe`, `bin/release/bench_mpsc_queue.exe`)
- **compile_commands.json**: configure 후 빌드 스크립트에서 루트로 복사 (symlink 아님)
- **의존성 (vcpkg)**: benchmark, boost-asio, boost-unordered, flatbuffers, gtest, hiredis, libpq, librdkafka, redis-plus-plus, spdlog, tomlplusplus — 향후: boost-beast (v0.5.1.0)

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

- **버전 체계**: `v[메이저].[대].[중].[소]` — 메이저 0=개발중, 1=프레임워크 완성. 대=도메인 전환, 중=마일스톤, 소=수정/리뷰
- **현재**: v0.4.4.0 (외부 어댑터 완료)
- **다음**: v0.4 (외부 어댑터) → v0.5 (서비스 체인) → v0.6 (운영 인프라) → v1.0.0.0 (프레임워크 완성)
- **v1.0.0.0 이후**: v1.1+ (게임 레퍼런스 — 게임 서비스 + Android 클라이언트 + AWS)
- 상세: `docs/Apex_Pipeline.md` §10

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
- **구조/실행 가이드**: `apex_core/benchmarks/README.md` 참조 (벤치마크 목록, 실행 방법, JSON 출력 옵션)
- **순차 실행 필수** — 병렬 실행 시 CPU 경합으로 결과 오염. 절대 동시에 돌리지 않는다
- **백그라운드 서브에이전트 1개**에서 11개를 순차 실행 (Bash 타임아웃 회피)
- **Release 빌드**(`release` 프리셋)로 측정 — Debug는 참고용
- 결과 JSON 저장: `apex_core/benchmark_results/`

### 벤치마크 보고서 생성
- **도구 가이드**: `apex_tools/benchmark/report/README.md` 참조
- **템플릿/분석 분리 구조**:
  - `apex_tools/benchmark/report/generate_benchmark_report.py` — 레이아웃/디자인/차트 템플릿 (**수정하지 않는다**)
  - `apex_core/benchmark_results/analysis.json` — 섹션별 분석 텍스트 (매번 새로 작성)
  - `apex_core/benchmark_results/{release,debug}/*.json` — 벤치마크 데이터
- **에이전트 워크플로우**:
  1. Release/Debug 벤치마크 11개 순차 실행 → JSON 저장
  2. 결과 데이터를 분석하여 `analysis.json` 작성 (8개 섹션)
  3. 스크립트 실행: `python generate_benchmark_report.py --release=... --debug=... --analysis=... --output=...`
- **analysis.json 작성 규칙**:
  - 한글 베이스 + 영어 기술 용어 혼용, HTML 태그 사용 (`<b>`, `<br/>`)
  - 각 섹션 3~10줄 — 핵심 수치 강조 + 아키텍처 맥락에서의 의미 해석
  - 8개 키: `mpsc_queue`, `ring_buffer`, `frame_codec`, `dispatcher`, `slab_pool`, `timing_session`, `integration`, `overview`

### 에이전트 작업
- **모든 작업은 에이전트 팀 병렬 실행** — 수정 가능 파일 목록 명시해서 충돌 방지

## 프로젝트 정보

- GitHub: `Gazuua/apex_pipeline`

## CI/CD 트러블슈팅

- **TSAN**: Boost.Asio false positive → `tsan_suppressions.txt` (루트+apex_core 양쪽 배치). `race:boost::asio::detail::*` (atomic_thread_fence) + `mutex:boost::asio::detail::posix_mutex` (io_context 소멸 시)
- **ASAN/LSAN**: spdlog 글로벌 레지스트리 leak → `lsan_suppressions.txt`
- **ASAN aligned_alloc**: size는 alignment 배수여야 함. `max(capacity, alignment)`로 보정
- **CMakePresets ${sourceDir}**: 루트+하위 양쪽에 suppressions 파일 배치 (include 시 `${sourceDir}` 변환 대응)
- **[[nodiscard]]**: GCC에서 EXPECT_THROW 내 반환값 경고 → `(void)` 캐스트
- **test preset**: TSAN_OPTIONS/LSAN_OPTIONS는 configure preset이 아닌 **test preset**에 설정
- **CI workflow**: `ctest --preset <name>` 사용 (--test-dir 대신)
- **vcpkg 다운로드 실패**: GitHub CDN 간헐적 HTTP 502 → `gh run rerun --failed`
- **CI path filter**: 소스(`.cpp/.hpp`, `CMakeLists.txt`, `vcpkg.json` 등) 미변경 시 빌드/테스트 자동 스킵 — 문서/도구만 변경된 커밋은 CI 불필요
- **CI 실패 분석 원칙**: 한 잡만 보고 판단하지 말고 **모든 실패 잡의 로그를 확인** — 잡마다 실패 원인이 다를 수 있음
- **CI 대기**: `gh run watch`는 **반드시 백그라운드(`run_in_background`)로 실행** — 타임아웃 제한 없이 완료까지 대기
- **GCC `SIZE_MAX`**: `<cstdint>` include 필수. MSVC는 transitively include되어 빌드되지만, GCC에서 직접 include하면 `'SIZE_MAX' was not declared` 에러
