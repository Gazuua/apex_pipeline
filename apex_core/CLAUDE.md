# apex_core — 빌드 & 개발 가이드

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

## MSVC 주의사항

- `std::aligned_alloc` 미지원 → `_aligned_malloc` / `_aligned_free` 분기 필요
- CRTP에서 `using FrameType = Derived::FrameType` → 불완전 타입 에러 (템플릿 파라미터로 우회)
- `MessageDispatcher`: `boost::unordered_flat_map` 기반 (기존 `std::array<std::function, 65536>` ~2MB 이슈 해결됨)
- Windows TCP: ws2_32 + mswsock 링크, `_WIN32_WINNT=0x0A00`, IOCP 클라이언트 소켓은 별도 io_context

## 아키텍처 결정

- gRPC 제거 → 자체 프레임워크가 모든 네트워킹 담당
- 코루틴 프레임 할당: mimalloc/jemalloc + HALO (ADR-21) — 설계 결정, 미구현
- 서비스별 독립 vcpkg.json (Docker 독립 빌드용)
- 상세: `docs/apex_core/design-decisions.md`, `docs/apex_core/design-rationale.md` (ADR 23개)

## 벤치마크 실행

- **구조/실행 가이드**: `apex_core/benchmarks/README.md` 참조 (벤치마크 목록, 실행 방법, JSON 출력 옵션)
- **순차 실행 필수** — 병렬 실행 시 CPU 경합으로 결과 오염. 절대 동시에 돌리지 않는다
- **백그라운드 서브에이전트 1개**에서 11개를 순차 실행 (Bash 타임아웃 회피)
- **Release 빌드**(`release` 프리셋)로 측정 — Debug는 참고용
- 결과 JSON 저장: `apex_core/benchmark_results/`

## 벤치마크 보고서 생성

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
