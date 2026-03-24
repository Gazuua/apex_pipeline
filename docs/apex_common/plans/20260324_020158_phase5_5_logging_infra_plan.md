# Phase 5.5 로깅 인프라 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ScopedLogger 기반 통일 로깅 인터페이스 구현 + 전 레이어(코어/shared/서비스) 로그 충실화

**Architecture:** `ScopedLogger` 클래스를 apex_core에 구현하고, `log_loc` 래퍼로 `std::source_location`을 자동 캡처한다. 기존 `log_helpers.hpp`(standalone 함수 5개)와 `APEX_SVC_LOG_METHODS`(매크로 15개 메서드)를 폐기하고, 모든 레이어에서 `logger_.info(...)` 통일 인터페이스를 사용한다. spdlog 패턴은 변경하지 않고, source_location 정보를 `%v` 메시지에 수동 포맷한다.

**Tech Stack:** C++23, spdlog (async), `std::source_location`, fmt, Google Test

**설계 문서:** `docs/apex_common/plans/20260324_014244_phase5_5_logging_infra_design.md`
**백로그:** BACKLOG-161, BACKLOG-174

---

## 파일 구조

### 신규 생성
| 파일 | 책임 |
|------|------|
| `apex_core/include/apex/core/scoped_logger.hpp` | ScopedLogger + log_loc 정의 (헤더 전용 템플릿 + 선언) |
| `apex_core/src/scoped_logger.cpp` | log_impl() 구현 (포맷 로직) |
| `apex_core/tests/unit/test_scoped_logger.cpp` | ScopedLogger 단위 테스트 |

### 주요 수정
| 파일 | 변경 내용 |
|------|-----------|
| `apex_core/CMakeLists.txt` | `src/scoped_logger.cpp` 추가 |
| `apex_core/tests/CMakeLists.txt` | `test_scoped_logger.cpp` 추가 |
| `apex_core/include/apex/core/log_helpers.hpp` | **삭제** |
| `apex_core/src/session_manager.cpp` | `apex::core::log::*` → `logger_.info(...)` (3건) |
| `apex_core/src/spsc_mesh.cpp` | `apex::core::log::trace` → `logger_.trace(...)` (1건) |
| `apex_core/src/core_engine.cpp` | include 제거 + ScopedLogger 멤버 추가 |
| `apex_core/include/apex/core/core_engine.hpp` | ScopedLogger 멤버 추가 + `spdlog::warn` 직접 호출 3건 마이그레이션 |
| `apex_core/include/apex/core/listener.hpp` | `spdlog::warn` 직접 호출 2건 마이그레이션 |
| `apex_core/include/apex/core/service_base.hpp` | APEX_SVC_LOG_METHODS 삭제 + ScopedLogger 멤버 + `spdlog::warn/error` 직접 호출 2건 마이그레이션 |
| `apex_core/src/server.cpp` | `spdlog::*` 직접 호출 14건 마이그레이션 |
| `apex_core/src/tcp_acceptor.cpp` | `spdlog::*` 직접 호출 2건 마이그레이션 |
| `apex_core/src/message_dispatcher.cpp` | `spdlog::*` 직접 호출 5건 마이그레이션 |
| `apex_core/src/bump_allocator.cpp` | `spdlog::*` 직접 호출 1건 마이그레이션 |
| `apex_services/auth-svc/src/auth_service.cpp` | `log_*` 6건 + `spdlog::*` ~43건 마이그레이션 |
| `apex_services/chat-svc/src/chat_service.cpp` | `log_*` 4건 + `spdlog::*` 직접 호출 마이그레이션 |
| `apex_services/gateway/src/*.cpp` | `spdlog::*` 직접 호출 전량 마이그레이션 (gateway_service + jwt_verifier + response_dispatcher + session_store + pubsub_listener + config_reloader) |
| (+ 로그 0건 파일 18개) | ScopedLogger 멤버 추가 + 레벨별 로그 심기 |

### 마이그레이션 범위 결정
**`spdlog::*` 직접 호출 전량 마이그레이션** — `log_*` 매크로뿐 아니라 `spdlog::info(...)` 직접 호출도 모두 `logger_.info(...)`로 전환. 예외: `main.cpp`의 서버 시작/종료 로그는 로컬 ScopedLogger 인스턴스 사용 (`ScopedLogger logger{"Main", ScopedLogger::NO_CORE, "app"};`).

### 로거 라우팅 변경 노트
현재 코어 코드의 `spdlog::info(...)` 직접 호출은 기본 로거("app")를 사용한다. ScopedLogger 전환 후 코어 코드는 `"apex"` 로거를 사용하게 되어 **로그 라우팅이 변경**된다. 이 변경은 의도적 — 설계 문서 §3.2에서 프레임워크 코어="apex", 서비스 코드="app" 분리를 명시. `logging.cpp`에서 두 로거 모두 동일 sink을 공유하므로 실질적 출력 차이는 로거 이름 태그(`[apex]` vs `[app]`)뿐.

---

## Task 1: ScopedLogger 클래스 구현 + 단위 테스트

> ScopedLogger의 핵심 — log_loc 래퍼, 6레벨 × 3오버로드, with_trace(), NO_CORE.
> **중요**: log_loc + variadic template 조합이 MSVC/GCC 양쪽에서 컴파일되는지 반드시 검증.
> 설계 문서 §3.1~§3.4 참조.

**Files:**
- Create: `apex_core/include/apex/core/scoped_logger.hpp`
- Create: `apex_core/src/scoped_logger.cpp`
- Create: `apex_core/tests/unit/test_scoped_logger.cpp`
- Modify: `apex_core/CMakeLists.txt` (라인 58-79 `add_library` 목록에 `src/scoped_logger.cpp` 추가)
- Modify: `apex_core/tests/CMakeLists.txt` (`test_scoped_logger.cpp` 추가)

**참고 파일:**
- `apex_core/include/apex/core/assert.hpp` — `std::source_location` 사용 패턴 참조
- `apex_core/tests/unit/test_logging.cpp` — 기존 로깅 테스트 패턴 참조 (LoggingTest 클래스, init_logging/shutdown_logging)
- `apex_core/include/apex/core/session.hpp:188` — `using SessionPtr = boost::intrusive_ptr<Session>;` 타입 확인
- `apex_core/include/apex/core/session.hpp:119-126` — `Session::id()`, `Session::core_id()` 접근자 확인

### 구현 순서

- [ ] **Step 1**: `scoped_logger.hpp` 작성 — `log_loc` 구조체, `ScopedLogger` 클래스 선언 (설계 §3.2 기준). `log_loc`의 `constexpr` 생성자 + `std::source_location::current()` 디폴트가 MSVC/GCC에서 동작하는지 확인이 핵심. 동작하지 않을 경우 대안: (A) 포맷 스트링 래퍼에 source_location을 캡처하는 `std::type_identity_t` 기법, (B) 매크로 래퍼 `APEX_LOG(logger, level, ...)` 폴백.
- [ ] **Step 2**: `scoped_logger.cpp` 작성 — `log_impl()` 구현. `%v`에 `[file:line func] [core=N][component]` 포맷을 수동 삽입. `NO_CORE` 시 `[core=N]` 태그 생략. `corr_id != 0` 시 `[trace=hex]` 추가. 세션/메시지ID 오버로드별 포맷 분기. `spdlog::get(logger_name)` 으로 명명된 로거 캐시. `file_name()` 결과에서 경로 제거 (basename만 출력).
- [ ] **Step 3**: `test_scoped_logger.cpp` 작성 — 테스트 케이스:
  - `ScopedLogger_BasicInfo`: 기본 로그 출력 포맷 검증 (`[file:line func] [core=0][TestComponent] message`)
  - `ScopedLogger_WithSession`: 세션 컨텍스트 포맷 검증 (`[sess=N]`)
  - `ScopedLogger_WithSessionAndMsgId`: 세션+메시지ID 포맷 검증 (`[sess=N][msg=0xHHHH]`)
  - `ScopedLogger_WithTrace`: `with_trace()` 후 `[trace=hex]` 태그 검증
  - `ScopedLogger_NoCoreContext`: NO_CORE 사용 시 `[core=N]` 생략 검증
  - `ScopedLogger_LevelFiltering`: `should_log()` 체크 — 비활성 레벨에서 포맷 스킵 검증
  - `ScopedLogger_ApexVsAppLogger`: "apex"와 "app" 로거 선택 검증
  - 테스트 패턴: `spdlog::sinks::ostream_sink_mt`로 출력 캡처 → 문자열 매칭
- [ ] **Step 4**: `logging.cpp` 호환성 확인 — `apex_core/src/logging.cpp`에서 `spdlog::get("apex")`, `spdlog::get("app")`이 ScopedLogger 생성자에서 정상 반환되는지 확인. `init_logging()` 이후 호출 순서 검증. 필요 시 `logging.cpp`에 호환성 코드 추가.
- [ ] **Step 5**: `CMakeLists.txt` 수정 — `add_library(apex_core STATIC ...)` 목록에 `src/scoped_logger.cpp` 추가. `tests/CMakeLists.txt`에 `test_scoped_logger.cpp` 추가.
- [ ] **Step 6**: 빌드 + 테스트 실행 — `run-hook queue build debug`. 테스트만: `ctest --test-dir build/debug -R test_scoped_logger -V`
- [ ] **Step 7**: 커밋 — `feat(core): BACKLOG-161 ScopedLogger 클래스 구현 + 단위 테스트`

---

## Task 2: 코어 프레임워크 마이그레이션 (log_helpers.hpp → ScopedLogger)

> 기존 standalone 로깅 함수를 ScopedLogger로 교체. 영향 범위가 작음 — 4건의 호출만 존재.
> 설계 문서 §5.1 참조.

**Files:**
- Delete: `apex_core/include/apex/core/log_helpers.hpp`
- Modify: `apex_core/src/session_manager.cpp` (라인 5: include 제거, 라인 36/50/60: 3건의 `log::*` 호출 교체)
- Modify: `apex_core/src/spsc_mesh.cpp` (라인 7: include 제거, 라인 85: 1건의 `log::trace` 호출 교체)
- Modify: `apex_core/src/core_engine.cpp` (라인 8: include 제거)
- Modify: `apex_core/include/apex/core/core_engine.hpp` (ScopedLogger 멤버 추가)
- Modify: `apex_core/include/apex/core/session_manager.hpp` (ScopedLogger 멤버 추가)
- Modify: `apex_core/include/apex/core/spsc_mesh.hpp` (ScopedLogger 멤버 추가)
- Modify: `apex_core/include/apex/core/core_engine.hpp` (ScopedLogger 멤버 + `spdlog::warn` 인라인 3건)
- Modify: `apex_core/include/apex/core/listener.hpp` (ScopedLogger 멤버 + `spdlog::warn` 인라인 2건)
- Modify: `apex_core/src/server.cpp` (ScopedLogger + `spdlog::*` 14건)
- Modify: `apex_core/src/tcp_acceptor.cpp` (`spdlog::*` 2건)
- Modify: `apex_core/src/message_dispatcher.cpp` (`spdlog::*` 5건)
- Modify: `apex_core/src/bump_allocator.cpp` (`spdlog::*` 1건)

### 구현 순서

- [ ] **Step 1**: 각 코어 클래스 헤더에 `ScopedLogger logger_` 멤버 추가:
  - `core_engine.hpp`: `ScopedLogger logger_{"CoreEngine", core_id_};` + `spawn_tracked()`의 `spdlog::warn` 3건 → `logger_.warn(...)` 교체
  - `session_manager.hpp`: `ScopedLogger logger_{"SessionManager", core_id_};`
  - `spsc_mesh.hpp`: `ScopedLogger logger_{"SpscMesh", core_id_};` (per-core 가용 여부 확인, 불가 시 NO_CORE)
  - `listener.hpp`: `ScopedLogger logger_{"Listener", core_id_};` + `spdlog::warn` 2건 교체
  - `server.hpp` 또는 Server 클래스: `ScopedLogger logger_{"Server", ScopedLogger::NO_CORE};`
- [ ] **Step 2**: `session_manager.cpp` — `#include <apex/core/log_helpers.hpp>` 제거, `log::warn/info/debug` 3건 → `logger_.warn/info/debug` 교체.
- [ ] **Step 3**: `spsc_mesh.cpp` — include 제거, `log::trace` 1건 교체.
- [ ] **Step 4**: `core_engine.cpp` — include 제거.
- [ ] **Step 5**: `server.cpp` — `spdlog::*` 직접 호출 14건 → `logger_.*` 교체. `tcp_acceptor.cpp` — 2건 교체.
- [ ] **Step 6**: `message_dispatcher.cpp` — 5건, `bump_allocator.cpp` — 1건 교체.
- [ ] **Step 7**: `service_base.hpp` — 매크로 외 직접 `spdlog::warn/error` 2건 (FlatBuffers verify 실패, spawn 예외) → `logger_.warn/error` 교체.
- [ ] **Step 8**: `log_helpers.hpp` 파일 삭제.
- [ ] **Step 9**: 빌드 — `run-hook queue build debug`. `grep -r "log_helpers\|apex::core::log::" apex_core/ --include="*.cpp" --include="*.hpp"` 로 잔여 참조 0건 확인.
- [ ] **Step 10**: 기존 테스트 확인 — `test_logging.cpp`에서 `log_helpers` 의존이 있다면 수정.
- [ ] **Step 11**: 커밋 — `refactor(core): log_helpers.hpp 폐기, 코어 전체 ScopedLogger 마이그레이션`

---

## Task 3: ServiceBase 마이그레이션 (APEX_SVC_LOG_METHODS → ScopedLogger)

> ServiceBase의 매크로 기반 로깅을 ScopedLogger로 교체. 서비스 코드 호출부도 함께 수정해야 함.
> 설계 문서 §5.1 참조.

**Files:**
- Modify: `apex_core/include/apex/core/service_base.hpp` (라인 345-378: 매크로 삭제, ScopedLogger 멤버 추가)

**참고 파일:**
- `apex_core/include/apex/core/service_base.hpp:349-370` — 현재 APEX_SVC_LOG_METHODS 매크로 정의
- `apex_core/include/apex/core/service_base.hpp:372-378` — 5레벨 매크로 인스턴스화 + #undef

### 구현 순서

- [ ] **Step 1**: `service_base.hpp`에서 `APEX_SVC_LOG_METHODS` 매크로 정의(라인 349-370) + 5개 인스턴스화(라인 372-376) + `#undef`(라인 378) 전체 삭제.
- [ ] **Step 2**: ServiceBase 클래스에 `protected: ScopedLogger logger_;` 멤버 추가. 생성자 또는 `internal_configure()` 에서 `logger_ = ScopedLogger(name_, core_id_for_log(), "app");` 초기화. `core_id_for_log()`와 `name_` 초기화 시점 확인 필수 — `internal_configure()` 이후에 유효.
- [ ] **Step 3**: `#include <apex/core/scoped_logger.hpp>` 추가.
- [ ] **Step 4**: 빌드 시도 — 서비스 코드의 `log_info(...)` 호출이 모두 컴파일 에러가 됨. 이건 Task 4에서 수정하므로 여기서는 **빌드 에러 목록만 확인**.
- [ ] **Step 5**: 커밋하지 않음 — Task 4와 함께 커밋 (중간에 빌드가 깨지므로).

---

## Task 4: 서비스 코드 마이그레이션 (log_info → logger_.info)

> Task 3에서 매크로를 삭제했으므로 서비스 코드의 모든 `log_trace/debug/info/warn/error` 호출을
> `logger_.trace/debug/info/warn/error`로 교체. 기계적 치환이지만 파일 수가 많음.
> 설계 문서 §5.1 참조.

**Files:**
- Modify: `apex_services/auth-svc/src/auth_service.cpp` — `log_*` 6건 + `spdlog::*` 직접 ~43건
- Modify: `apex_services/chat-svc/src/chat_service.cpp` — `log_*` 4건 + `spdlog::*` 직접 호출
- Modify: `apex_services/gateway/src/gateway_service.cpp` — `spdlog::*` 직접 호출
- Modify: `apex_services/gateway/src/jwt_verifier.cpp` — 7건
- Modify: `apex_services/gateway/src/response_dispatcher.cpp` — 7건
- Modify: `apex_services/gateway/src/pubsub_listener.cpp` — 12건
- Modify: `apex_services/gateway/src/config_reloader.cpp` — 6건
- Modify: `apex_services/gateway/src/broadcast_fanout.cpp` — 2건
- Modify: `apex_services/gateway/src/gateway_config_parser.cpp` — 2건
- Modify: `apex_services/gateway/src/file_watcher.cpp` — 4건
- Modify: `apex_services/gateway/src/jwt_blacklist.cpp` — 2건
- Modify: `apex_services/gateway/src/route_table.cpp` — 2건
- Modify: `apex_services/gateway/src/message_router.cpp` — 1건
- Modify: `apex_services/gateway/src/pending_requests.cpp` — 1건
- Modify: `apex_services/auth-svc/src/jwt_manager.cpp` — 10건
- Modify: `apex_services/auth-svc/src/password_hasher.cpp` — 2건
- Modify: `apex_services/auth-svc/src/session_store.cpp` — 7건
- Modify: `apex_services/chat-svc/src/chat_db_consumer.cpp` — 2건
- Modify: `apex_services/*/src/main.cpp` — 로컬 ScopedLogger 인스턴스 사용

### 구현 순서

- [ ] **Step 1**: 전체 호출 목록 확보 — `grep -rn "log_trace\|log_debug\|log_info\|log_warn\|log_error\|spdlog::" apex_services/ --include="*.cpp"` 로 모든 대상 파악.
- [ ] **Step 2**: `log_*` 매크로 호출 치환 (Task 3에서 매크로 삭제 후 컴파일 에러 대상):
  - `log_trace(` → `logger_.trace(`
  - `log_debug(` → `logger_.debug(`
  - `log_info(` → `logger_.info(`
  - `log_warn(` → `logger_.warn(`
  - `log_error(` → `logger_.error(`
  - **`log_loc` 파라미터 방식에 따라 호출부 조정 필요** — Task 1 최종 API에 맞춤.
- [ ] **Step 3**: `spdlog::*` 직접 호출 마이그레이션 — 서비스 파일의 `spdlog::info(...)`, `spdlog::warn(...)` 등 전량 → `logger_.*` 교체. 기존 수동 컨텍스트 태그(`[AuthService]` 등)는 ScopedLogger가 자동 추가하므로 제거.
- [ ] **Step 4**: 서비스 보조 클래스 전량 마이그레이션:
  - **Gateway** (11개 파일): `jwt_verifier`, `response_dispatcher`, `pubsub_listener`, `config_reloader`, `broadcast_fanout`, `gateway_config_parser`, `file_watcher`, `jwt_blacklist`, `route_table`, `message_router`, `pending_requests` — 각각 ScopedLogger 멤버 추가 + `spdlog::*` 교체.
  - **Auth** (3개 파일): `jwt_manager`(10건), `password_hasher`(2건), `session_store`(7건) — 동일 패턴.
  - **Chat** (1개 파일): `chat_db_consumer`(2건) — 동일 패턴.
- [ ] **Step 5**: `main.cpp` 파일들 — 로컬 `ScopedLogger logger{"Main", ScopedLogger::NO_CORE, "app"};` 인스턴스로 서버 시작/종료 로그 교체.
- [ ] **Step 6**: 빌드 — `run-hook queue build debug`. 컴파일 에러 0건 확인.
- [ ] **Step 7**: 기존 테스트 실행 — `ctest --test-dir build/debug -V`. E2E 로그 포맷 의존 여부 확인.
- [ ] **Step 8**: clang-format 실행.
- [ ] **Step 9**: 커밋 (Task 3 + Task 4 통합) — `refactor(core,services): APEX_SVC_LOG_METHODS 폐기, 전체 서비스 ScopedLogger 마이그레이션`

---

## Task 5: apex_shared 어댑터 ScopedLogger 도입

> shared 레이어의 어댑터 클래스에 ScopedLogger 멤버를 추가한다.
> 백그라운드 스레드(Kafka consumer, Redis reconnection)에서는 NO_CORE 사용.
> 설계 문서 §3.2 NO_CORE, §6.3 참조.

**Files:**
- Modify: 어댑터 클래스 헤더/소스 — 전체 대상 10개 파일:
  - `kafka_adapter.cpp`, `kafka_consumer.cpp`, `consumer_payload_pool.cpp`, `kafka_producer.cpp`, `kafka_sink.cpp`
  - `pg_adapter.cpp`, `pg_connection.cpp`, `pg_pool.cpp`
  - `redis_adapter.cpp`, `redis_multiplexer.cpp`
- 각 어댑터 클래스에 `ScopedLogger logger_{"ClassName", NO_CORE};` 또는 `ScopedLogger logger_{"ClassName", core_id};` 멤버 추가

### 구현 순서

- [ ] **Step 1**: `grep -rn "spdlog::" apex_shared/lib/ --include="*.cpp"` 로 모든 직접 spdlog 호출 목록 확보.
- [ ] **Step 2**: 각 어댑터 클래스 헤더에 `ScopedLogger logger_` 멤버 추가. core_id 사용 가능 여부에 따라:
  - Per-core 컨텍스트: `ScopedLogger logger_{"KafkaAdapter", core_id};`
  - 백그라운드 스레드: `ScopedLogger logger_{"KafkaConsumer", ScopedLogger::NO_CORE};`
- [ ] **Step 3**: 소스 파일의 `spdlog::info(...)` → `logger_.info(...)` 교체. 기존 포맷 문자열에서 수동으로 넣던 컨텍스트 태그(`[KafkaAdapter]` 등)는 ScopedLogger가 자동 추가하므로 제거.
- [ ] **Step 4**: 빌드 + 테스트 — `run-hook queue build debug`.
- [ ] **Step 5**: clang-format 실행.
- [ ] **Step 6**: 커밋 — `refactor(shared): apex_shared 어댑터 ScopedLogger 마이그레이션`

---

## Task 6: 로그 충실화 — apex_core (10개 파일)

> 로그 0건인 코어 파일에 레벨별 로그를 심는다. 핫패스는 trace, 상태 전이는 debug, 생명주기는 info.
> 설계 문서 §6.1 레벨 기준, §6.2 대상 목록 참조.

**Files:**
- Modify: `apex_core/src/session.cpp` — 우선순위 높음
- Modify: `apex_core/src/cross_core_dispatcher.cpp` — 우선순위 높음
- Modify: `apex_core/src/frame_codec.cpp` — 중간
- Modify: `apex_core/src/config.cpp` — 중간
- Modify: `apex_core/src/periodic_task_scheduler.cpp` — 중간
- Modify: `apex_core/src/error_sender.cpp` — 중간
- Modify: `apex_core/src/arena_allocator.cpp` — 낮음
- Modify: `apex_core/src/slab_allocator.cpp` — 낮음
- Modify: `apex_core/src/ring_buffer.cpp` — 낮음
- Modify: `apex_core/src/wire_header.cpp` — 낮음

**참고 파일:**
- 설계 문서 §6.2 — 각 파일별 로그 포인트 목록
- `apex_core/include/apex/core/session.hpp` — Session 클래스 구조 (state enum, id, core_id)

### 구현 순서

- [ ] **Step 1**: 각 파일의 클래스에 ScopedLogger 멤버가 아직 없으면 추가. `session.cpp`의 경우 Session 클래스에 `ScopedLogger logger_{"Session", core_id_};` 추가 (헤더 수정).
- [ ] **Step 2**: `session.cpp` — 쓰기 큐 enqueue/drain (trace), 상태 전이 connected→closing→closed (debug), close 시퀀스 시작/종료 (info), 비정상 종료 (warn). 핵심 메서드: `enqueue_write`, `do_write`, `close`, `~Session`.
- [ ] **Step 3**: `cross_core_dispatcher.cpp` — 디스패치 시도 (trace), 성공 (debug), 핸들러 미등록 (warn). 메서드: `dispatch`, `register_handler`.
- [ ] **Step 4**: `frame_codec.cpp` — 프레임 파싱 완료 (trace: 바이트 수), 파싱 실패 (debug: 에러 종류). `config.cpp` — 설정 로드 성공 (info), 키 폴백 (debug).
- [ ] **Step 5**: `periodic_task_scheduler.cpp` — 태스크 등록/제거 (info), 실행 (trace). `error_sender.cpp` — 에러 전송 (debug), 전송 실패 (error).
- [ ] **Step 6**: 낮은 우선순위 파일들 — `arena_allocator.cpp`, `slab_allocator.cpp` (할당/해제 trace, 풀 고갈 warn), `ring_buffer.cpp` (push/pop trace, 오버플로우 warn), `wire_header.cpp` (파싱 실패 debug).
- [ ] **Step 7**: 빌드 + 테스트 — `run-hook queue build debug`.
- [ ] **Step 8**: clang-format 실행.
- [ ] **Step 9**: 커밋 — `feat(core): BACKLOG-161 코어 레이어 로그 충실화 — 10개 파일 레벨별 로그 추가`

---

## Task 7: 로그 충실화 — apex_shared (8개 파일)

> shared 레이어의 로그 0건 파일에 로그를 심는다.
> 설계 문서 §6.3 대상 목록 참조.

**Files:**
- Modify: `apex_shared/lib/common/src/circuit_breaker.cpp` — 우선순위 높음
- Modify: `apex_shared/lib/adapters/pg/src/pg_transaction.cpp` — 우선순위 높음
- Modify: `apex_shared/lib/adapters/redis/src/redis_connection.cpp` — 중간
- Modify: `apex_shared/lib/common/src/cancellation_token.cpp` — 중간
- Modify: `apex_shared/lib/adapters/redis/src/hiredis_asio_adapter.cpp` — 중간
- Modify: `apex_shared/lib/common/src/adapter_error.cpp` — 낮음
- Modify: `apex_shared/lib/adapters/kafka/src/kafka_config.cpp` — 낮음
- Modify: `apex_shared/lib/adapters/redis/src/redis_config.cpp` — 낮음

### 구현 순서

- [ ] **Step 1**: `circuit_breaker.cpp` — 상태 전이 closed→open→half-open (info), 트립 조건 (warn), 시도 결과 (debug).
- [ ] **Step 2**: `pg_transaction.cpp` — BEGIN/COMMIT/ROLLBACK (debug), 쿼리 실행 결과 (trace), 에러 (error).
- [ ] **Step 3**: 중간 우선순위 — `redis_connection.cpp` (연결/해제 info, 명령 실패 error), `cancellation_token.cpp` (발행 debug, 취소 info), `hiredis_asio_adapter.cpp` (이벤트 trace).
- [ ] **Step 4**: 낮은 우선순위 — `adapter_error.cpp` (에러 생성 trace), `kafka_config.cpp`/`redis_config.cpp` (설정 파싱 debug).
- [ ] **Step 5**: 빌드 + 테스트 — `run-hook queue build debug`.
- [ ] **Step 6**: clang-format 실행.
- [ ] **Step 7**: 커밋 — `feat(shared): BACKLOG-174 shared 레이어 로그 충실화 — 8개 파일 레벨별 로그 추가`

---

## Task 8: 로그 충실화 — apex_services

> 서비스 레이어 로그 보강. 이미 ~160문 존재하므로 빈 구간 위주로 추가.
> 설계 문서 §6.4 참조.

**Files:**
- Modify: `apex_services/gateway/src/gateway_service.cpp` — 라우팅, JWT, rate limit
- Modify: `apex_services/auth-svc/src/auth_service.cpp` — bcrypt, 토큰 흐름
- Modify: `apex_services/chat-svc/src/chat_service.cpp` — 방 관리, Kafka publish

### 구현 순서

- [ ] **Step 1**: Gateway — 라우팅 결정 msg_id → 토픽 매핑 (debug), JWT 검증 결과 (debug), rate limit 트리거 (warn), 세션 생명주기 (info).
- [ ] **Step 2**: Auth — bcrypt 검증 타이밍 상세 (trace), 토큰 발급/갱신 (debug), 블랙리스트 추가 (info), 에러 경로 (warn/error).
- [ ] **Step 3**: Chat — 방 생성/참가/퇴장 상태 전이 (debug), Kafka publish 결과 (trace), Redis pub/sub 이벤트 (trace).
- [ ] **Step 4**: 빌드 + 테스트 — `run-hook queue build debug`.
- [ ] **Step 5**: clang-format 실행.
- [ ] **Step 6**: 커밋 — `feat(services): BACKLOG-174 서비스 레이어 로그 충실화 — Gateway/Auth/Chat 보강`

---

## Task 9: 최종 검증 + 핸드오프 상태 전이

> 전체 빌드/테스트 최종 확인, 핸드오프 plan notify.

- [ ] **Step 1**: clang-format 전체 실행 — `find apex_core apex_shared apex_services \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) ! -name '*_generated.h' | xargs clang-format -i`
- [ ] **Step 2**: 전체 빌드 — `run-hook queue build debug`
- [ ] **Step 3**: 전체 테스트 — `ctest --test-dir build/debug -V`
- [ ] **Step 4**: 잔여 레거시 호출 확인 — `grep -rn "log_helpers\|APEX_SVC_LOG_METHODS\|log_trace\(\|log_debug\(\|log_info\(\|log_warn\(\|log_error\(" apex_core/ apex_shared/ apex_services/ --include="*.cpp" --include="*.hpp"` — 0건 확인. 추가로 `grep -rn "spdlog::" apex_core/src/ apex_shared/lib/ apex_services/ --include="*.cpp"` — ScopedLogger 내부와 `logging.cpp` 외에 직접 호출 0건 확인.
- [ ] **Step 5**: 커밋 (필요 시) — 최종 정리
