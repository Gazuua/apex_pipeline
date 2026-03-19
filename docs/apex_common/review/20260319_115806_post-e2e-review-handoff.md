# Post-E2E 코드 리뷰 핸드오프 문서

**이슈**: BACKLOG #48
**브랜치**: `feature/48-post-e2e-code-review`
**작성일**: 2026-03-19
**목적**: #1 프레임워크 가이드 작성 에이전트에 리뷰 결과를 전달하여 가이드 품질 향상에 활용

---

## 1. 리뷰 개요

PR #27~#37 (v0.5.2.0 ~ v0.5.5.1) 범위의 ~18K줄 프로덕션 코드를 10개 관점으로 리뷰.
총 46건 고유 이슈 발견 (CRITICAL 1 / MAJOR 27 / MINOR 18).

---

## 2. 수정 완료 항목 (25건) — 빌드 + 71/71 테스트 통과

### 2.1 Safety / Shutdown (7건)

| # | 등급 | 파일 | 수정 내용 |
|---|------|------|-----------|
| S1 | **CRITICAL** | `gateway_service.cpp` | Kafka consumer 콜백의 `ResponseDispatcher` 레퍼런스 캡처(`&rd`)를 `globals_` shared_ptr 복사 캡처로 변경 — shutdown 소멸 순서 의존 제거 |
| S2 | MAJOR | `gateway_service.cpp` | `on_stop()`에서 `globals_->pubsub_listener->stop()` 호출 추가 — CoreEngine 정지 전 PubSubListener 스레드 명시적 중단 |
| S3 | MAJOR | `auth_service.cpp` | `on_stop()` 보강 — `session_store_.reset()` + 어댑터 raw pointer null 리셋 |
| S4 | MAJOR | `chat_service.cpp` + 헤더 | `on_stop()` 구현 추가 — 어댑터 포인터 4개 null 리셋 |
| S5 | MAJOR | `service_base.hpp` | `bump()`, `arena()`, `core_id()` 접근자에 `assert(per_core_ != nullptr)` 가드 추가 |
| S6 | MAJOR | `gateway_service.cpp` | `rl_redis_adapter_->do_init()` → `init()` 변경 — `ready_` 플래그 정상 설정 |
| S7 | MINOR | `server.cpp` | `~Server()` 소멸자에서 `running_` 플래그 확인 + finalize_shutdown 미호출 경고 로깅 |

### 2.2 에러 핸들링 (8건)

| # | 등급 | 파일 | 수정 내용 |
|---|------|------|-----------|
| E1 | MAJOR | `auth_service.cpp` ×4곳 | `session_store_->set_user_session_id()` / `remove_user_session_id()` 반환값 체크 + error 로깅 추가 (기존엔 완전 무시) |
| E2 | MAJOR | `auth_service.cpp` ×3곳 | `(void)revoke_all`, `(void)revoke_result` → warn 로깅 / `(void)insert_result` → **error** 로깅 (데이터 정합성) |
| E3 | MAJOR | `chat_service.cpp` | `on_send_message` Kafka persist produce 결과 체크 + error 로깅 (미저장 시 데이터 손실 감지) |
| E4 | MAJOR | `chat_service.cpp` ×2곳 | PG 쿼리 실패 시 잘못된 에러 코드 수정: `ChatMessageError_NOT_IN_ROOM` / `ChatRoomError_ROOM_NOT_FOUND` → `INTERNAL_ERROR` |
| E5 | MAJOR | `chat_room.fbs`, `chat_message.fbs` | FlatBuffers 스키마에 `INTERNAL_ERROR = 8` 열거값 추가 |
| E6 | MAJOR | `service_base.hpp` | `route<T>` FlatBuffers 검증 실패 시 `spdlog::warn` 추가 (기존엔 무로깅) |
| E7 | MAJOR | `kafka_adapter.cpp` | producer flush `[[nodiscard]]` 반환값 체크 + timeout 경고 로깅 |
| E8 | MAJOR | `gateway_service.hpp` | 미사용 `jwt_blacklist` 매개변수 + 멤버 변수 제거 |

### 2.3 매직넘버 / 설정 외부화 (10건)

| # | 등급 | 대상 | 기존 값 | 수정 |
|---|------|------|---------|------|
| C1 | MAJOR | Gateway `msg_id 3,4,5` | 숫자 리터럴 | `system_msg_ids::AUTHENTICATE_SESSION` 등 상수 구조체 도입 |
| C2 | MAJOR | Gateway `heartbeat_timeout_ticks` | `300` 하드코딩 | `GatewayConfig` + TOML `[timeouts]` |
| C3 | MAJOR | Gateway `sweep_interval_ms` | `1000` 하드코딩 | `GatewayConfig` + TOML `[timeouts]` |
| C4 | MAJOR | Gateway `global_channels` | `{"pub:global:chat"}` | 기본값 `{}` + TOML `[pubsub]`에 명시 |
| C5 | MAJOR | Kafka `producer_poll_interval` | `100ms` | `KafkaConfig.producer_poll_interval_ms` |
| C6 | MAJOR | Kafka `flush_timeout` | `10s`/`5s` | `KafkaConfig.flush_timeout_ms` (adapter + producer 소멸자) |
| C7 | MAJOR | Kafka `consumer_max_batch` | `64` | `KafkaConfig.consumer_max_batch` |
| C8 | MAJOR | Redis `max_pending_commands` | `4096` | `RedisConfig.max_pending_commands` (slab allocator) |
| C9 | MAJOR | PubSubListener `reconnect_interval` | `1s` ×2곳 | `PubSubListener::Config.reconnect_interval_ms` |
| C10 | MAJOR | Chat `max_room_name_length`, `max_list_rooms_limit` | `100`, `100u` | `ChatService::Config` 필드 추가 |

---

## 3. 가이드 대기 항목 (11건) — 아키텍처 결정 필요

이 항목들은 **프레임워크의 의도된 설계**를 확인해야 올바르게 수정할 수 있어서 가이드 완성 후 진행.
가이드 작성 시 아래 항목들을 **명시적으로 다뤄주면** 수정 작업이 크게 수월해짐.

### 3.1 KafkaServiceBase / post_init_callback 마이그레이션

**현상**: Auth/Chat `main.cpp`의 `post_init_callback` 내부 KafkaDispatchBridge 와이어링 ~50줄이 사실상 동일. Gateway는 이미 라이프사이클 훅(`on_configure`/`on_wire`/`on_start`)으로 마이그레이션 완료.

**가이드에서 다뤄줄 내용**:
- `has_kafka_handlers()`를 활용한 자동 배선이 코어 책임인지, 서비스 책임인지
- Kafka-based 서비스를 위한 중간 계층(`KafkaServiceBase<T>`) 도입 여부
- `post_init_callback` 폐기 로드맵 (현재 서버 코드에 "Auth/Chat 마이그레이션 완료 시 제거 예정" 주석 존재)

### 3.2 ServiceRegistry 활용

**현상**: `ServiceRegistry` + `ServiceRegistryView`가 코어에 존재하지만, `Server::run()`에서 서비스를 registry에 등록하지 않아 빈 상태. 4곳에서 `dynamic_cast<Service*>(state.services[0].get())` 패턴 사용.

**가이드에서 다뤄줄 내용**:
- `registry.get<T>()` 패턴이 의도된 서비스 접근 방법인지
- `Server::run()`에서 registry 자동 등록이 코어 책임인지
- per-core 서비스 접근의 올바른 패턴

### 3.3 GatewayGlobals 소유권

**현상**: `GatewayGlobals`가 `shared_ptr`로 전 코어에 공유. 내부에 `ResponseDispatcher`, `BroadcastFanout`, `PubSubListener` 등이 있고, 소멸 순서가 ref-count에 의존.

**가이드에서 다뤄줄 내용**:
- cross-core 공유 자원의 권장 소유권 모델 (Server 레벨 단일 소유 + raw pointer 참조 vs shared_ptr)
- `on_wire()`에서 core 0이 생성한 자원을 다른 코어가 접근하는 패턴의 정석
- shutdown 시 cross-core 자원 정리 순서

### 3.4 ChannelSessionMap per-core 전환

**현상**: `ChannelSessionMap`이 `std::shared_mutex`로 전 코어 공유 — shared-nothing 원칙 위반. 브로드캐스트 빈번 시 경합.

**가이드에서 다뤄줄 내용**:
- per-core 구독 맵 + cross-core 메시지 동기화가 권장 패턴인지
- `cross_core_post()` 사용 시 주의사항 (shutdown 시 io_context 정지 상태에서의 post)
- 어떤 경우에 cross-core 공유가 허용되는지 (예외 기준)

### 3.5 send_response 에러 헬퍼

**현상**: Auth/Chat 핸들러에서 에러 응답 빌드 패턴 (`FlatBufferBuilder` → `Create*Response` → `Finish` → `send_response`)이 15회 이상 반복.

**가이드에서 다뤄줄 내용**:
- `ServiceBase` 또는 Kafka 서비스 베이스에 에러 응답 헬퍼를 제공할 계획인지
- `send_response`의 시그니처와 사용 패턴의 정석
- FlatBuffers 응답 빌드 패턴의 권장 추상화 수준

### 3.6 Kafka 콜백 힙 할당 최적화

**현상**: Kafka consumer 콜백에서 `std::make_shared<std::vector<uint8_t>>(payload)` 매번 힙 할당. consumer 스레드가 코어 외부이므로 per-core allocator 접근 불가.

**가이드에서 다뤄줄 내용**:
- Kafka consumer 스레드와 코어 스레드 간 데이터 전달의 권장 패턴
- consumer 스레드용 메모리 풀 도입 계획 여부
- `cross_core_post` 시 payload 소유권 이전 패턴

### 3.7 co_spawn(detached) 추적

**현상**: Auth/Chat의 Kafka 핸들러 코루틴이 `co_spawn(detached)`로 fire-and-forget 실행. shutdown 시 진행 중인 코루틴 완료 대기 없음.

**가이드에서 다뤄줄 내용**:
- `co_spawn(detached)` 사용의 허용 범위와 대안
- outstanding 코루틴 추적 메커니즘 (completion handler + counter?)
- graceful shutdown 시 진행 중인 코루틴 처리 전략

---

## 4. MINOR → 백로그 이전 예정 (18건, 주요 항목만)

| 항목 | 설명 |
|------|------|
| `ParsedConfig` 중복 | Auth/Chat main.cpp에서 익명 네임스페이스 구조체 중복 정의 |
| `std::unordered_map` | 서비스 레이어 5곳에서 `boost::unordered_flat_map` 미사용 |
| `response_topic` 기본값 | Auth(`auth.responses`)/Chat(`chat.responses`) 기본값 ≠ 실제 사용값(`gateway.responses`) |
| `pub:global:chat` 산재 | Gateway config + Chat 서비스 + TOML 3곳에 동일 문자열 |
| `on_start` default handler 비대 | Gateway의 65줄 인라인 람다, system message handler 미분리 |
| `set_default_handler` 캡슐화 우회 | Gateway가 `dispatcher()` 직접 조작 |
| `BroadcastFanout` 힙 할당 | `make_shared<vector<uint8_t>>` per-broadcast |
| `drain_batch_limit` 미파싱 | CoreEngineConfig에 필드는 있으나 TOML에서 미파싱 |
| `Session::max_queue_depth_` | `256` 하드코딩, ServerConfig 미노출 |
| `Kafka producer 소멸자 5초` | 수정 6과 별도의 5초 하드코딩 (이미 config 연동됨) |
