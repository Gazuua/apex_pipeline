# FSD 백로그 소탕 — v0.5.10.5

**브랜치**: feature/fsd-backlog-20260322_145014
**일시**: 2026-03-22

---

## 해결 (2건)

### BACKLOG-102: GatewayPipeline 에러 흐름 단위 테스트
### BACKLOG-127: blacklist_fail_open fail-open/fail-close 분기 단위 테스트

GatewayPipeline을 template policy 패턴으로 리팩토링하여 compile-time dependency injection 지원:
- `GatewayPipelineBase<VerifierT, BlacklistT, LimiterT>` — 3개 template 파라미터
- 프로덕션: `using GatewayPipeline = GatewayPipelineBase<JwtVerifier, JwtBlacklist, RateLimitFacade>` (zero-cost)
- 테스트: `using TestPipeline = GatewayPipelineBase<MockVerifier, MockBlacklist, MockLimiter>`

헤더 분리:
- `gateway_pipeline.hpp` — template class만 (mock 테스트용, 프로덕션 의존성 없음)
- `gateway_pipeline_production.hpp` — concrete includes + using alias

신규 테스트 16건 (`test_gateway_pipeline_errors.cpp`):
- IP rate limit: 거부/허용/null limiter (3건)
- JWT auth: exempt skip/no token/invalid token/valid token (4건)
- User rate limit: 거부/Redis error fail-open (2건)
- Endpoint rate limit: 거부 (1건)
- Blacklist: blocked/allowed/fail-open/fail-close/non-sensitive skip/null (6건)

코루틴 테스트 하네스: `io_context::run()` + `co_spawn(detached)` + `restart()` 패턴.
MSVC 코루틴 제약: lambda coroutine 내 `ASSERT_*` 불가 (return → C3773) → `EXPECT_*` 사용.

## 드롭 (1건)

| 항목 | 사유 |
|------|------|
| BACKLOG-132 | 단순 shutdown 재배치(close→stop)는 UAF 위험 — detached 코루틴이 파괴된 multiplexer 멤버에 접근 가능. cancellation_signal 기반 명시적 취소 인프라 선행 필요 |

## 빌드 결과

- 83/83 테스트 전체 통과 (신규 1개 테스트 타겟 포함)
- Zero Warning (MSVC /W4 /WX)
