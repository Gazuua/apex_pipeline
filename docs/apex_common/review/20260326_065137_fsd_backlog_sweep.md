# Auto-Review: FSD 백로그 소탕

- **일시**: 2026-03-26
- **브랜치**: feature/fsd-backlog-20260326_054351
- **PR**: #175
- **리뷰어**: logic, test, systems+design (3명)

## 리뷰 대상 (11파일, +425/-45)

| 백로그 | 파일 | 변경 유형 |
|--------|------|-----------|
| BACKLOG-207 | gateway_service.cpp | BUG: silent failure 에러 응답 추가 |
| BACKLOG-209 | pubsub_listener.cpp | BUG: SUBSCRIBE/UNSUBSCRIBE 응답 소비 |
| BACKLOG-210 | server_config.hpp | SECURITY: max_connections 기본값 10000 |
| BACKLOG-213 | service_registry.hpp, test_service_registry.cpp | DESIGN_DEBT: for_each/size map_ 기반 |
| BACKLOG-228 | test_cross_core_call.cpp | TEST: void 타임아웃 테스트 |
| BACKLOG-229 | test_shared_payload.cpp | TEST: 혼합 refcount 테스트 |
| BACKLOG-219 | test_metrics_http_server.cpp | TEST: malformed HTTP 테스트 |
| BACKLOG-220 | test_timing_wheel.cpp | TEST: 콜백 exception 테스트 |
| BACKLOG-201 | adapter_base.hpp | DESIGN_DEBT: close timeout 파라미터화 |
| BACKLOG-225 | client.go | DESIGN_DEBT: sleep 파라미터화 |

## 수정 2건

### 1. handle_unsubscribe_channel 에러 응답 누락 (logic)
- **파일**: gateway_service.cpp
- **이슈**: handle_authenticate_session, handle_subscribe_channel에는 에러 응답이 추가됐으나 handle_unsubscribe_channel에는 누락
- **수정**: 동일 에러 응답 패턴 적용

### 2. register_ref 핵심 계약 미테스트 (test)
- **파일**: test_service_registry.cpp
- **이슈**: register_ref() 후 get<T>()/find<T>() 조회 가능성 미검증 — size/for_each만 테스트
- **수정**: RefRegisteredRetrievableByGetAndFind 테스트 추가

## 보고 1건 (문서)

### apex_core_guide.md ServerConfig 기본값 불일치
- **파일**: docs/apex_core/apex_core_guide.md:187
- **이슈**: max_connections=0 기재, 실제 코드는 10000
- **처리**: 문서 갱신 단계에서 수정

## 이슈 없음 확인

| 리뷰어 | 결과 |
|--------|------|
| logic | 1건 수정, 1건 보고 |
| test | 1건 수정, 나머지 4파일 PASS |
| systems+design | 전체 PASS (스레드 안전성, CRTP 조화, Go 컨벤션 모두 적합) |
