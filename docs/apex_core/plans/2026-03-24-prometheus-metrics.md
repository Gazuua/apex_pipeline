# v0.6.1 Prometheus 메트릭 노출 — 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 프레임워크/어댑터 17개 메트릭을 Prometheus exposition format으로 HTTP `/metrics` 엔드포인트에 노출한다.

**Architecture:** Server의 control_io_에서 Boost.Beast 경량 HTTP 서버를 구동한다. MetricsRegistry가 Counter/Gauge를 관리하고, scrape 시점에 per-core atomic 카운터를 읽어 text format으로 직렬화한다. 어댑터는 `register_metrics()` 가상 메서드로, 서비스는 `ctx.server.metrics_registry()` 경유로 메트릭을 등록한다.

**Tech Stack:** C++23, Boost.Beast (HTTP), Boost.Asio, spdlog, toml++ (config)

**Spec:** `docs/apex_core/plans/2026-03-24-prometheus-metrics-design.md`

---

## 파일 구조

### 신규 파일

| 파일 | 책임 |
|------|------|
| `apex_core/include/apex/core/metrics_registry.hpp` | Counter, Gauge, MetricsRegistry 클래스 정의 |
| `apex_core/src/metrics_registry.cpp` | Prometheus text format 직렬화 (serialize) |
| `apex_core/include/apex/core/metrics_http_server.hpp` | Beast HTTP 서버 선언 |
| `apex_core/src/metrics_http_server.cpp` | /metrics, /health, /ready 핸들러 |
| `apex_core/tests/unit/test_metrics_registry.cpp` | MetricsRegistry 단위 테스트 |

### 수정 파일

| 파일 | 변경 |
|------|------|
| `apex_core/include/apex/core/config.hpp` | MetricsConfig 구조체 추가, AppConfig에 metrics 필드 |
| `apex_core/src/config.cpp` | `[metrics]` TOML 파싱 (`parse_metrics` 함수) |
| `apex_core/include/apex/core/adapter_interface.hpp` | `register_metrics(MetricsRegistry&)` 가상 메서드 추가 |
| `apex_shared/.../adapter_base.hpp` | AdapterWrapper에서 register_metrics 위임 |
| `apex_core/include/apex/core/server.hpp` | MetricsRegistry + MetricsHttpServer 멤버, `metrics_registry()` getter |
| `apex_core/src/server.cpp` | 프레임워크 메트릭 등록, adapter->register_metrics() 호출, HTTP 서버 기동/셧다운 |
| `apex_core/include/apex/core/session_manager.hpp` | 세션 생성/타임아웃/힙폴백 atomic 카운터 |
| `apex_core/src/session_manager.cpp` | 카운터 증가 로직 |
| `apex_core/include/apex/core/message_dispatcher.hpp` | 디스패치/예외 atomic 카운터 |
| `apex_core/src/message_dispatcher.cpp` | 카운터 증가 로직 |
| `apex_shared/.../kafka_producer.hpp` | produce/error atomic 카운터 |
| `apex_shared/.../kafka_producer.cpp` | 카운터 증가 |
| `apex_shared/.../kafka_consumer.hpp` | consume/DLQ atomic 카운터 |
| `apex_shared/.../kafka_consumer.cpp` | 카운터 증가 |
| `apex_shared/.../redis_multiplexer.hpp` | command/connected atomic 카운터 |
| `apex_shared/.../redis_multiplexer.cpp` | 카운터 증가 |
| `apex_shared/.../pg_pool.hpp` | query/active atomic 카운터 |
| `apex_shared/.../pg_pool.cpp` 또는 `pg_connection.cpp` | 카운터 증가 |
| `apex_shared/.../circuit_breaker.hpp` | state gauge 노출 getter |
| `apex_infra/prometheus/prometheus.yml` | scrape target 주석 해제 |
| `apex_core/vcpkg.json` | boost-beast 의존 추가 |
| `apex_core/CMakeLists.txt` | 신규 소스 파일 + Boost::beast 링크 |

---

## Task 1: MetricsConfig + TOML 파싱

**Files:**
- Modify: `apex_core/include/apex/core/config.hpp`
- Modify: `apex_core/src/config.cpp`

- [ ] **Step 1:** `config.hpp`에 MetricsConfig 구조체 추가 (enabled=false, port=8081), AppConfig에 `MetricsConfig metrics` 필드 추가
- [ ] **Step 2:** `config.cpp`에 `parse_metrics(const toml::table&)` 함수 추가. 기존 `parse_logging`과 동일 패턴. `get_or` 헬퍼 사용
- [ ] **Step 3:** `AppConfig::from_file`에서 `config.metrics = parse_metrics(tbl)` 호출 추가
- [ ] **Step 4:** 커밋

---

## Task 2: MetricsRegistry 코어 + 단위 테스트

**Files:**
- Create: `apex_core/include/apex/core/metrics_registry.hpp`
- Create: `apex_core/src/metrics_registry.cpp`
- Create: `apex_core/tests/unit/test_metrics_registry.cpp`
- Modify: `apex_core/CMakeLists.txt`

- [ ] **Step 1:** 테스트 파일 작성 — Counter 증가, Gauge 증감, text format 직렬화, 라벨 지원
- [ ] **Step 2:** `metrics_registry.hpp` 작성:
  - `Counter` 클래스: `increment()`, `value()`. `std::atomic<uint64_t>` 기반
  - `Gauge` 클래스: `increment()`, `decrement()`, `set()`, `value()`. `std::atomic<int64_t>` 기반
  - `MetricsRegistry` 클래스 — 두 가지 등록 패턴 지원:
    - **소유 패턴** (어댑터/서비스용): `Counter& counter(name, help, labels={})` — registry가 Counter를 소유, 호출자가 참조를 저장하여 직접 증가
    - **참조 패턴** (프레임워크용): `void counter_from(name, help, labels, std::atomic<uint64_t>& source)` — 외부 atomic을 읽기 전용으로 등록. per-core 컴포넌트의 기존 카운터를 API 변경 없이 노출
    - **콜백 패턴** (동적 값용): `void gauge_fn(name, help, labels, std::function<int64_t()> reader)` — scrape 시점에 콜백 호출 (활성 세션 수 등)
    - `std::string serialize() const` — Prometheus text format
    - 내부: `std::vector<MetricEntry>` (name, help, type, labels, value 소스 — owned/ref/fn 중 하나)
- [ ] **Step 3:** `metrics_registry.cpp` 작성 — `serialize()` 구현: `# HELP`, `# TYPE`, `name{labels} value` 형식
- [ ] **Step 4:** CMakeLists.txt에 소스 + 테스트 타겟 추가, Boost::beast 링크 (아직 HTTP 미사용이지만 선제 추가)
- [ ] **Step 5:** vcpkg.json에 boost-beast 추가
- [ ] **Step 6:** 빌드 + 테스트 실행
- [ ] **Step 7:** 커밋

---

## Task 3: MetricsHttpServer (Beast HTTP)

**Files:**
- Create: `apex_core/include/apex/core/metrics_http_server.hpp`
- Create: `apex_core/src/metrics_http_server.cpp`
- Modify: `apex_core/CMakeLists.txt`

- [ ] **Step 1:** `metrics_http_server.hpp` 작성:
  - `MetricsHttpServer` 클래스
  - `start(io_context&, uint16_t port, MetricsRegistry&, std::atomic<bool>& running)` — acceptor 시작
  - `stop()` — acceptor 닫기
  - 내부: Beast `tcp::acceptor`, 비동기 accept 루프
- [ ] **Step 2:** `metrics_http_server.cpp` 작성:
  - accept loop: `async_accept` → `handle_request` 코루틴
  - `handle_request`: HTTP request 파싱 → 경로별 분기
    - `GET /metrics` → `registry.serialize()` → 200 OK, `text/plain; version=0.0.4; charset=utf-8`
    - `GET /health` → 200 OK
    - `GET /ready` → `running_ ? 200 : 503`
    - 그 외 → 404
  - Beast 최소 구현: `http::request<http::empty_body>` 읽기, `http::response<http::string_body>` 쓰기
- [ ] **Step 3:** CMakeLists.txt에 소스 추가
- [ ] **Step 4:** 커밋

---

## Task 4: AdapterInterface 확장

**Files:**
- Modify: `apex_core/include/apex/core/adapter_interface.hpp`
- Modify: `apex_shared/lib/adapters/common/include/apex/shared/adapters/adapter_base.hpp`

- [ ] **Step 1:** `adapter_interface.hpp`에 forward declaration + 가상 메서드 추가:
  ```cpp
  class MetricsRegistry;  // forward decl
  virtual void register_metrics(MetricsRegistry&) {}  // 기본 no-op
  ```
- [ ] **Step 2:** `adapter_base.hpp`의 `AdapterWrapper`에 위임 추가:
  ```cpp
  void register_metrics(MetricsRegistry& reg) override { adapter_.register_metrics(reg); }
  ```
  `AdapterBase`에도 기본 no-op `register_metrics` 추가
- [ ] **Step 3:** 커밋

---

## Task 5: Server 통합

**Files:**
- Modify: `apex_core/include/apex/core/server.hpp`
- Modify: `apex_core/src/server.cpp`

- [ ] **Step 1:** `server.hpp`에 추가:
  - `#include <apex/core/metrics_registry.hpp>` + `#include <apex/core/metrics_http_server.hpp>`
  - 멤버: `MetricsRegistry metrics_registry_`, `std::unique_ptr<MetricsHttpServer> metrics_http_server_`
  - public getter: `MetricsRegistry& metrics_registry() noexcept { return metrics_registry_; }`
- [ ] **Step 2:** `server.cpp` `run()` 수정:
  - adapter init 루프 직후 (line ~155): `adapter->register_metrics(metrics_registry_)` 호출 추가 (Task 4에서 인터페이스 추가 완료)
  - listeners start 직전 (line ~291): `if (config에서 metrics.enabled) MetricsHttpServer::start(control_io_, port, registry, running_)` 호출
- [ ] **Step 3:** `server.cpp` shutdown 수정:
  - `begin_shutdown()` 또는 drain 시작 부분에서 `metrics_http_server_->stop()` 호출
- [ ] **Step 4:** 커밋

---

## Task 6: 프레임워크 메트릭 계측

**Files:**
- Modify: `apex_core/include/apex/core/session_manager.hpp` + `session_manager.cpp`
- Modify: `apex_core/include/apex/core/message_dispatcher.hpp` + `message_dispatcher.cpp`

- [ ] **Step 1:** `session_manager.hpp`에 atomic 카운터 추가:
  - `std::atomic<uint64_t> sessions_created_{0}`
  - `std::atomic<uint64_t> sessions_timeout_{0}`
  - `std::atomic<uint64_t> heap_fallback_{0}`
  - public getters (const noexcept)
- [ ] **Step 2:** `session_manager.cpp`에서 카운터 증가:
  - `create_session()`: `sessions_created_.fetch_add(1, std::memory_order_relaxed)`
  - `on_timer_expire()`: `sessions_timeout_.fetch_add(1, ...)`
  - 힙 폴백 경로 (line ~32): `heap_fallback_.fetch_add(1, ...)`
  - 기존 `sessions_.size()`를 활성 세션 gauge로 사용 (scrape 시 읽기)
- [ ] **Step 3:** `message_dispatcher.hpp`에 atomic 카운터 추가:
  - `std::atomic<uint64_t> dispatched_{0}`
  - `std::atomic<uint64_t> exceptions_{0}`
- [ ] **Step 4:** `message_dispatcher.cpp`에서 카운터 증가:
  - dispatch 경로: `dispatched_.fetch_add(1, ...)`
  - exception catch 경로: `exceptions_.fetch_add(1, ...)`
- [ ] **Step 5:** `server.cpp`에서 프레임워크 메트릭을 MetricsRegistry에 바인딩 (**참조 패턴 + 콜백 패턴** 사용):
  - per-core 루프에서 각 코어의 컴포넌트 카운터를 `counter_from()` / `gauge_fn()`으로 등록:
    - `counter_from("apex_sessions_created_total", ..., {{"core", "N"}}, session_mgr.sessions_created())`
    - `gauge_fn("apex_sessions_active", ..., {{"core", "N"}}, [&sm]{ return sm.active_count(); })`
    - CoreMetrics의 기존 `post_total`, `post_failures`도 `counter_from`으로 등록
  - 이 방식으로 per-core 컴포넌트의 API를 변경하지 않고 메트릭을 노출
- [ ] **Step 6:** 커밋

---

## Task 7: 어댑터 메트릭 계측

**Files:**
- Modify: `apex_shared/.../kafka_producer.hpp` + `kafka_producer.cpp`
- Modify: `apex_shared/.../kafka_consumer.hpp` + `kafka_consumer.cpp`
- Modify: `apex_shared/.../redis_multiplexer.hpp` + `redis_multiplexer.cpp`
- Modify: `apex_shared/.../pg_pool.hpp` + `pg_connection.cpp`
- Modify: `apex_shared/.../circuit_breaker.hpp`
- Modify: `apex_shared/.../kafka_adapter.hpp` + `kafka_adapter.cpp`
- Modify: `apex_shared/.../redis_adapter.hpp` + `redis_adapter.cpp`
- Modify: `apex_shared/.../pg_adapter.hpp` + `pg_adapter.cpp`

- [ ] **Step 1:** 각 컴포넌트(Producer, Consumer, Multiplexer, PgPool/PgConnection, CircuitBreaker)에 `std::atomic<uint64_t>` 카운터 추가 + public getter
- [ ] **Step 2:** 각 컴포넌트의 핵심 경로에 `fetch_add` 삽입:
  - KafkaProducer: `produce()` 성공/실패
  - KafkaConsumer: `poll_loop()` 소비, DLQ 발송
  - RedisMultiplexer: `execute()`/`pipeline()` 커맨드 수, 연결 상태
  - PgConnection/PgPool: `execute()`/`query()` 수, 활성 연결
  - CircuitBreaker: 현재 `state_` 값 (이미 atomic)
- [ ] **Step 3:** 각 Adapter 클래스(KafkaAdapter, RedisAdapter, PgAdapter)에서 `register_metrics(MetricsRegistry&)` 오버라이드:
  - 자기 컴포넌트의 카운터 참조를 registry에 등록
  - per-core 어댑터(Redis, PG)는 core 라벨 포함
- [ ] **Step 4:** 커밋

---

## Task 8: 인프라 활성화

**Files:**
- Modify: `apex_infra/prometheus/prometheus.yml`

- [ ] **Step 1:** prometheus.yml에서 apex 서비스 scrape target 주석 해제 + 실제 포트 반영:
  ```yaml
  - job_name: "apex-gateway"
    static_configs:
      - targets: ["host.docker.internal:8081"]
    metrics_path: /metrics
  ```
- [ ] **Step 2:** 커밋

---

## Task 9: 빌드 + 통합 검증

- [ ] **Step 1:** clang-format 실행
- [ ] **Step 2:** debug 빌드 (`apex-agent queue build debug`)
- [ ] **Step 3:** 테스트 실행 (단위 테스트 통과 확인)
- [ ] **Step 4:** 빌드 오류/경고 수정 (있으면)
- [ ] **Step 5:** 최종 커밋
