# v0.6.1 Prometheus 메트릭 노출 완료

**일자**: 2026-03-24
**브랜치**: feature/prometheus-metrics
**버전**: v0.6.1.0
**백로그**: BACKLOG-175

## 작업 요약

Boost.Beast 기반 HTTP `/metrics` 엔드포인트를 Server의 control_io_에서 구동하여 Prometheus scrape를 지원한다. MetricsRegistry가 Counter/Gauge를 관리하고, 프레임워크/어댑터 17개 핵심 메트릭을 자동 수집한다. 서비스는 등록 API를 통해 자체 메트릭을 추가할 수 있다.

## 산출물

### 신규 파일 (4개)
- `apex_core/include/apex/core/metrics_registry.hpp` — Counter, Gauge, MetricsRegistry (3종 패턴: 소유/참조/콜백)
- `apex_core/src/metrics_registry.cpp` — Prometheus text exposition format 직렬화
- `apex_core/include/apex/core/metrics_http_server.hpp` — Beast HTTP 서버
- `apex_core/src/metrics_http_server.cpp` — /metrics, /health, /ready 핸들러

### 수정 파일 (23개)
- **Config**: MetricsConfig 구조체 (server_config.hpp), TOML [metrics] 파싱 (config.cpp)
- **AdapterInterface**: register_metrics() 가상 메서드 + AdapterWrapper 위임
- **Server**: MetricsRegistry 소유, HTTP 서버 조건부 기동/셧다운, adapter->register_metrics() 호출
- **프레임워크**: SessionManager (생성/타임아웃/힙폴백), MessageDispatcher (디스패치/예외) atomic 카운터
- **Kafka**: Producer (produce/error), Consumer (consume/DLQ) atomic + KafkaAdapter register_metrics
- **Redis**: Multiplexer (commands) atomic + RedisAdapter register_metrics (connected gauge_fn)
- **PG**: Pool (queries/record_query) + PgAdapter register_metrics (pool_active gauge_fn)
- **인프라**: prometheus.yml scrape target 3개 활성화

### 테스트
- test_metrics_registry: 8개 단위 테스트 (Counter, Gauge, serialize, labels, counter_from, gauge_fn)
- 전체 86/86 테스트 통과

## 수치
- 메트릭: 프레임워크 8 + 어댑터 9 = 17개
- 엔드포인트: /metrics, /health, /ready
- TOML 설정: [metrics] enabled, port
