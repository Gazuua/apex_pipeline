# v0.6.1 Prometheus 메트릭 노출 — 설계 스펙

**일자**: 2026-03-24
**상태**: 승인됨
**백로그**: BACKLOG-175
**범위**: 프레임워크/어댑터 메트릭 풀 구현 + 서비스 등록 API + HTTP /metrics 엔드포인트

---

## 1. 배경 및 목적

v0.5.11.0에서 ScopedLogger 로깅 인프라가 완성되었고, v0.6 운영 인프라의 첫 단계로 Prometheus 메트릭 노출을 구현한다.

**목표**: 모든 서비스가 `/metrics` HTTP 엔드포인트를 노출하여 Prometheus가 scrape할 수 있도록 하고, 프레임워크/어댑터 레벨의 핵심 운영 메트릭을 자동 수집한다.

**비목표** (v0.6.1 범위 밖):
- Grafana 대시보드 프로비저닝
- Histogram 타입 메트릭 (API만 예약)
- 서비스별 비즈니스 메트릭 구현 (등록 API만 제공)

## 2. 설계 결정

| 결정 | 선택 | 근거 |
|------|------|------|
| HTTP 구현 | Boost.Beast 직접 구현 | prometheus-cpp 내장 HTTP 서버는 별도 스레드 모델 — shared-nothing per-core 아키텍처와 충돌. Beast는 이미 vcpkg에 있음 |
| 메트릭 범위 | 프레임워크+어댑터 풀 구현 + 서비스 등록 API | 운영 가시성 핵심은 프레임워크/어댑터. 서비스는 API만 제공하여 후속 확장 용이 |
| 호스팅 위치 | Server의 control_io_ | Prometheus scrape는 10~30초에 1회 단순 GET — per-core Listener 분배 불필요. control_io_는 시그널 대기 중이라 HTTP acceptor 추가가 자연스러움 |

## 3. 아키텍처

### 3.1 전체 구조

```
Server
├── control_io_  ← 기존 시그널 대기
│   └── MetricsHttpServer (Beast)  ← 새로 추가
│       ├── GET /metrics → MetricsRegistry 조회 → Prometheus text 응답
│       ├── GET /health  → 200 OK
│       └── GET /ready   → running_ 체크
│
├── CoreEngine (per-core)
│   ├── CoreMetrics (atomic counters)  ← 기존 확장
│   └── Services (per-core instances)
│       └── 서비스별 메트릭 (서비스가 등록 API로 추가)
│
└── Adapters
    ├── KafkaAdapter  → 자체 메트릭 (produce/consume 카운터)
    ├── RedisAdapter  → 자체 메트릭 (command 카운터, 연결 상태)
    └── PgAdapter     → 자체 메트릭 (쿼리 카운터, 풀 사용량)
```

### 3.2 데이터 흐름

1. 각 컴포넌트가 자기 atomic 카운터를 런타임에 갱신 (lock-free)
2. Prometheus가 `/metrics`에 HTTP GET
3. MetricsHttpServer가 MetricsRegistry를 순회하며 모든 메트릭 값을 읽음
4. Prometheus text exposition format으로 직렬화하여 응답
5. per-core 메트릭은 라벨(`core="0"`)로 구분 — 합산은 Prometheus의 `sum()` 쿼리에 위임

### 3.3 Prometheus 연동

Prometheus는 MSA 파이프라인(Kafka 경유)과 **완전히 독립**된 채널. 각 서비스의 HTTP /metrics 엔드포인트에 직접 pull 방식으로 접근한다.

```
우리 MSA 파이프라인: Client → Gateway → Kafka → Services → Redis/PG
Prometheus 모니터링: Prometheus → 각 서비스 /metrics (직접 HTTP GET, 10~30초 간격)
```

## 4. MetricsRegistry

### 4.1 메트릭 타입

```
Counter   — 단조 증가 (요청 수, 에러 수). atomic<uint64_t>
Gauge     — 증감 가능 (활성 세션 수, 큐 깊이). atomic<int64_t>
Histogram — 분포 측정 (응답 시간). v0.6.1에서 미구현, API만 예약
```

### 4.2 설계 원칙

- MetricsRegistry는 **Server가 소유**, 어댑터/서비스에 참조 전달
- 메트릭 등록은 **초기화 시점에 1회** (on_configure/on_start), 런타임에 동적 등록 없음
- 내부 저장은 `std::vector<MetricEntry>` — 등록 순서 고정, scrape 시 순회만
- per-core 메트릭은 **label로 구분** (`core="0"`, `core="1"`)
- 스레드 안전: 등록은 단일 스레드(초기화), 값 갱신은 atomic, 읽기는 scrape 시점에 relaxed load
- **shared-nothing 예외**: SessionManager, MessageDispatcher 등 per-core 클래스에 `std::atomic` 카운터를 추가한다. 이 카운터는 해당 코어 스레드만 갱신하고, scrape 스레드(control_io_)가 `relaxed` load로 읽기만 한다. 관찰 전용 단방향 접근이므로 ordering 의존 없이 안전하며, per-core shared-nothing 원칙의 의도적 예외이다.

### 4.3 서비스 등록 API

서비스는 `ctx.server.metrics_registry()` 경유로 메트릭을 등록한다. 기존 `ctx.server.adapter<T>()` 패턴과 동일:

```cpp
// 서비스가 on_configure에서:
auto& registry = ctx.server.metrics_registry();
my_counter_ = registry.counter("auth_login_total", "Total login attempts");
my_gauge_   = registry.gauge("auth_active_sessions", "Active sessions");
```

### 4.4 어댑터 메트릭 등록

어댑터는 `AdapterInterface`에 추가되는 `register_metrics(MetricsRegistry&)` 가상 메서드로 등록한다. 기본 구현은 no-op:

```cpp
// AdapterInterface:
virtual void register_metrics(MetricsRegistry&) {}  // 기본 no-op

// KafkaAdapter에서 오버라이드:
void register_metrics(MetricsRegistry& reg) override {
    produce_total_ = reg.counter("apex_kafka_produce_total", "Kafka produce total");
    // ...
}
```

**호출 시점**: `Server::run()`에서 `adapter->init()` 직후, 서비스 초기화 전에 `adapter->register_metrics(registry_)` 호출. 어댑터가 먼저 등록하고, 서비스가 on_configure에서 추가 메트릭을 등록하는 순서.

## 5. 메트릭 목록

### 5.1 apex_core (프레임워크) — 8개

| 이름 | 타입 | 라벨 | 설명 |
|------|------|------|------|
| `apex_sessions_active` | Gauge | `core` | per-core 활성 세션 수 |
| `apex_sessions_created_total` | Counter | `core` | 세션 생성 누적 |
| `apex_sessions_timeout_total` | Counter | `core` | 타임아웃 누적 |
| `apex_messages_dispatched_total` | Counter | `core` | 메시지 디스패치 누적 |
| `apex_handler_exceptions_total` | Counter | `core` | 핸들러 예외 누적 |
| `apex_crosscore_post_total` | Counter | `core` | cross-core 전송 누적 |
| `apex_crosscore_post_failures_total` | Counter | `core` | cross-core 실패 누적 |
| `apex_session_pool_heap_fallback_total` | Counter | `core` | 세션 풀 고갈 시 힙 폴백 누적 (SessionManager) |

### 5.2 apex_shared (어댑터) — 9개

| 이름 | 타입 | 라벨 | 설명 |
|------|------|------|------|
| `apex_kafka_produce_total` | Counter | — | Kafka 발행 누적 |
| `apex_kafka_produce_errors_total` | Counter | — | Kafka 발행 실패 누적 |
| `apex_kafka_consume_total` | Counter | — | Kafka 소비 누적 |
| `apex_kafka_dlq_total` | Counter | — | DLQ 전송 누적 (KafkaConsumer 에러 핸들링 경로에서 증가) |
| `apex_redis_commands_total` | Counter | `core` | Redis 커맨드 누적 (per-core RedisMultiplexer) |
| `apex_redis_connected` | Gauge | `core` | Redis 연결 상태 (0/1, per-core) |
| `apex_pg_queries_total` | Counter | `core` | PG 쿼리 누적 (per-core PgPool) |
| `apex_pg_pool_active` | Gauge | `core` | PG 풀 활성 연결 수 (per-core) |
| `apex_circuit_breaker_state` | Gauge | `name` | 서킷 상태 (0=CLOSED, 1=OPEN, 2=HALF_OPEN) |

### 5.3 총계

프레임워크 8 + 어댑터 9 = **17개 메트릭**. 핵심 운영 지표에 집중.

## 6. MetricsHttpServer

### 6.1 호스팅

- Server의 `control_io_` (기존 시그널 처리 io_context)에서 구동
- Boost.Beast HTTP acceptor
- 단일 스레드, 비동기 (control_io_ 이벤트 루프 공유)

### 6.2 엔드포인트

| 경로 | 메서드 | 응답 | 용도 |
|------|--------|------|------|
| `/metrics` | GET | Prometheus text exposition format | Prometheus scrape |
| `/health` | GET | `200 OK` | 서비스 생존 확인 (향후 liveness probe) |
| `/ready` | GET | `200 OK` / `503` | 서비스 준비 상태 (running_ 체크, 향후 readiness probe) |

### 6.3 Prometheus Text Format

```
# HELP apex_sessions_active Current active session count
# TYPE apex_sessions_active gauge
apex_sessions_active{core="0"} 42
apex_sessions_active{core="1"} 38
```

### 6.4 라이프사이클

- Server::run()에서 `MetricsHttpServer::start(control_io_, port)` 호출 (listeners 시작 직전)
- Server::stop()에서 `MetricsHttpServer::stop()` 호출 (drain 시작 시)

## 7. 설정 (TOML)

```toml
[metrics]
enabled = true
port = 8081
```

AppConfig에 MetricsConfig 구조체 추가:

```cpp
struct MetricsConfig {
    bool enabled = false;
    uint16_t port = 8081;
};
```

`enabled = false` 기본값 — 명시적 활성화 필요.

## 8. 인프라 변경

| 파일 | 변경 |
|------|------|
| `apex_infra/prometheus/prometheus.yml` | 기존 코멘트된 scrape target 주석 해제 |
| `vcpkg.json` (루트) | boost-beast 의존 유지 (이미 있음) |
| `apex_core/vcpkg.json` | boost-beast 추가 (core에서 직접 사용) |

docker-compose.yml 변경 없음 (이미 `--profile observability`로 Prometheus+Grafana staged).

## 9. 파일 변경 범위

### 9.1 신규 파일

| 파일 | 설명 |
|------|------|
| `apex_core/include/apex/core/metrics_registry.hpp` | Counter/Gauge/MetricsRegistry 정의 |
| `apex_core/src/metrics_registry.cpp` | Prometheus text format 직렬화 |
| `apex_core/include/apex/core/metrics_http_server.hpp` | Beast HTTP 서버 선언 |
| `apex_core/src/metrics_http_server.cpp` | /metrics, /health, /ready 핸들러 구현 |

### 9.2 수정 파일

| 파일 | 변경 |
|------|------|
| `apex_core/include/apex/core/config.hpp` | MetricsConfig 구조체 추가 |
| `apex_core/src/config.cpp` | [metrics] TOML 파싱 |
| `apex_core/include/apex/core/server.hpp` | MetricsRegistry + MetricsHttpServer 멤버, `metrics_registry()` public getter 추가 |
| `apex_core/src/server.cpp` | 프레임워크 메트릭 등록 + adapter->register_metrics() 호출 + HTTP 서버 기동/셧다운 |
| `apex_core/include/apex/core/session_manager.hpp` | 세션 생성/타임아웃/힙 폴백 atomic 카운터 추가 |
| `apex_core/include/apex/core/message_dispatcher.hpp` | 디스패치/예외 atomic 카운터 추가 |
| `apex_core/include/apex/core/core_engine.hpp` | CoreMetrics 확장 (기존 2개 유지) |
| `apex_shared/.../adapter_interface.hpp` | `register_metrics(MetricsRegistry&)` 가상 메서드 선언 (기본 no-op) |
| `apex_shared/.../adapter_base.hpp` | `AdapterWrapper`에서 `register_metrics` 위임 |
| `apex_shared/.../kafka_producer.hpp` | produce 카운터 추가 |
| `apex_shared/.../kafka_consumer.hpp` | consume/DLQ 카운터 추가 |
| `apex_shared/.../redis_multiplexer.hpp` | command/연결 카운터 추가 |
| `apex_shared/.../pg_pool.hpp` | 쿼리/풀 카운터 추가 |
| `apex_shared/.../circuit_breaker.hpp` | 상태 gauge 추가 |
| `apex_infra/prometheus/prometheus.yml` | scrape target 주석 해제 |
| `apex_core/vcpkg.json` | boost-beast 의존 추가 |
| `apex_core/CMakeLists.txt` | 신규 소스 파일 추가 |
