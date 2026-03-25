# 완료: CORE+SHARED 유닛 테스트 커버리지 보강

- **PR**: #146
- **백로그**: BACKLOG-148 (핵심 경로 9건), BACKLOG-191 (MetricsHttpServer), BACKLOG-192 (어댑터 실패 복구)
- **브랜치**: `feature/test-coverage-core-shared`

## 작업 결과

### 테스트 추가 (22건)

| 카테고리 | 파일 | 케이스 수 |
|----------|------|-----------|
| 동시성 | test_circuit_breaker.cpp (확장) | 2 |
| 동시성 | test_session.cpp (확장) | 2 |
| 동시성 | test_redis_multiplexer_close.cpp (신규) | 2 |
| 에러/엣지 | test_cross_core_call.cpp (확장) | 1 |
| 에러/엣지 | test_timing_wheel.cpp (확장) | 2 |
| 에러/엣지 | test_frame_codec.cpp (확장) | 1 |
| 에러/엣지 | test_config.cpp (확장) | 1 |
| 에러/엣지 | test_spsc_mesh.cpp (확장) | 1 |
| 에러/엣지 | test_adapter_base_failure.cpp (신규) | 4 |
| 통합 | test_metrics_http_server.cpp (신규) | 6 |

### 소스 변경 (2건)

1. **AdapterBase::init()** — do_init() 예외 시 state + 인프라(tokens_, io_ctxs_, base_engine_)를 CLOSED로 완전 롤백
2. **MetricsHttpServer** — `local_port()` const 접근자 추가 (port 0 테스트 지원)

### auto-review 결과

- HIGH 1건 수정: init 롤백 시 인프라 벡터 미정리 → 이중 채움 방지
- MEDIUM 1건: close 5초 타임아웃 하드코딩 → BACKLOG-201 등록
- systems 검증: 동시성/메모리 안전성 전체 확인

### 빌드 + 테스트

- 로컬: 89/89 통과 (debug)
- CI: Linux GCC + MSVC 통과
