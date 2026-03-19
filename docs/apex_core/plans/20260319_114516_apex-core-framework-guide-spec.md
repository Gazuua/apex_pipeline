# apex_core 프레임워크 가이드 — 설계 스펙

**백로그**: #1
**버전**: v0.5.5.2 기준 (초안 완성 후 main 머지 받아 변경점 반영)
**산출물**: `docs/apex_core/apex_core_guide.md` (단일 파일)

---

## 1. 목적

에이전트가 이 문서 하나만 읽고 apex_core 위에 새 서비스를 올릴 수 있도록 하는 통합 레퍼런스.

- **1차 독자**: AI 에이전트 — 구조화된 시그니처, 제약 조건 목록, 복붙용 패턴
- **2차 독자**: 서비스 개발자 (사람) — 에이전트 우선 구조지만 사람이 읽어도 충분히 이해 가능

---

## 2. 문서 구조

2레이어 구조, 태스크 기반 배치. 에이전트가 위에서 아래로 따라가면 서비스가 완성됨.

### 레이어 1 — 서비스 개발 API 가이드

| 섹션 | 내용 | 비고 |
|------|------|------|
| **§1. 퀵 레퍼런스** | 최소 동작 서비스 스켈레톤 **전체 코드** — MyService 클래스 + main() (TOML 파싱, 어댑터 Config 생성, Server 등록, 로깅 초기화 포함). Kafka 서비스는 `kafka_route<T>()` 등록만으로 자동 배선 (설계 결정 D2). 이후 섹션의 앵커 역할 | 복붙 시작점 |
| **§2. Server 설정 & 부트스트랩** | **§2.1** ServerConfig 전체 필드 + 기본값 + 의미. **§2.2** Server fluent API — `listen<P>`, `add_service<T>` vs `add_service_factory` 선택 기준, `add_adapter<T>(role)`, `server.global<T>(factory)` (cross-core 자원, 설계 결정 D3). **§2.3** TOML 설정 파일 — 서비스별 TOML 구조, 어댑터 Config 타입(KafkaConfig, RedisConfig, PgAdapterConfig) 필드 + TOML 매핑 예시, `toml::parse_file()` 파싱 패턴. LogConfig + `init_logging()` + `shutdown_logging()` (#60). **§2.4** Kafka 자동 배선 — 코어가 `has_kafka_handlers()` 감지 → KafkaDispatchBridge 자동 생성 (설계 결정 D2). `post_init_callback`은 Kafka 배선 용도 폐기 | |
| **§3. 라이프사이클 훅** | Phase 1→2→3→3.5 순서. 각 Phase별: 목적, 파라미터 상세 (ConfigureContext / WireContext), 코드 예시, 이 Phase에서 하면 안 되는 것. Phase 2에서 `registry.get<T>()` 패턴으로 서비스 간 와이어링 (설계 결정 D1). **런타임 훅**: on_session_closed — 세션 종료 시 정리 패턴 (per-session 상태, 구독 해제 등). **Shutdown 시퀀스**: on_stop() — drain_timeout, outstanding 코루틴 카운터 대기 (설계 결정 D7), 어댑터 연결 정리 | |
| **§4. 핸들러 & 메시지** | **§4.1** 핸들러 등록 4종: `handle()` → `route<T>()` → `kafka_route<T>()` → `set_default_handler()`. 각각: 정확한 C++ 시그니처, 용도, 완전한 코드 예시, 주의사항. **§4.2** 메시지 정의: msg_id 할당 규약 (서비스별 범위), .fbs 파일 배치 (서비스 내부 schemas/ vs 공유 apex_shared/schemas/), FlatBuffers 스키마 작성 규약 (namespace, table 구조). **§4.3** 와이어 프로토콜: WireHeader 필드 (msg_id, payload_size, flags), 응답 전송 API 선택 (`async_send` vs `enqueue_write`), ErrorSender::build_error_frame() 패턴 | |
| **§5. 어댑터 접근** | `ctx.server.adapter<T>(role)` 패턴, 현재 3종 (Kafka/Redis/PG), role 기반 다중 인스턴스, on_configure에서 획득 → 레퍼런스 보관. 어댑터별 에러 처리 — CircuitBreaker 상태 (CircuitOpen), PgPool (PoolExhausted), 일반적인 어댑터 에러 핸들링 패턴 | |
| **§6. 메모리 관리** | `bump()` — 요청 스코프 / `arena()` — 트랜잭션 스코프. API, 용량 설정, 판단 기준. 힙 할당은 금지가 아님 (가이드). Kafka consumer 스레드 전용 메모리 풀 — 코어가 제공, per-core allocator 접근 금지 (설계 결정 D6) | |
| **§7. 유틸리티** | `cross_core_call()` / `cross_core_post()` (per-core 복제 동기화 패턴, 설계 결정 D4), `PeriodicTaskScheduler`, `Session` 주요 API, `Result<T>` 패턴, `spawn()` tracked API — 백그라운드 코루틴의 유일한 실행 방법 (설계 결정 D7) | |
| **§8. 금지사항 & 안티패턴** | BAD/GOOD 코드 쌍 6종 (아래 상세) | |
| **§9. 빌드 시스템 통합** | 새 서비스 추가 시 CMakeLists.txt 템플릿 — 디렉토리 구조, FlatBuffers 스키마 컴파일 커스텀 커맨드, target_link_libraries 필수 타겟 목록, apex_services/CMakeLists.txt에 add_subdirectory 추가 | |

#### §8 안티패턴 목록

| # | 안티패턴 | 위험 |
|---|---------|------|
| 1 | co_await 후 FlatBuffers 포인터 접근 | dangling pointer — 코루틴 재개 시 수신 버퍼가 이미 재사용됨 |
| 2 | 코어 간 직접 상태 공유 (멤버 변수, shared_mutex 등) | shared-nothing 위반, 데이터 레이스. per-core 복제 + `cross_core_post()` 사용 (D4) |
| 3 | Phase 순서 위반 (on_configure에서 ServiceRegistry 접근 등) | 다른 서비스 미초기화 상태, nullptr |
| 4 | io_context 직접 접근 / `co_spawn(detached)` | 프레임워크 캡슐화 우회, 코루틴 추적 불가. `spawn()` API 사용 (D7) |
| 5 | 핸들러 내 동기 블로킹 (sleep, 동기 I/O) | 코어 스레드 전체 블로킹, 다른 세션 지연 |
| 6 | 핸들러에서 error 반환의 의미 오해 | dispatch가 에러 프레임 자동 전송 — ok()로 반환하되 직접 에러 응답 전송이 일반 패턴 |
| 7 | cross-core 자원에 shared_ptr 사용 | 소멸 순서 불확정 — `server.global<T>()` + raw ptr 참조 패턴 사용 (D3) |

각 항목에 BAD 코드 → GOOD 코드 쌍 + 1-2줄 설명.

### 레이어 2 — 프레임워크 내부 아키텍처

| 섹션 | 내용 | 비고 |
|------|------|------|
| **§10.1 컴포넌트 배치도** | Server → CoreEngine → PerCoreState → (SessionManager, ServiceRegistry, Allocators, Scheduler), Listener → ConnectionHandler, Adapters, GlobalResourceRegistry. 텍스트 다이어그램 | |
| **§10.2 Phase 시퀀스** | Server::run() 내부 실행 순서. Adapter init → ServiceFactory → Phase 1~3.5 → CoreEngine::run() | "왜 이 순서인지"는 ADR 링크 |
| **§10.3 TCP 요청 처리 흐름** | Client → Session → ConnectionHandler → try_decode → MessageDispatcher → 핸들러 코루틴 | 핫패스 가시화 |
| **§10.4 Kafka 메시지 흐름** | KafkaAdapter → KafkaDispatchBridge → kafka_handler_map → 핸들러 코루틴. Gateway response 경로 포함 | |
| **§10.5 ADR 포인터 테이블** | 서비스 개발에 직접 관련된 ADR 10개 내외 선별. 주제 → ADR 번호 → 파일 경로 | |

### 부록

| 섹션 | 내용 | 비고 |
|------|------|------|
| **§11. 실전 서비스 패턴** | 기존 3서비스에서 추출한 4가지 패턴: ① Gateway 패턴 (default_handler + per-session + `server.global<T>()` cross-core, D3) ② Kafka-only 서비스 패턴 (TCP 없이 `kafka_route<T>()` 자동 배선, D2) ③ 어댑터 다중 역할 (Redis "data" vs "pubsub") ④ 응답 전송 패턴 (TCP direct vs Kafka produce, 에러 응답 헬퍼 자체 작성, D5). 각 패턴: 상황 설명 + 핵심 코드 40-60줄 + 10줄 해설 | 분량 제한 명시 |

---

## 3. 설계 결정사항

### 문서 관련

| 결정 | 선택 | 근거 |
|------|------|------|
| 1차 독자 | 에이전트 | 당장 새 서비스를 만들 주체. 구조화된 레퍼런스가 에이전트 정확도를 높임 |
| #48 선후관계 | #1 먼저 | 가이드가 #48 리뷰 기준선 역할. 인터페이스 대폭 변경 가능성 낮음 |
| 파일 구조 | 단일 파일 | 에이전트는 파일 하나를 통째로 읽는 게 효율적. 레이어 간 앵커 참조 편리 |
| 안티패턴 수준 | BAD/GOOD 상세 | 에이전트 코드 생성 시 정확도 향상. 체크리스트만으로는 실수 방지 부족 |
| 레이어 2 깊이 | 요약 + ADR 포인터 | "왜?"는 ADR에 이미 완벽. 중복 없이 유지보수 용이 |
| 문서 배치 | `docs/apex_core/apex_core_guide.md` | plans/ 아래 X — 일회성 계획이 아닌 지속 유지 레퍼런스 |

### 아키텍처 결정 (D1-D7) — #48 코드 리뷰 핸드오프 기반

가이드가 "이렇게 해야 한다"를 기술하고, 실제 코드 구현은 #48 에이전트가 담당.

| ID | 주제 | 결정 | 근거 | 가이드 반영 섹션 |
|----|------|------|------|----------------|
| **D1** | ServiceRegistry | 코어가 Phase 1 전에 모든 서비스를 자동 등록. `registry.get<T>()` 가 정석, `dynamic_cast` 금지 | Registry가 구현되어 있는데 미사용. 인덱스 기반 접근(`services[0]`)은 등록 순서 의존 취약 패턴 | §3 라이프사이클 |
| **D2** | Kafka 자동 배선 | 코어가 `has_kafka_handlers()` 감지 → KafkaDispatchBridge 자동 생성. `post_init_callback` Kafka용 폐기 | D1과 일관된 "코어 책임" 원칙. Auth/Chat의 ~50줄 동일 보일러플레이트 제거 | §1 스켈레톤, §2.4 |
| **D3** | cross-core 자원 소유권 | Server 레벨 `GlobalResourceRegistry` 도입. `server.global<T>(factory)` + raw ptr 참조. shared_ptr 금지 | shared_ptr ref-count 기반 소멸 순서는 암묵적이고, 람다 캡처로 수명 연장 시 소멸 순서 파괴 (핸드오프 S1이 이 문제). Server 단독 소유 → 명시적 소멸 순서 | §2.2 Server API, §8 금지사항 |
| **D4** | shared-nothing 예외 | 예외 없음. per-core 복제 + `cross_core_post()` 동기화가 원칙. shared_mutex 금지 | 예외 기준("읽기 95%+")은 측정 없이 판단 불가, 실질적으로 원칙 약화. PubSub 전파 지연은 Redis 자체가 비동기이므로 허용 | §7 유틸리티, §8 금지사항 |
| **D5** | send_response 헬퍼 | 가이드에 패턴만 제시, 서비스가 자체 헬퍼 작성 | 각 서비스의 FlatBuffers 스키마(에러 enum, Response 타입)가 달라 코어 제네릭 추상화는 복잡도 대비 효과 낮음. 서비스 증가 시 재평가 | §4.3 응답 패턴 |
| **D6** | Kafka consumer 메모리 | consumer 스레드 전용 메모리 풀 제공, 코어가 관리. per-core allocator 접근 금지 | 프레임워크가 모든 계층에 커스텀 할당기를 제공하는 일관성. 할당기 인프라 이미 존재하여 구현 비용 낮음 | §6 메모리 관리 |
| **D7** | co_spawn 추적 | `spawn()` tracked API 제공 (내부 카운터 증감). io_context 미노출로 서비스의 직접 `co_spawn(detached)` 코드 레벨 차단. shutdown 시 카운터 0 대기 (타임아웃 포함) | D2에서 코어가 Kafka 배선하므로 co_spawn도 코어 래핑 자연스러움. io_context 금지(§8 #4)가 detached 차단까지 해결 | §7 유틸리티, §8 금지사항 |

---

## 4. 유지보수 정책

가이드가 코드와 괴리되지 않도록 프로젝트 지침에 다음 규칙 추가:

### 루트 CLAUDE.md에 추가할 규칙

```
### 프레임워크 가이드 유지보수
- **갱신 트리거**: 코어 인터페이스 변경 시 `docs/apex_core/apex_core_guide.md` 동시 갱신 필수
  - ServiceBase 훅 추가/삭제/시그니처 변경
  - 핸들러 등록 API 변경 (handle, route, kafka_route, set_default_handler)
  - ConfigureContext / WireContext 필드 변경
  - ServerConfig 필드 추가/삭제
  - 새 Phase 도입 또는 Phase 순서 변경
  - 새 어댑터 타입 추가
- **갱신 범위**: 레이어 1(API)은 직접 수정, 레이어 2(내부)는 ADR 포인터 정합성 확인
- **머지 전 체크**: 코어 영역 PR에서 가이드 갱신 여부 확인 (auto-review 체크 항목)
```

### 가이드 포인터 테이블 갱신

루트 CLAUDE.md의 기존 포인터 테이블에 추가:

```
| 프레임워크 가이드 (서비스 개발 API + 내부 아키텍처) | `docs/apex_core/apex_core_guide.md` |
```

---

## 5. 구현 전략

1. **v0.5.5.2 코드 기준 가이드 작성** — 현재 API + D1-D7 설계 결정을 "의도된 설계"로 기술
2. **CLAUDE.md 유지보수 규칙 추가**
3. **가이드 포인터 테이블 갱신**
4. **#48 에이전트에 핸드오프** — 가이드의 D1-D7 설계 결정을 기준으로 코드 구현

**역할 분담**: #1(이 작업)은 가이드 문서 작성만 담당. D1-D7의 코드 구현은 #48 에이전트가 가이드를 참조하여 수행.

---

## 6. 기존 자료 활용 맵

| 가이드 섹션 | 참조할 기존 자료 |
|------------|----------------|
| §1 스켈레톤 | Gateway/Auth/Chat main.cpp + 서비스 클래스에서 공통 패턴 추출 |
| §2 Server + TOML | `server.hpp`, 각 서비스 main.cpp (TOML 파싱 패턴), `*_e2e.toml` 설정 예시 |
| §3 라이프사이클 | `service_base.hpp`, `server.cpp` Phase 오케스트레이션, Gateway `on_session_closed` |
| §4 핸들러 + 메시지 | `service_base.hpp`, `wire_header.hpp`, `apex_shared/schemas/` (.fbs), 각 서비스 msg_ids |
| §5 어댑터 | Gateway/Auth/Chat on_configure() + 에러 핸들링 패턴, `circuit_breaker.hpp` |
| §6 메모리 | `bump_allocator.hpp`, `arena_allocator.hpp` |
| §7 유틸리티 | `cross_core_call.hpp`, `periodic_task_scheduler.hpp`, `session.hpp`, `result.hpp` |
| §8 금지사항 | `design-decisions.md` ADR + 서비스 코드 주석 + 탐색 결과 |
| §9 빌드 | `apex_services/auth-svc/CMakeLists.txt` (템플릿 베이스), `apex_services/CMakeLists.txt` |
| §10 내부 | `server.cpp`, `core_engine.cpp`, `connection_handler.hpp`, `message_dispatcher.hpp` |
| §10.5 ADR | `design-decisions.md`, `design-rationale.md` |
| §11 패턴 | `gateway_service.cpp`, `auth_service.cpp`, `chat_service.cpp` |

---

## 7. 스코프 외 (별도 백로그)

- **CLAUDE.md 중복 정리**: `docs/CLAUDE.md` 백로그 운영 규칙 80줄이 루트 CLAUDE.md와 부분 중복. 별도 백로그 항목으로 분리.
- **레이어 1 사람 튜토리얼 (B)**: 에이전트 레퍼런스 완성 후 여력에 따라 추가. 별도 판단.
- **서비스 테스트 작성 가이드**: 유닛 테스트(GTest + Mock 어댑터) + E2E 테스트 패턴. 가이드와 별개 문서로 분리.
- **auto-review 가이드 검증 자동화**: 코어 인터페이스 변경 시 가이드 갱신 누락을 auto-review 스크립트에서 자동 탐지. 유지보수 정책(§4)의 "auto-review 체크 항목"을 구체화.
