# Apex Pipeline

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.
자체 네트워크 프레임워크 위에 MSA 아키텍처 (Gateway → Kafka → Services → Redis/PostgreSQL) 를 구축하는 프로젝트.

## 현재 상태 — v0.3.0.0

### 완료

- **v0.1 — 코어 프레임워크 기초**
  - Boost.Asio 코루틴 기반 TCP 서버/클라이언트
  - 커스텀 프레임 프로토콜 (Length-prefixed binary frame)
  - CoreEngine 멀티코어 아키텍처 (코어별 독립 io_context + 크로스코어 메시징)
  - FlatBuffers 직렬화, RingBuffer, MessageDispatcher
  - 서비스 레지스트리 + 파이프라인 체인

- **v0.2 — 개발 인프라** (PR #1 merged)
  - TOML 설정 시스템 (tomlplusplus)
  - spdlog 구조화 로깅
  - Graceful shutdown (시그널 핸들링 + drain 타임아웃)
  - GitHub Actions CI (GCC debug/asan/tsan + MSVC, 5개 잡 병렬)
  - 빌드 환경 개선 (.gitattributes, Docker CI, 사전 체크 공용 헬퍼)

- **v0.3 — 코어 성능 완성** (PR #10 merged)
  - Tier 0: Google Benchmark 벤치마크 인프라 (micro 7개 + integration 4개)
  - Tier 0.5: 에러 타입 통일 (DispatchError/QueueError → ErrorCode 단일 채널, Result<T> 도입)
  - Tier 1: drain/tick 분리, Cross-Core Message Passing 인프라, MessageDispatcher unordered_flat_map, zero-copy dispatch
  - Tier 1.5: E2E echo 부하 테스터 + JSON before/after 비교 도구
  - Tier 2: intrusive_ptr 전환, Session SlabPool 할당, sessions_/timer_to_session_ unordered_flat_map
  - Tier 2.5: 벤치마크 시각화 (matplotlib) + PDF 보고서 (ReportLab) 파이프라인
  - Tier 3: Server/ConnectionHandler 분리, SO_REUSEPORT per-core acceptor, io_uring CMake 옵션, SlabPool auto-grow

### Known Issues

- **Debug 벤치마크 CRT mismatch crash** — vcpkg `benchmark.dll`이 release/debug 동일 파일명이라, `bin/` 공유 디렉토리에서 release DLL이 debug 바이너리에 로드됨 → exit code 3 크래시. spdlog/fmt는 `d` suffix(`spdlogd.dll`)로 구분되나 Google Benchmark는 미지원. 다음 세션에서 벤치마크 빌드 안정화 태스크로 수정 예정.

### 다음

- **v0.4 — 외부 어댑터** (Kafka + Redis + PostgreSQL + Connection Pool)
- **v0.5 — 서비스 체인** (WebSocket + Gateway + Auth + E2E)
- **v0.6 — 운영 인프라** (Prometheus + Docker + K8s + CI/CD)
- **v1.0.0.0 — 프레임워크 완성**

> 상세 로드맵: `docs/Apex_Pipeline.md` §10
