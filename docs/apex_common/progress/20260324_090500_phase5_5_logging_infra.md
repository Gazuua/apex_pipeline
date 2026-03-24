# Phase 5.5 — ScopedLogger 로깅 인프라 (v0.5.11.0)

- **백로그**: BACKLOG-161 (ScopedLogger), BACKLOG-174 (로그 충실화)
- **브랜치**: `feature/phase5.5-logging-infra`
- **상태**: 완료

## 작업 결과

### 1. ScopedLogger 클래스 구현
- `apex_core/include/apex/core/scoped_logger.hpp` + `apex_core/src/scoped_logger.cpp`
- `std::source_location` 자동 캡처 (파일:라인 + 함수명 — short_func 파싱으로 `Class::method` 형태)
- compile-time format string 검증 (`consteval log_loc` + `type_identity_t` alias)
- 6레벨 × 5오버로드 매크로 코드 생성 (`APEX_SCOPED_LOG_`, 즉시 `#undef`)
- `HasSessionId` concept — SessionPtr 구조적 타이핑으로 세션 헤더 의존 방지
- `with_trace(corr_id)` — copy semantics로 요청 추적 (원본 불변)
- `NO_CORE` sentinel — per-core 컨텍스트 밖에서 사용
- 단위 테스트 10건 (`test_scoped_logger.cpp`)

### 2. 레거시 폐기
- `log_helpers.hpp` 삭제
- `APEX_SVC_LOG_METHODS` 매크로 삭제
- `log_trace/debug/info/warn/error()` 자유 함수 0건 확인

### 3. 전 레이어 마이그레이션
- **core** (12파일): CoreEngine, Server, Session, SessionManager, MessageDispatcher, 할당자 3종, FrameCodec, WireHeader 등
- **services** (8파일): Gateway, Auth, Chat 서비스 + main.cpp 3개
- **shared** (15파일): Kafka/PG/Redis/Common 어댑터 전체

### 4. 로그 충실화
- 이전에 로그 없던 경로에 적절한 수준의 로그 추가
- trace: 내부 진행 (버퍼 크기, hash 결과, enqueue 크기 등)
- debug: 분기 결정 (pipeline 거부, 토큰 발급, corr_id 매핑)
- info: 주요 이벤트 (핸들러 진입, 로그인 성공, 서비스 시작)
- warn: 복구 가능 실패 (Redis 에러, rate limit)
- error: 복구 불가 실패 (PG 쿼리 실패, 토큰 생성 실패)

### 5. Auto-Review 수정
- **CRITICAL**: namespace-scope static ScopedLogger 19건 → function-scope static 전환 (init_logging() 전 생성 no-op 버그)
- **MEDIUM**: core_id_ 기본값 0→NO_CORE, Gateway 구조화 태그 overload 통일
- **LOW**: 주석 오버로드 개수 수정, mutable 제거

## 빌드 결과
- 85/85 유닛 테스트 100% 통과
- spdlog 직접 호출 0건 (인프라 제외)
- clang-format 적용 완료
