# FSD Backlog Sweep — Core/Shared 프레임워크 리팩토링

- **일시**: 2026-03-22
- **브랜치**: feature/fsd-backlog-20260322_122946
- **PR**: #100

---

## 해결 항목

### BACKLOG-131: KafkaSecurityConfig (security)
- `KafkaSecurityConfig` typed struct 도입 (protocol, ssl_*, sasl_*)
- `extra_properties` map으로 librdkafka 200+ 설정 passthrough
- Producer/Consumer 양쪽에 보안 설정 전달
- auto-review에서 크레덴셜 마스킹 + 보안 설정 실패 시 init 중단 추가

### BACKLOG-130: TransportContext (design-debt)
- `TransportContext` 번들 struct 도입 (`ssl::context*` 포인터 + 향후 확장)
- Transport concept 시그니처 1회 변경: `make_socket(io_context&, const TransportContext&)`
- `make_socket_with_context()` 제거, function-local static `ssl::context` 제거
- per-core SSL_CTX 안전성 확보 (Seastar-style)

### BACKLOG-24: AdapterState enum (design-debt)
- `AdapterBase`에 3-state `AdapterState` enum 도입 (CLOSED/RUNNING/DRAINING)
- `std::atomic<bool> ready_` → `std::atomic<AdapterState> state_` 교체
- KafkaAdapter 독자 `AdapterState` enum + `state_` + `is_running()` 삭제
- 전 어댑터(Redis, Kafka, Pg) 상태 관리 통일

### BACKLOG-29: drain/stop 의미 분리 (design-debt)
- Listener: drain = graceful wind-down, stop = immediate shutdown 주석 명확화
- AdapterBase: init→RUNNING, drain→DRAINING, close→CLOSED 상태 전이 분리

## 드롭 항목

### BACKLOG-132: awaitable do_close()
- 구현 시도 중 Server shutdown 시퀀스와 충돌 발견
- adapter->close()는 core threads 정지 후 호출 — awaitable 실행 불가
- Server shutdown 재설계 후 재시도 필요

## 신규 백로그

- #133: TransportContext OpenSSL 의존 (MAJOR)
- #134: AdapterInterface state 미노출 (MAJOR)
- #135: sasl_password 평문 저장 (MINOR)
