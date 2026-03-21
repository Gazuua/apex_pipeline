# apex_core — 빌드 & 개발 가이드

## 빌드 환경

- **OS/컴파일러**: Windows 10 Pro, VS2022 Community (MSVC 19.44), C++23
- **빌드 시스템**: CMake + Ninja, vcpkg (`C:\Users\JHG\vcpkg`)
- **빌드 변형**: `APEX_BUILD_VARIANT` = release / debug / asan / tsan
- **출력**: `apex_core/bin/{variant}/{target}.exe` (예: `bin/debug/echo_server.exe`, `bin/release/bench_mpsc_queue.exe`)
- **compile_commands.json**: configure 후 빌드 스크립트에서 루트로 복사 (symlink 아님)


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
- **queue-lock.sh 경유 필수** — `queue-lock.sh benchmark <exe> [args]`로 실행. build 채널 lock을 공유하여 빌드/벤치마크 상호배제 보장. 직접 실행은 PreToolUse hook이 차단
- **Release 빌드**(`release` 프리셋)로 측정 — 총 14개 벤치마크 실행 파일
- 결과 JSON 저장: `apex_core/benchmark_results/` (임시) → `docs/apex_core/benchmark/{version}/` (git 추적)

## 벤치마크 보고서 생성

- **도구 가이드**: `apex_tools/benchmark/report/README.md` 참조
- **버전 비교 + 방법론 비교 체계**:
  - `apex_tools/benchmark/report/generate_benchmark_report.py` — 보고서 생성 스크립트
  - `docs/apex_core/benchmark/analysis.json` — 섹션별 분석 텍스트 (매번 새로 작성)
  - `docs/apex_core/benchmark/{version}/*.json` — 벤치마크 데이터 (버전별 디렉토리)
- **에이전트 워크플로우**:
  1. Release 빌드 후 벤치마크 14개 순차 실행 (`queue-lock.sh benchmark` 경유) → JSON 저장
  2. `docs/apex_core/benchmark/{version}/`으로 복사 + `metadata.json` 생성
  3. 결과 데이터를 분석하여 `analysis.json` 작성 (11개 섹션)
  4. 스크립트 실행: `python generate_benchmark_report.py --data-dir=docs/apex_core/benchmark --baseline=v0.5.9.0 --current=v0.5.10.0 --analysis=... --output=...`
  5. baseline 생략 시 단독 보고서 (비교 없이 절대값만)
- **analysis.json 작성 규칙**:
  - 한글 베이스 + 영어 기술 용어 혼용, HTML 태그 사용 (`<b>`, `<br/>`)
  - 각 섹션 3~10줄 — 핵심 수치 강조 + 아키텍처 맥락에서의 의미 해석
  - 11개 키: `queue`, `allocators`, `frame_codec`, `serialization`, `dispatcher`, `session_timer`, `ring_buffer`, `integration`, `overview`, `version_summary`, `methodology_summary`
