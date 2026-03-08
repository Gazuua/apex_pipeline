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

- **auto-review v1.1 — 멀티에이전트 자동 리뷰 플러그인** (PR #2 CI 대기)
  - 5개 전문 리뷰어 에이전트 (docs/structure/code/test/general) 병렬 리뷰
  - v1.1 개선: code/test 리뷰어에 2-tier superpowers:code-reviewer 연동, confidence ≥40 하향, Smart Re-review Skip
  - 리뷰 → 수정 → 재리뷰 → Clean(0건) → PR + CI 전 과정 자동화

