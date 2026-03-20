# Tier 3 아키텍처 정비 — 작업 결과 (v0.5.9.0)

**브랜치**: `feature/backlog-tier3-arch`
**기간**: 2026-03-20
**백로그**: BACKLOG-91, 89, 3, 66, 56, 90

---

## 작업 요약

Tier 3 아키텍처 이슈 6건(CRITICAL 2 + MAJOR 4)을 4개 Phase로 구현 완료.
코어 프레임워크의 타입 안전성, 모듈 경계, 서비스 캡슐화를 강화.

## 완료 항목

### Phase 1: SessionId 강타입화 (BACKLOG-91)
- `using SessionId = uint64_t` → `enum class SessionId : uint64_t {}`
- `make_session_id()`, `to_underlying()` 변환 헬퍼
- `std::hash<SessionId>`, `fmt::formatter<SessionId>` 특수화
- 커밋: `ff22aa0`

### Phase 2: core→shared 역방향 의존 해소 + FrameType concept (BACKLOG-89, 3)
- forwarding header 3개 제거 (wire_header, frame_codec, tcp_binary_protocol)
- KafkaMessageMeta 구조체 도입 → core에서 shared 타입 참조 제거
- FrameType concept + `payload()` accessor 도입
- 커밋: `ea83a9b`

### Phase 3: CoreEngine spawn_tracked + ServiceBase io_context 캡슐화 (BACKLOG-66, 56)
- `CoreEngine::spawn_tracked()` — 인프라 코루틴 추적 API
- `ServiceBase::post()` / `get_executor()` — io_context 캡슐화
- `KafkaAdapter::wire_services()` co_spawn(detached) → spawn_tracked 전환
- 커밋: `0344eda`

### Phase 4: ErrorCode 서비스 에러 분리 (BACKLOG-90)
- ErrorCode에서 Gateway 전용 에러(100-199) 제거
- `ErrorCode::ServiceError` (= 99) sentinel 도입
- `GatewayError` enum + `GatewayResult` 타입 (`std::expected<void, GatewayError>`)
- `AuthError` enum 도입
- ErrorResponse FlatBuffers `service_error_code` 필드 추가
- `GatewayPipeline::process()` — 에러 프레임 직접 전송 + ok() 반환 패턴
- 커밋: `0ad23d8`

### Phase 5: 문서 갱신
- `apex_core_guide.md`: ErrorCode 체계, ServiceError sentinel, post()/get_executor(), spawn_tracked(), SessionId 강타입
- `Apex_Pipeline.md`: v0.5.9.0 버전 기록 추가
- `BACKLOG.md` → `BACKLOG_HISTORY.md`: 6건 이전
- `CLAUDE.md`, `README.md`: 로드맵 버전 갱신

## 빌드 검증

- MSVC `/W4 /WX`: 0 경고, 빌드 성공
- 테스트: 71/71 통과, 0 실패
- 변경 파일: 20+ (core 5, gateway 12, auth-svc 2, docs 6)
