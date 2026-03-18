# Post-E2E 코드 리뷰 계획서

**브랜치**: `feature/post-e2e-code-review` (예정)
**대상**: PR #27~#37 (v0.5.2.0 ~ v0.5.5.1) — Wave 2 서비스 체인 전체
**작성일**: 2026-03-18

---

## 1. 배경

E2E 11/11 통과(v0.5.5.1) 달성 후, 기능 동작 검증은 완료되었으나 코드 품질 리뷰가 한 번도 수행되지 않은 상태. 101 파일 변경, ~7,863 insertions 규모의 코드를 10개 관점에서 체계적으로 리뷰한다.

## 2. 리뷰 대상 범위

### 코어 프레임워크 (apex_core)
| 파일 | 변경 사유 |
|------|-----------|
| `service_base.hpp` | kafka_route, on_configure/on_wire 라이프사이클, per_core_ 바인딩 |
| `server.hpp / server.cpp` | add_adapter, add_service_factory, post_init_callback, Phase 1→2→3→3.5 시퀀스 |
| `configure_context.hpp / wire_context.hpp` | Phase 1/2 컨텍스트 분리 |
| `service_registry.hpp` | 서비스 간 lookup |
| `message_dispatcher.cpp` | default_handler 지원 |
| `connection_handler.hpp` | multi-listener sync |
| `listener.hpp` | sync_default_handler |
| `session_manager.cpp` | on_session_closed 콜백 |

### 서비스 레이어 (apex_services)
| 서비스 | 핵심 파일 | LOC |
|--------|-----------|-----|
| **Gateway** | `gateway_service.cpp`, `main.cpp`, `response_dispatcher.cpp`, `pubsub_listener.cpp`, `broadcast_fanout.cpp` | ~384 (서비스) + ~81 (main) |
| **Auth** | `auth_service.cpp`, `main.cpp`, `session_store.cpp`, `jwt_manager.cpp` | ~632 (서비스) + ~216 (main) |
| **Chat** | `chat_service.cpp`, `main.cpp`, `chat_db_consumer.cpp` | ~865 (서비스) + ~217 (main) |

### 어댑터/공유 (apex_shared)
| 파일 | 관심 포인트 |
|------|------------|
| `kafka_dispatch_bridge.hpp` | Kafka→코루틴 브릿지 패턴 |
| `kafka_envelope.hpp` | MetadataPrefix 파싱 |

## 3. 리뷰 관점 (10개)

### 관점 1: 코어 인터페이스 단순화 기회
- **질문**: 서비스 코드에서 반복되는 보일러플레이트를 코어 프레임워크 인터페이스로 흡수할 수 있는가?
- **핵심 체크**:
  - Auth/Chat `main.cpp`의 KafkaDispatchBridge 와이어링 코드 ~40줄이 사실상 동일 — 코어가 Kafka 서비스 베이스를 제공하면 제거 가능
  - `post_init_callback` 패턴 vs 라이프사이클 훅 — Gateway는 이미 마이그레이션 완료, Auth/Chat은 미완
  - `dynamic_cast<AuthService*>(state.services[0].get())` — 타입 안전하지 않은 서비스 접근

### 관점 2: 초기화 순서 의존성 제약
- **질문**: 서비스가 init 호출 순서를 잘못하면 런타임 크래시가 발생하는 지점이 있는가? 코어에서 순서를 강제할 수 있는가?
- **핵심 체크**:
  - `on_configure` → `on_wire` → `on_start` 3-phase가 Server가 강제하므로 기본은 안전
  - 그러나 `post_init_callback`은 Phase 3 이후에 실행 → 어댑터 init 완료 보장되나 서비스 시작 후 코루틴 스폰이라 타이밍 의존
  - `per_core_` null 체크 — `internal_configure` 전에 `bump()` / `arena()` 호출 시 UB
  - `std::this_thread::sleep_for(1s)` — Auth/Chat main.cpp에서 PG warm-up / bcrypt 시드 후 하드코딩 대기

### 관점 3: OOP / 유지보수성
- **질문**: 객체지향 원칙 위배, 단일 책임 위반, 과도한 결합이 있는가?
- **핵심 체크**:
  - `GatewayService` 생성자 매개변수 6개 — 과도한 의존성 주입 (config, jwt_verifier, jwt_blacklist, route_table, channel_map, rl_redis_adapter)
  - `GatewayGlobals` 구조체 — core 0 전용 객체를 shared_ptr로 전 코어 공유. 소유권 모호
  - `auth_service.cpp` / `chat_service.cpp` 600~865줄 단일 파일에 모든 핸들러 — 핸들러 분리 검토
  - `ParsedConfig` 익명 네임스페이스 구조체 — main.cpp마다 중복 정의

### 관점 4: shared-nothing 원칙 준수 / 고성능 모듈 활용
- **질문**: 코어 간 데이터 공유, 불필요한 힙 할당, 프레임워크 제공 모듈 미사용이 있는가?
- **핵심 체크**:
  - `ChannelSessionMap` — `std::shared_ptr`로 전 코어 공유. 내부 동기화 방식 확인 필요
  - `GatewayGlobals` — `ResponseDispatcher`, `PubSubListener`가 core 0에서 생성되어 전 코어 접근
  - `std::vector<uint8_t> payload_copy` — Kafka 콜백에서 매번 힙 할당. 슬랩/범프 할당자 사용 가능 여부
  - `std::unordered_map` vs `boost::unordered_flat_map` — 서비스 코드에서 std 버전 사용 여부
  - `auth_states_` 맵 — per-session 상태를 어떤 컨테이너로 관리하는지

### 관점 5: 서비스 간 의존성
- **질문**: 서비스 A가 서비스 B의 내부 구현에 직접 의존하는 코드가 있는가?
- **핵심 체크**:
  - Gateway → Auth/Chat 통신이 Kafka 토픽으로만 이루어지는지 (직접 함수 호출 없는지)
  - `response_topic` 하드코딩 — 한 서비스가 다른 서비스의 토픽명을 알아야 하는 구조
  - 공유 FlatBuffers 스키마가 서비스 간 유일한 계약인지
  - `#include` 그래프에서 cross-service 참조 없는지

### 관점 6: 코루틴 lifetime 안전성
- **질문**: `co_await` 전후로 댕글링 참조가 발생할 수 있는 경로가 있는가?
- **핵심 체크**:
  - `route<T>` 핸들러 — `const FbsType*` 포인터가 `co_await` 이후에도 사용되는지 (ServiceBase 주석에 경고는 있음)
  - `kafka_route<T>` — `MetadataPrefix`가 값 복사인지 참조인지
  - `co_spawn(detached)` — Auth/Chat main.cpp에서 fire-and-forget 코루틴이 캡처하는 레퍼런스의 수명
  - `[&engine]` 캡처 — Kafka 콜백 람다가 CoreEngine 레퍼런스를 캡처하는데 shutdown 시 순서 보장

### 관점 7: 에러 핸들링 일관성
- **질문**: `Result<T>` / `std::expected` 사용이 일관적인가? 에러 무시 경로가 있는가?
- **핵심 체크**:
  - 어댑터 호출 후 `result.has_value()` 체크만 하고 에러 전파 없는 경우
  - `co_return apex::core::ok()` — FlatBuffers 검증 실패 시 에러를 클라이언트에 보내고 ok() 반환. 메트릭/로깅 누락 가능
  - Kafka produce 실패 시 서비스 레벨 에러 핸들링
  - `spdlog::warn` 후 계속 진행하는 패턴 — 무시해도 되는 에러 vs 전파해야 하는 에러 구분

### 관점 8: Shutdown 경로 정합성
- **질문**: Server의 shutdown 시퀀스와 서비스의 리소스 정리가 맞물리는가?
- **핵심 체크**:
  - Server 순서: acceptor 중지 → 세션 drain → 어댑터 drain → 스케줄러 stop → 서비스 stop → CoreEngine stop/join → 어댑터 close
  - Auth `on_stop()` — 로깅만 수행. 진행 중인 DB 트랜잭션 / Kafka produce 정리 없음
  - Chat `on_stop()` — 동일하게 최소한
  - Gateway `~GatewayGlobals()` — PubSubListener/ResponseDispatcher 소멸 타이밍과 Kafka consumer 콜백 간 레이스
  - `co_spawn(detached)` 코루틴이 shutdown 시 완료 대기 없이 버려지는지

### 관점 9: 매직넘버 / 하드코딩 설정
- **질문**: 운영 환경에서 튜닝이 필요한 값이 코드에 하드코딩되어 있는가?
- **핵심 체크**:
  - `std::this_thread::sleep_for(std::chrono::seconds{1})` — Auth/Chat main.cpp
  - `heartbeat_timeout_ticks = 300` — Gateway main.cpp
  - `pool_size_per_core = 2` — TOML 기본값이지만 기본값 자체의 적정성
  - `max_subscriptions_per_session` — Gateway config에서 오는지 확인
  - 타임아웃, 재시도 횟수, 버퍼 크기 등이 TOML로 외부화되어 있는지 전수 확인

### 관점 10: 에이전트 자율 판단 항목
- **설명**: 위 9개 관점에 해당하지 않지만 리뷰 과정에서 발견되는 이슈를 에이전트가 자체 판단으로 보고한다.
- **예상 영역**:
  - 네이밍 일관성 (컨벤션 위반)
  - 불필요한 `#include`
  - 테스트 커버리지 갭
  - API surface 불일치 (헤더 public인데 테스트 없음)
  - 로깅 레벨 부적절 (info → debug 격하 필요)
  - 주석 정합성 (코드와 주석 불일치)

## 4. 실행 방법

### 4.1 리뷰 순서
1. **코어 프레임워크 (apex_core)** — 서비스 계약의 기준이므로 먼저 확인
2. **Gateway 서비스** — 가장 복잡한 라이프사이클 (on_configure → on_wire → on_start)
3. **Auth 서비스** — Kafka-only 서비스 패턴
4. **Chat 서비스** — Auth와 유사하나 Redis PubSub + PG 복합 패턴
5. **Cross-cutting** — 관점 4~10을 전체에 걸쳐 교차 검증

### 4.2 리뷰 산출물
- **리뷰 보고서**: `docs/apex_common/review/YYYYMMDD_HHMMSS_post-e2e-review.md`
  - 관점별 발견 사항 + 등급 (CRITICAL / MAJOR / MINOR)
  - 각 발견 사항에 권장 수정 방안
- **백로그 갱신**: 발견된 이슈 중 별도 작업이 필요한 항목 → BACKLOG.md 추가
- **즉시 수정**: trivial fix (오타, include 정리 등)는 리뷰 브랜치에서 바로 수정 가능

### 4.3 주의사항
- 에이전트 리뷰 시 **코드 실제 상태 확인 필수** — README TODO 목록이나 주석이 아닌 실제 코드 기준 판단
- 기존 백로그(#1~#47)와 중복되는 이슈는 새로 추가하지 않고 기존 항목에 메모 보강
- ASAN/TSAN 통과 상태이므로 sanitizer가 잡는 범위의 메모리 이슈는 저확률 — 타이밍/논리 이슈에 집중

## 5. 이미 식별된 사전 관측 (Pre-observations)

리뷰 실행 전 코드 구조 파악 과정에서 발견된 항목. 리뷰 시 우선 검증 대상.

| # | 관점 | 관측 내용 | 예상 등급 |
|---|------|-----------|----------|
| P1 | 1, 2 | Auth/Chat `post_init_callback` → 라이프사이클 훅 미마이그레이션. KafkaDispatchBridge 와이어링 코드 ~40줄 사실상 복붙 | MAJOR |
| P2 | 2 | `std::this_thread::sleep_for(1s)` — PG warm-up / bcrypt 시드 후 하드코딩 대기. 비동기 완료 대기로 교체 필요 | MAJOR |
| P3 | 1 | `dynamic_cast<Service*>(state.services[0].get())` — 인덱스 기반 서비스 접근. 타입 세이프하지 않음 | MAJOR |
| P4 | 4 | `std::unordered_map` 사용 여부 + ChannelSessionMap 코어 간 공유 동기화 방식 | 확인 필요 |
| P5 | 6 | `co_spawn(detached)` + `[&engine]` 캡처 — shutdown 순서에 따라 dangling ref 가능성 | 확인 필요 |
| P6 | 8 | Auth/Chat `on_stop()` — 로깅만 수행, 진행 중 비동기 작업 정리 없음 | MAJOR |

## 6. 백로그 연동

본 리뷰는 BACKLOG **#48**로 등록.
- **시간축**: NOW (v0.6 진입 전 반드시 수행)
- **내용축**: CRITICAL (리뷰 미수행 시 v0.6 작업에서 기술 부채 누적)
- 리뷰 완료 후 발견 이슈는 개별 백로그 항목으로 분리 등록
