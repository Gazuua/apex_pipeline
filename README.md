# Apex Pipeline

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.
자체 네트워크 프레임워크 위에 MSA 아키텍처 (Gateway → Kafka → Services → Redis/PostgreSQL) 를 구축하는 프로젝트.

## 현재 상태

### 완료

- **Phase 1~4.7 — 코어 프레임워크 기초**
  - Boost.Asio 코루틴 기반 TCP 서버/클라이언트
  - 커스텀 프레임 프로토콜 (Length-prefixed binary frame)
  - CoreEngine 멀티코어 아키텍처 (코어별 독립 io_context + 크로스코어 메시징)
  - FlatBuffers 직렬화, RingBuffer, MessageDispatcher
  - 서비스 레지스트리 + 파이프라인 체인

- **Phase 5 — 기반 정비** (PR #1 merged)
  - TOML 설정 시스템 (tomlplusplus)
  - spdlog 구조화 로깅
  - Graceful shutdown (시그널 핸들링 + drain 타임아웃)
  - GitHub Actions CI (GCC debug/asan/tsan + MSVC, 5개 잡 병렬)

- **auto-review v1.1 — 멀티에이전트 자동 리뷰 플러그인** (PR #2 merged)
  - 5개 전문 리뷰어 에이전트 (docs/structure/code/test/general) 병렬 리뷰
  - v1.1 개선: code/test 리뷰어에 2-tier superpowers:code-reviewer 연동, confidence ≥40 하향, Smart Re-review Skip
  - Phase 4 CI 실패 인프라/코드 분기 + 대규모 수정 시 재리뷰
  - Phase 5 자동 squash merge + 워크트리 정리
  - CI concurrency 설정 (동일 브랜치 이전 실행 자동 취소)
  - 리뷰 → 수정 → 재리뷰 → Clean(0건) → PR + CI + merge 전 과정 자동화

- **코드 품질 개선** (PR #4 merged)
  - TSAN flaky 테스트 수정 (`timeout_multiplier()` 도입, TSAN 환경 10x 자동 확대)
  - CoreEngine drain 루프 예외 보호 (try-catch)
  - CMakePresets 정비 (tsan TSAN_OPTIONS 중복 제거, asan-msvc test preset 추가)
  - auto-review 지침 개선 (full 모드 커밋 불필요, 자동화 원칙, CI watch 타임아웃)

- **auto-review v1.2 — task 모드 스마트 스킵** (PR #8 merged)
  - task 모드 Round 1에서 변경 파일 타입 기반 리뷰어 선택 디스패치
  - 파일타입 매핑 확장 (.fbs, Dockerfile, CI yml, suppressions 등 6개 항목 추가)
  - Phase 1/2 공용 매핑 테이블로 통합, 중복 제거

- **빌드 환경 개선** (PR #6 merged)
  - `.gitattributes` CRLF/LF 정규화, `VCPKG_INSTALLED_DIR` 공유 + `${hostSystemName}` 빌드 디렉토리 분리
  - 빌드 스크립트 리팩토링 (사전 체크 + 버전 검증)
  - CI Docker 이미지 (ubuntu:24.04 + GCC 14 + vcpkg 사전 설치, Linux 잡 apt-get/vcpkg 설치 완전 제거)
  - GitHub Actions CI 5개 잡 병렬 (linux-gcc/asan/tsan + windows-msvc + root-linux-gcc)
