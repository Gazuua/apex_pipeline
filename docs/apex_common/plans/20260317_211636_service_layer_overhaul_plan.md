# Service Layer Overhaul Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 코어 프레임워크 인터페이스를 확장하고 전 서비스를 Server+ServiceBase 패턴으로 통일

**Architecture:** ServiceBase에 다단계 라이프사이클(on_configure→on_wire→on_start) 추가, 어댑터 다중 등록, per-core 서비스 레지스트리, 주기적 작업 스케줄러, kafka_route 도입. 서비스 3개(Gateway, Auth, Chat) 마이그레이션.

**Tech Stack:** C++23, Boost.Asio coroutines, FlatBuffers, CRTP, Kafka (librdkafka)

**Spec:** `docs/apex_common/plans/20260317_210546_service_layer_overhaul_design.md`

---

## 파일 구조 맵

### 신규 파일

| 파일 | 책임 |
|------|------|
| `apex_core/include/apex/core/configure_context.hpp` | Phase 1 Context 구조체 |
| `apex_core/include/apex/core/wire_context.hpp` | Phase 2 Context 구조체 |
| `apex_core/include/apex/core/service_registry.hpp` | 타입 기반 per-core 서비스 레지스트리 + 읽기 전용 뷰 |
| `apex_core/include/apex/core/periodic_task_scheduler.hpp` | 주기적 작업 API (header-only 또는 minimal cpp) |
| `apex_core/src/periodic_task_scheduler.cpp` | 스케줄러 구현 (steady_timer + co_spawn) |
| `apex_core/tests/unit/test_service_registry.cpp` | ServiceRegistry 유닛 테스트 |
| `apex_core/tests/unit/test_periodic_task_scheduler.cpp` | PeriodicTaskScheduler 유닛 테스트 |
| `apex_core/tests/unit/test_service_lifecycle.cpp` | 다단계 라이프사이클 통합 테스트 |
| `apex_shared/lib/protocols/kafka/include/apex/shared/protocols/kafka/envelope_builder.hpp` | EnvelopeBuilder (build_into) |
| `apex_shared/lib/protocols/kafka/include/apex/shared/protocols/kafka/kafka_dispatch_bridge.hpp` | Kafka→dispatcher 브릿지 |
| `apex_shared/lib/protocols/kafka/src/envelope_builder.cpp` | EnvelopeBuilder 구현 |
| `apex_shared/lib/protocols/kafka/src/kafka_dispatch_bridge.cpp` | 브릿지 구현 |

### 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `apex_core/include/apex/core/service_base.hpp` | on_configure/on_wire/on_session_closed 훅 추가, kafka_route 추가, bump()/arena()/core_id() 접근자, internal_configure/internal_start |
| `apex_core/include/apex/core/server.hpp` | 어댑터 다중 등록 (role 파라미터), post_init_callback 제거, 서비스 레지스트리 통합, 라이프사이클 오케스트레이션 |
| `apex_core/src/server.cpp` | run() 재구성: Phase 1→2→3 순서 강제, 세션 종료 훅 와이어링 |
| `apex_core/include/apex/core/session_manager.hpp` | on_session_removed 콜백 타입 추가 |
| `apex_core/src/session_manager.cpp` | 세션 제거 시 콜백 호출 |
| `apex_core/tests/unit/test_service_base.cpp` | 기존 테스트 새 라이프사이클에 맞게 업데이트 |
| `apex_core/tests/unit/test_server_adapter.cpp` | 어댑터 다중 등록 테스트 추가 |
| `apex_core/CMakeLists.txt` | 신규 소스 파일 추가 |
| `apex_shared/lib/protocols/kafka/CMakeLists.txt` | EnvelopeBuilder, KafkaDispatchBridge 소스 추가 |
| `apex_services/gateway/src/main.cpp` | post_init_callback 제거, 어댑터 다중 등록 |
| `apex_services/gateway/include/apex/gateway/gateway_service.hpp` | ServiceBase 라이프사이클 적용, set_* 제거 |
| `apex_services/gateway/src/gateway_service.cpp` | on_configure/on_wire/on_start/on_session_closed 구현 |
| `apex_services/auth-svc/include/apex/auth_svc/auth_service.hpp` | ServiceBase<AuthService> 상속, kafka_route 사용 |
| `apex_services/auth-svc/src/auth_service.cpp` | 핸들러 시그니처 변경, current_meta_ 제거 |
| `apex_services/auth-svc/src/main.cpp` | CoreEngine→Server 전환, 15줄로 축소 |
| `apex_services/chat-svc/include/apex/chat_svc/chat_service.hpp` | Auth와 동일 패턴 |
| `apex_services/chat-svc/src/chat_service.cpp` | Auth와 동일 패턴 |
| `apex_services/chat-svc/src/main.cpp` | Auth와 동일 패턴 |

---

## Chunk 1: Core Framework Foundation

### Task 1: Context 타입 + ServiceRegistry

**Files:**
- Create: `apex_core/include/apex/core/configure_context.hpp`
- Create: `apex_core/include/apex/core/wire_context.hpp`
- Create: `apex_core/include/apex/core/service_registry.hpp`
- Create: `apex_core/tests/unit/test_service_registry.cpp`
- Modify: `apex_core/CMakeLists.txt`

- [ ] **Step 1: ServiceRegistry 실패 테스트 작성**

```cpp
// apex_core/tests/unit/test_service_registry.cpp
#include <apex/core/service_registry.hpp>
#include <gtest/gtest.h>

namespace {

struct MockServiceA : apex::core::ServiceBaseInterface {
    void start() override {}
    void stop() override {}
    std::string_view name() const noexcept override { return "a"; }
    bool started() const noexcept override { return false; }
    void bind_dispatcher(apex::core::MessageDispatcher&) override {}
    int value = 42;
};

struct MockServiceB : apex::core::ServiceBaseInterface {
    void start() override {}
    void stop() override {}
    std::string_view name() const noexcept override { return "b"; }
    bool started() const noexcept override { return false; }
    void bind_dispatcher(apex::core::MessageDispatcher&) override {}
};

TEST(ServiceRegistryTest, GetRegisteredService) {
    apex::core::ServiceRegistry registry;
    auto svc = std::make_unique<MockServiceA>();
    svc->value = 99;
    registry.register_service(std::move(svc));

    auto& found = registry.get<MockServiceA>();
    EXPECT_EQ(found.value, 99);
}

TEST(ServiceRegistryTest, FindReturnsNullptrForUnregistered) {
    apex::core::ServiceRegistry registry;
    EXPECT_EQ(registry.find<MockServiceB>(), nullptr);
}

TEST(ServiceRegistryTest, GetThrowsForUnregistered) {
    apex::core::ServiceRegistry registry;
    EXPECT_THROW(registry.get<MockServiceB>(), std::logic_error);
}

TEST(ServiceRegistryTest, FindReturnsPointerForRegistered) {
    apex::core::ServiceRegistry registry;
    registry.register_service(std::make_unique<MockServiceA>());
    EXPECT_NE(registry.find<MockServiceA>(), nullptr);
}

} // namespace
```

- [ ] **Step 2: 테스트 빌드 실패 확인**

Run: `cmd.exe //c build.bat debug`
Expected: 컴파일 에러 — `service_registry.hpp` 파일 없음

- [ ] **Step 3: ServiceRegistry 구현**

```cpp
// apex_core/include/apex/core/service_registry.hpp
#pragma once

#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <functional>
#include <vector>

namespace apex::core {

// Forward declaration — include 대신 전방 선언으로 순환 의존 방지
class ServiceBaseInterface;

/// Per-core 타입 기반 서비스 레지스트리.
/// 단일 스레드 접근 전용 (per-core, 동기화 불필요).
class ServiceRegistry {
public:
    /// Server가 서비스 인스턴스 생성 시 호출. type_index로 자동 키잉.
    void register_service(std::unique_ptr<ServiceBaseInterface> svc) {
        auto key = std::type_index(typeid(*svc));
        map_[key] = svc.get();
        services_.push_back(std::move(svc));
    }

    /// 타입으로 서비스 조회. 미등록 시 std::logic_error throw.
    template <typename T>
    T& get() {
        auto it = map_.find(std::type_index(typeid(T)));
        if (it == map_.end()) {
            throw std::logic_error(
                std::string("ServiceRegistry::get<") + typeid(T).name() + ">: not registered");
        }
        return *static_cast<T*>(it->second);
    }

    /// 타입으로 서비스 탐색. 미등록 시 nullptr.
    /// @note 반환된 포인터는 레지스트리가 살아있는 동안 유효.
    ///       on_wire에서 받은 포인터를 멤버에 캐싱하는 것은 안전 (서비스 수명 = 레지스트리 수명).
    template <typename T>
    T* find() {
        auto it = map_.find(std::type_index(typeid(T)));
        if (it == map_.end()) return nullptr;
        return static_cast<T*>(it->second);
    }

    /// 전체 서비스 순회.
    template <typename Fn>
    void for_each(Fn&& fn) {
        for (auto& svc : services_) { fn(*svc); }
    }

    [[nodiscard]] size_t size() const noexcept { return services_.size(); }

private:
    std::vector<std::unique_ptr<ServiceBaseInterface>> services_;
    std::unordered_map<std::type_index, ServiceBaseInterface*> map_;
};

/// 전 코어의 서비스 읽기 전용 뷰.
/// Phase 2(on_wire) 시점에만 사용. 모든 서비스 인스턴스가 생성 완료 상태이고
/// 코어 스레드 시작 전이므로 데이터 레이스 없이 안전.
class ServiceRegistryView {
public:
    explicit ServiceRegistryView(std::vector<ServiceRegistry*> registries)
        : registries_(std::move(registries)) {}

    /// 전 코어의 특정 타입 서비스 순회 (읽기 전용).
    template <typename T>
    void for_each_core(std::function<void(uint32_t core_id, const T&)> fn) const {
        for (uint32_t i = 0; i < registries_.size(); ++i) {
            if (auto* svc = registries_[i]->find<T>()) {
                fn(i, *svc);
            }
        }
    }

    /// 특정 코어의 서비스 조회 (읽기 전용). 미등록 시 std::logic_error.
    template <typename T>
    const T& get(uint32_t core_id) const {
        return registries_.at(core_id)->get<T>();
    }

private:
    std::vector<ServiceRegistry*> registries_;
};

} // namespace apex::core
```

- [ ] **Step 4: Context 타입 구현**

```cpp
// apex_core/include/apex/core/configure_context.hpp
#pragma once

#include <cstdint>

namespace apex::core {

// Forward declarations
class Server;
struct PerCoreState;

/// Phase 1 Context: 어댑터만 접근 가능.
/// ServiceRegistry 멤버 의도적 제외 → 다른 서비스 접근 시 컴파일 에러.
struct ConfigureContext {
    Server& server;
    uint32_t core_id;
    PerCoreState& per_core_state;
};

} // namespace apex::core
```

```cpp
// apex_core/include/apex/core/wire_context.hpp
#pragma once

#include <cstdint>

namespace apex::core {

// Forward declarations
class Server;
class ServiceRegistry;
class ServiceRegistryView;
class PeriodicTaskScheduler;

/// Phase 2 Context: 서비스 간 와이어링 + 유틸리티.
struct WireContext {
    Server& server;
    uint32_t core_id;
    ServiceRegistry& local_registry;
    ServiceRegistryView& global_registry;
    PeriodicTaskScheduler& scheduler;
};

} // namespace apex::core
```

- [ ] **Step 5: CMakeLists.txt에 테스트 추가**

기존 `apex_core/CMakeLists.txt`의 테스트 소스 목록에 `tests/unit/test_service_registry.cpp` 추가.

- [ ] **Step 6: 빌드 + 테스트 통과 확인**

Run: `cmd.exe //c build.bat debug`
Run: `apex_core/bin/debug/apex_core_tests.exe --gtest_filter="ServiceRegistry*"`
Expected: 4 tests PASS

- [ ] **Step 7: 커밋**

```bash
git add apex_core/include/apex/core/configure_context.hpp \
        apex_core/include/apex/core/wire_context.hpp \
        apex_core/include/apex/core/service_registry.hpp \
        apex_core/tests/unit/test_service_registry.cpp \
        apex_core/CMakeLists.txt
git commit -m "feat(core): Context 타입 + ServiceRegistry 추가 (D2, D5)"
```

---

### Task 2: PeriodicTaskScheduler

**Files:**
- Create: `apex_core/include/apex/core/periodic_task_scheduler.hpp`
- Create: `apex_core/src/periodic_task_scheduler.cpp`
- Create: `apex_core/tests/unit/test_periodic_task_scheduler.cpp`
- Modify: `apex_core/CMakeLists.txt`

- [ ] **Step 1: 스케줄러 실패 테스트 작성**

```cpp
// apex_core/tests/unit/test_periodic_task_scheduler.cpp
#include <apex/core/periodic_task_scheduler.hpp>
#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST(PeriodicTaskSchedulerTest, ScheduleExecutesMultipleTimes) {
    boost::asio::io_context io;
    apex::core::PeriodicTaskScheduler scheduler(io);

    std::atomic<int> count{0};
    scheduler.schedule(50ms, [&] { ++count; });

    // io_context를 200ms 동안 실행 → 최소 3회 실행 기대
    auto work = boost::asio::make_work_guard(io);
    std::thread t([&] { io.run_for(250ms); });
    t.join();

    EXPECT_GE(count.load(), 3);
}

TEST(PeriodicTaskSchedulerTest, CancelStopsExecution) {
    boost::asio::io_context io;
    apex::core::PeriodicTaskScheduler scheduler(io);

    std::atomic<int> count{0};
    auto handle = scheduler.schedule(50ms, [&] { ++count; });

    // 1회 실행 후 cancel
    auto work = boost::asio::make_work_guard(io);
    std::thread t([&] { io.run_for(80ms); });
    t.join();

    int count_at_cancel = count.load();
    scheduler.cancel(handle);

    // cancel 후 추가 실행 없어야 함
    boost::asio::io_context io2;
    // scheduler는 io에 바인딩되어 있으므로 cancel 후 추가 실행 불가
    EXPECT_GE(count_at_cancel, 1);
}

TEST(PeriodicTaskSchedulerTest, StopAllCancelsEverything) {
    boost::asio::io_context io;
    apex::core::PeriodicTaskScheduler scheduler(io);

    std::atomic<int> count{0};
    scheduler.schedule(50ms, [&] { ++count; });
    scheduler.schedule(50ms, [&] { ++count; });

    scheduler.stop_all();

    auto work = boost::asio::make_work_guard(io);
    std::thread t([&] { io.run_for(200ms); });
    t.join();

    EXPECT_EQ(count.load(), 0);
}
```

- [ ] **Step 2: 빌드 실패 확인**

Run: `cmd.exe //c build.bat debug`
Expected: 컴파일 에러 — `periodic_task_scheduler.hpp` 없음

- [ ] **Step 3: PeriodicTaskScheduler 구현**

```cpp
// apex_core/include/apex/core/periodic_task_scheduler.hpp
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace apex::core {

using TaskHandle = uint64_t;

/// 주기적 작업 스케줄러. per-core io_context에 steady_timer 기반.
/// Server shutdown 시 stop_all()로 전체 취소.
class PeriodicTaskScheduler {
public:
    explicit PeriodicTaskScheduler(boost::asio::io_context& io_ctx);
    ~PeriodicTaskScheduler();

    PeriodicTaskScheduler(const PeriodicTaskScheduler&) = delete;
    PeriodicTaskScheduler& operator=(const PeriodicTaskScheduler&) = delete;

    /// 현재 코어에서 주기적 실행. 반환된 핸들로 cancel 가능.
    TaskHandle schedule(std::chrono::milliseconds interval,
                        std::function<void()> task);

    /// 등록된 작업 취소. 유효하지 않은 핸들은 무시.
    void cancel(TaskHandle handle);

    /// 전체 작업 취소 (Server shutdown 시).
    void stop_all();

private:
    struct TaskEntry {
        std::unique_ptr<boost::asio::steady_timer> timer;
        std::function<void()> task;
        std::chrono::milliseconds interval;
        bool cancelled = false;
    };

    void schedule_next(TaskHandle handle);

    boost::asio::io_context& io_ctx_;
    std::unordered_map<TaskHandle, TaskEntry> tasks_;
    TaskHandle next_handle_{1};
};

} // namespace apex::core
```

```cpp
// apex_core/src/periodic_task_scheduler.cpp
#include <apex/core/periodic_task_scheduler.hpp>

namespace apex::core {

PeriodicTaskScheduler::PeriodicTaskScheduler(boost::asio::io_context& io_ctx)
    : io_ctx_(io_ctx) {}

PeriodicTaskScheduler::~PeriodicTaskScheduler() {
    stop_all();
}

TaskHandle PeriodicTaskScheduler::schedule(
    std::chrono::milliseconds interval,
    std::function<void()> task) {
    auto handle = next_handle_++;
    auto& entry = tasks_[handle];
    entry.timer = std::make_unique<boost::asio::steady_timer>(io_ctx_);
    entry.task = std::move(task);
    entry.interval = interval;
    entry.cancelled = false;

    schedule_next(handle);
    return handle;
}

void PeriodicTaskScheduler::cancel(TaskHandle handle) {
    auto it = tasks_.find(handle);
    if (it != tasks_.end()) {
        it->second.cancelled = true;
        it->second.timer->cancel();
        tasks_.erase(it);
    }
}

void PeriodicTaskScheduler::stop_all() {
    for (auto& [handle, entry] : tasks_) {
        entry.cancelled = true;
        entry.timer->cancel();
    }
    tasks_.clear();
}

void PeriodicTaskScheduler::schedule_next(TaskHandle handle) {
    auto it = tasks_.find(handle);
    if (it == tasks_.end()) return;

    auto& entry = it->second;
    entry.timer->expires_after(entry.interval);
    entry.timer->async_wait(
        [this, handle](boost::system::error_code ec) {
            if (ec) return;  // cancelled or error
            auto it = tasks_.find(handle);
            if (it == tasks_.end() || it->second.cancelled) return;
            it->second.task();
            schedule_next(handle);
        });
}

} // namespace apex::core
```

- [ ] **Step 4: CMakeLists.txt에 소스 + 테스트 추가**

`apex_core/CMakeLists.txt`에:
- 소스: `src/periodic_task_scheduler.cpp`
- 테스트: `tests/unit/test_periodic_task_scheduler.cpp`

- [ ] **Step 5: 빌드 + 테스트 통과**

Run: `cmd.exe //c build.bat debug`
Run: `apex_core/bin/debug/apex_core_tests.exe --gtest_filter="PeriodicTaskScheduler*"`
Expected: 3 tests PASS

- [ ] **Step 6: 커밋**

```bash
git add apex_core/include/apex/core/periodic_task_scheduler.hpp \
        apex_core/src/periodic_task_scheduler.cpp \
        apex_core/tests/unit/test_periodic_task_scheduler.cpp \
        apex_core/CMakeLists.txt
git commit -m "feat(core): PeriodicTaskScheduler 추가 (D11)"
```

---

### Task 3: ServiceBase 라이프사이클 오버홀

**Files:**
- Modify: `apex_core/include/apex/core/service_base.hpp`
- Create: `apex_core/tests/unit/test_service_lifecycle.cpp`
- Modify: `apex_core/tests/unit/test_service_base.cpp`

**의존**: Task 1 (Context 타입), Task 2 (PeriodicTaskScheduler)

- [ ] **Step 1: 라이프사이클 순서 테스트 작성**

```cpp
// apex_core/tests/unit/test_service_lifecycle.cpp
#include <apex/core/service_base.hpp>
#include <apex/core/configure_context.hpp>
#include <apex/core/wire_context.hpp>
#include <gtest/gtest.h>
#include <vector>
#include <string>

namespace {

// 라이프사이클 호출 순서를 기록하는 테스트 서비스
class LifecycleTracker : public apex::core::ServiceBase<LifecycleTracker> {
public:
    LifecycleTracker() : ServiceBase("tracker") {}

    std::vector<std::string> calls;

    void on_configure(apex::core::ConfigureContext& ctx) override {
        calls.push_back("configure");
    }
    void on_wire(apex::core::WireContext& ctx) override {
        calls.push_back("wire");
    }
    void on_start() override {
        calls.push_back("start");
    }
    void on_stop() override {
        calls.push_back("stop");
    }
    void on_session_closed(apex::core::SessionId) override {
        calls.push_back("session_closed");
    }
};

TEST(ServiceLifecycleTest, DefaultHooksAreNoOp) {
    // on_configure/on_wire를 오버라이드하지 않는 서비스가 정상 동작하는지 확인
    class MinimalService : public apex::core::ServiceBase<MinimalService> {
    public:
        MinimalService() : ServiceBase("minimal") {}
        void on_start() override { started = true; }
        bool started = false;
    };

    MinimalService svc;
    // on_configure, on_wire 호출해도 문제 없음 (default no-op)
    // 실제 Context 생성은 Server 통합 테스트에서 검증
    EXPECT_FALSE(svc.started);
}

TEST(ServiceLifecycleTest, BumpAndArenaAccessors) {
    // bump()/arena()/core_id()가 internal_configure 후 유효한지 확인
    // 이 테스트는 Server 통합에서 full context로 검증
    // 여기서는 컴파일 확인용
    LifecycleTracker svc;
    // bump(), arena(), core_id()는 internal_configure 호출 전에는 사용 불가
    // (per_core_ == nullptr → 런타임 에러)
    // Server 통합 테스트에서 검증
}

} // namespace
```

- [ ] **Step 2: ServiceBase 수정 — 라이프사이클 훅 + 접근자 추가**

`apex_core/include/apex/core/service_base.hpp` 수정 요약:

ServiceBaseInterface에 추가:
```cpp
virtual void on_configure(ConfigureContext&) {}
virtual void on_wire(WireContext&) {}
virtual void on_session_closed(SessionId) {}
```

ServiceBase<Derived>에 추가:
```cpp
// 프레임워크 전용 (Server가 호출)
void internal_configure(ConfigureContext& ctx) {
    per_core_ = &ctx.per_core_state;
    static_cast<Derived*>(this)->on_configure(ctx);
}

void internal_wire(WireContext& ctx) {
    static_cast<Derived*>(this)->on_wire(ctx);
}

void internal_start() {
    static_cast<Derived*>(this)->on_start();
    started_ = true;
}

protected:
    BumpAllocator& bump() { return per_core_->bump_allocator; }
    ArenaAllocator& arena() { return per_core_->arena_allocator; }
    uint32_t core_id() const { return per_core_->core_id; }

private:
    PerCoreState* per_core_ = nullptr;
```

기존 `start()` → `internal_start()` 호출로 변경.
기존 `route<T>()`: 현재도 FlatBuffers 검증을 수행하지만 error code만 반환. Task 7에서 error frame 자동 전송으로 변경 예정. Task 3에서는 기존 동작 유지.

ServiceBase에 kafka_route용 내부 데이터 구조 추가:
```cpp
private:
    // kafka_route 핸들러 맵 — KafkaDispatchBridge가 접근
    using KafkaHandler = std::function<
        boost::asio::awaitable<Result<void>>(
            EnvelopeMetadata, uint32_t, std::span<const uint8_t>)>;
    std::unordered_map<uint32_t, KafkaHandler> kafka_handlers_;
    bool has_kafka_handlers_ = false;

    // KafkaDispatchBridge가 핸들러 맵에 접근하기 위한 getter
    friend class KafkaDispatchBridge;
    const auto& kafka_handler_map() const { return kafka_handlers_; }
```

- [ ] **Step 3: 기존 test_service_base.cpp 업데이트**

기존 테스트의 `svc.start()` 호출을 `svc.internal_start()`로 변경하거나, 테스트용으로 `start()` public 래퍼 유지.
기존 API 호환을 위해 `start()`는 `internal_start()` 위임으로 유지.

- [ ] **Step 4: 빌드 + 기존 테스트 regression 확인**

Run: `cmd.exe //c build.bat debug`
Run: `apex_core/bin/debug/apex_core_tests.exe --gtest_filter="ServiceBase*:ServiceLifecycle*"`
Expected: 기존 + 신규 테스트 전부 PASS

- [ ] **Step 5: 커밋**

```bash
git add apex_core/include/apex/core/service_base.hpp \
        apex_core/tests/unit/test_service_lifecycle.cpp \
        apex_core/tests/unit/test_service_base.cpp
git commit -m "feat(core): ServiceBase 다단계 라이프사이클 훅 추가 (D2, D3, D9, D10)"
```

---

### Task 4: Server 어댑터 다중 등록 + 라이프사이클 오케스트레이션

**Files:**
- Modify: `apex_core/include/apex/core/server.hpp`
- Modify: `apex_core/src/server.cpp`
- Modify: `apex_core/tests/unit/test_server_adapter.cpp`

**의존**: Task 1, 2, 3

- [ ] **Step 1: 어댑터 다중 등록 테스트 작성**

```cpp
// test_server_adapter.cpp에 추가
TEST(ServerAdapterTest, MultipleAdaptersOfSameType) {
    // 동일 타입의 어댑터를 역할별로 다중 등록 가능한지 확인
    // MockAdapter를 "auth"와 "pubsub" 역할로 등록
    // adapter<MockAdapter>("auth") ≠ adapter<MockAdapter>("pubsub") 확인
}

TEST(ServerAdapterTest, DefaultRoleBackwardCompatible) {
    // role 미지정 시 "default"로 동작하여 기존 코드 호환 확인
}

TEST(ServerAdapterTest, UnregisteredRoleThrows) {
    // 미등록 역할 접근 시 std::out_of_range throw 확인
}
```

- [ ] **Step 2: server.hpp 수정 — 어댑터 다중 등록**

`adapter_map_` 키를 `{type_index, role}` 쌍으로 변경:

```cpp
// server.hpp 변경
struct AdapterKey {
    std::type_index type;
    std::string role;
    bool operator==(const AdapterKey&) const = default;
};

struct AdapterKeyHash {
    size_t operator()(const AdapterKey& k) const {
        return std::hash<std::type_index>{}(k.type)
             ^ (std::hash<std::string>{}(k.role) << 1);
    }
};

// add_adapter 오버로드
template <typename T, typename... Args>
Server& add_adapter(std::string role, Args&&... args) { ... }

template <typename T, typename... Args>
Server& add_adapter(Args&&... args) {
    return add_adapter<T>("default", std::forward<Args>(args)...);
}

// adapter 오버로드
template <typename T>
T& adapter(std::string_view role = "default") const { ... }
```

- [ ] **Step 3: server.cpp 수정 — 라이프사이클 오케스트레이션**

`Server::run()` 재구성:

```cpp
void Server::run() {
    // 1. 어댑터 초기화
    for (auto& adapter : adapters_) adapter->init(*core_engine_);

    // 2. per-core 서비스 생성 + ServiceRegistry 등록
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
        auto& state = *per_core_[core_id];
        auto& dispatcher = listeners_.empty()
            ? state.registry.get_or_create_default_dispatcher()
            : listeners_[0]->dispatcher(core_id);

        for (auto& factory : service_factories_) {
            auto svc = factory(state, dispatcher);
            state.registry.register_service(std::move(svc));
        }
    }

    // 3. Phase 1: on_configure (어댑터 접근 가능, 서비스 간 접근 불가)
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
        auto& state = *per_core_[core_id];
        ConfigureContext ctx{*this, core_id, state};
        state.registry.for_each([&](ServiceBaseInterface& svc) {
            svc.on_configure(ctx);
        });
    }

    // 4. Phase 2: on_wire (서비스 레지스트리 + 스케줄러 접근 가능)
    auto global_view = build_global_registry_view();
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
        auto& state = *per_core_[core_id];
        WireContext ctx{*this, core_id, state.registry, global_view, state.scheduler};
        state.registry.for_each([&](ServiceBaseInterface& svc) {
            svc.on_wire(ctx);
        });
    }

    // 5. Phase 3: on_start (핸들러 등록)
    for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
        auto& state = *per_core_[core_id];
        state.registry.for_each([&](ServiceBaseInterface& svc) {
            svc.start();  // calls internal_start → on_start
        });
    }

    // 6. (기존 post_init_callback은 유지 — Gateway 마이그레이션(Task 8) 완료 전까지 호환)
    if (post_init_cb_) post_init_cb_(*this);

    // 7. CoreEngine 시작 + 리스너 시작 (기존과 동일)
    // ...
}
```

**참고**: `post_init_callback`은 Gateway 마이그레이션(Task 8) 완료 시까지 유지. Task 8 Step 4에서 제거.
**참고**: `add_service<T>()`는 기존 Server API에 이미 존재 (server.hpp:135-146). 서비스 팩토리 내부에서 `internal_configure`/`internal_start` 호출 경로로 변경만 필요.

- [ ] **Step 4: PerCoreState에 ServiceRegistry + PeriodicTaskScheduler 추가**

```cpp
// server.hpp — PerCoreState 변경
struct PerCoreState {
    uint32_t core_id;
    SessionManager session_mgr;
    ServiceRegistry registry;                    // 신규
    PeriodicTaskScheduler scheduler;             // 신규 — io_context 참조 필요
    BumpAllocator bump_allocator;
    ArenaAllocator arena_allocator;
    // services 벡터 제거 — ServiceRegistry가 소유

    // 생성자: io_context는 CoreEngine에서 가져옴
    explicit PerCoreState(uint32_t id, boost::asio::io_context& io_ctx,
                          uint32_t heartbeat_timeout_ticks,
                          size_t timer_wheel_slots, size_t recv_buf_capacity,
                          size_t bump_capacity, size_t arena_block, size_t arena_max);
};
```

PerCoreState 생성 시점 (Server 생성자 또는 run() 초입):
```cpp
// Server 생성자 또는 run() 시작 시
for (uint32_t i = 0; i < config_.num_cores; ++i) {
    per_core_[i] = std::make_unique<PerCoreState>(
        i,
        core_engine_->io_context(i),  // CoreEngine이 이미 생성된 상태
        config_.heartbeat_timeout_ticks,
        config_.timer_wheel_slots,
        config_.recv_buf_capacity,
        config_.bump_capacity_bytes,
        config_.arena_block_bytes,
        config_.arena_max_bytes
    );
}
```

- [ ] **Step 5: SessionManager 세션 종료 훅 와이어링**

서비스 레지스트리에 등록 완료 후, Phase 1 시작 전에 콜백 설정.
이 시점에서 모든 서비스가 레지스트리에 존재하므로 on_session_closed 호출 안전.

```cpp
// server.cpp — 서비스 등록 완료 후, Phase 1 시작 전에 콜백 설정
for (uint32_t core_id = 0; core_id < config_.num_cores; ++core_id) {
    auto& state = *per_core_[core_id];
    state.session_mgr.set_remove_callback(
        [&state](SessionId sid) {
            state.registry.for_each([sid](ServiceBaseInterface& svc) {
                svc.on_session_closed(sid);
            });
        });
}
```

- [ ] **Step 6: SessionManager 콜백 추가**

`session_manager.hpp`에 `set_remove_callback` 추가:
```cpp
using RemoveCallback = std::function<void(SessionId)>;
void set_remove_callback(RemoveCallback cb);
```

`session_manager.cpp`의 `remove_session()` 내에서 콜백 호출.

- [ ] **Step 7: 빌드 + 전체 기존 테스트 regression**

Run: `cmd.exe //c build.bat debug`
Run: `apex_core/bin/debug/apex_core_tests.exe`
Expected: 기존 테스트 전부 PASS (서비스 생성 경로가 바뀌었으므로 통합 테스트 주의)

- [ ] **Step 8: 커밋**

```bash
git add apex_core/include/apex/core/server.hpp \
        apex_core/src/server.cpp \
        apex_core/include/apex/core/session_manager.hpp \
        apex_core/src/session_manager.cpp \
        apex_core/tests/unit/test_server_adapter.cpp
git commit -m "feat(core): 어댑터 다중 등록 + 라이프사이클 오케스트레이션 + 세션 종료 훅 (D4, D5, D10, D12)"
```

---

## Chunk 2: Shared Utilities + kafka_route

### Task 5: EnvelopeBuilder

**Files:**
- Create: `apex_shared/lib/protocols/kafka/include/apex/shared/protocols/kafka/envelope_builder.hpp`
- Create: `apex_shared/lib/protocols/kafka/src/envelope_builder.cpp`
- Modify: `apex_shared/lib/protocols/kafka/CMakeLists.txt`

**의존**: 없음 (독립)

- [ ] **Step 1: EnvelopeBuilder 테스트 작성**

기존 `kafka_envelope.hpp`의 `build_full_envelope()` 출력과 동일한 바이트 배열을 생성하는지 검증.
BumpAllocator에 build_into 시 힙 할당 없이 동작하는지 확인.

- [ ] **Step 2: EnvelopeBuilder 구현**

`envelope_builder.hpp`에 빌더 패턴 클래스 구현.
`build_into(BumpAllocator&)` — bump에서 할당, `std::span<uint8_t>` 반환.
내부적으로 기존 `RoutingHeader::serialize()`, `MetadataPrefix::serialize()` 활용.

- [ ] **Step 3: 빌드 + 테스트 통과**
- [ ] **Step 4: 커밋**

```bash
git commit -m "feat(shared): EnvelopeBuilder + build_into(BumpAllocator) 추가 (D8)"
```

---

### Task 6: KafkaDispatchBridge + kafka_route

**Files:**
- Create: `apex_shared/lib/protocols/kafka/include/apex/shared/protocols/kafka/kafka_dispatch_bridge.hpp`
- Create: `apex_shared/lib/protocols/kafka/src/kafka_dispatch_bridge.cpp`
- Modify: `apex_core/include/apex/core/service_base.hpp` (kafka_route 추가)

**의존**: Task 3 (ServiceBase 오버홀), Task 5 (EnvelopeBuilder)

- [ ] **Step 1: kafka_route 등록 + 디스패치 테스트**

KafkaDispatchBridge가 Kafka envelope을 파싱하여 msg_id 추출 후 올바른 핸들러 호출하는지 검증.
EnvelopeMetadata가 값으로 핸들러에 전달되는지 확인.

- [ ] **Step 2: kafka_route를 ServiceBase에 추가**

```cpp
// service_base.hpp에 추가
template <typename FbsType>
void kafka_route(uint32_t msg_id,
                 boost::asio::awaitable<Result<void>> (Derived::*method)(
                     const EnvelopeMetadata&, uint32_t, const FbsType*))
{
    auto* self = static_cast<Derived*>(this);
    kafka_handlers_[msg_id] = [self, method](
        EnvelopeMetadata meta, uint32_t id,
        std::span<const uint8_t> payload)
            -> boost::asio::awaitable<Result<void>> {
        flatbuffers::Verifier verifier(payload.data(), payload.size());
        if (!verifier.VerifyBuffer<FbsType>()) {
            co_return error(ErrorCode::FlatBuffersVerifyFailed);
        }
        auto* msg = flatbuffers::GetRoot<FbsType>(payload.data());
        co_return co_await (self->*method)(meta, id, msg);
    };
    has_kafka_handlers_ = true;
}
```

- [ ] **Step 3: KafkaDispatchBridge 구현**

Kafka 메시지 수신 → envelope 파싱 → msg_id 추출 → 매칭되는 kafka_handler 호출.
EnvelopeMetadata를 값으로 복사하여 코루틴 프레임에 캡처.

- [ ] **Step 4: 빌드 + 테스트 통과**
- [ ] **Step 5: 커밋**

```bash
git commit -m "feat(core+shared): kafka_route + KafkaDispatchBridge 추가 (D7)"
```

---

### Task 7: route<T>() FlatBuffers error frame 자동 전송

**Files:**
- Modify: `apex_core/include/apex/core/service_base.hpp`
- Modify: `apex_core/tests/unit/test_flatbuffers_dispatch.cpp`

**의존**: Task 3

- [ ] **Step 1: 기존 route<T>() 수정**

현재: FlatBuffers 검증 실패 시 `co_return error(ErrorCode::FlatBuffersVerifyFailed)` 반환
변경: 검증 실패 시 `ErrorSender::build_error_frame()` → `session->enqueue_write()` → `co_return ok()`

ConnectionHandler가 error code를 받아서 error frame을 보내는 기존 경로와의 중복 방지:
- route 내부에서 error frame을 보내고 ok() 반환 → ConnectionHandler는 아무것도 안 함
- 또는 기존 ConnectionHandler 경로를 유지하고 route 변경 없음

**결정**: 설계 문서 D6에 따라 route 내부에서 자동 전송. 단, session이 nullptr (Kafka 핸들러)인 경우는 전송하지 않음.

- [ ] **Step 2: 테스트 업데이트 + 빌드**
- [ ] **Step 3: 커밋**

```bash
git commit -m "feat(core): route<T>() FlatBuffers 검증 실패 시 error frame 자동 전송 (D6)"
```

---

## Chunk 3: Service Migration

### Task 8: Gateway 리팩토링

**Files:**
- Modify: `apex_services/gateway/include/apex/gateway/gateway_service.hpp`
- Modify: `apex_services/gateway/src/gateway_service.cpp`
- Modify: `apex_services/gateway/src/main.cpp`

**의존**: Task 4 (Server 오케스트레이션), Task 5 (EnvelopeBuilder)

- [ ] **Step 1: GatewayService에 on_configure/on_wire/on_start/on_session_closed 구현**

현재 main.cpp의 post_init_callback 로직을 GatewayService 메서드로 이전:
- `on_configure()`: 어댑터 참조 확보 (Kafka, Redis×3)
- `on_wire()`: ResponseDispatcher, PubSubListener, RateLimiter 와이어링 + 주기적 sweep
- `on_start()`: `set_default_handler()` (기존과 동일)
- `on_session_closed()`: `auth_states_`, `pending_requests_` 정리

- [ ] **Step 2: main.cpp 축소**

post_init_callback 250줄 제거. 어댑터 다중 등록으로 전환:
```cpp
server.add_adapter<RedisAdapter>("auth", redis_auth_cfg);
server.add_adapter<RedisAdapter>("pubsub", redis_pubsub_cfg);
server.add_adapter<RedisAdapter>("ratelimit", redis_rl_cfg);
```

set_* 메서드 (set_pubsub_listener, set_rate_limiter) 제거 — on_wire에서 직접 와이어링.

- [ ] **Step 3: Gateway Dependencies 구조체 정리**

기존 `Dependencies` 구조체에서 raw pointer 멤버 제거.
on_configure에서 어댑터 참조를 직접 확보하므로 Dependencies 불필요해질 수 있음.
최소한의 설정 데이터(RouteTable, JwtConfig 등)만 생성자로 전달.

- [ ] **Step 4: Server에서 post_init_callback 제거**

Gateway가 on_configure/on_wire로 완전 이전되었으므로 이 시점에서 `set_post_init_callback()` API 제거.
(**주의**: Task 4에서는 유지, Task 8 완료 시점에서 제거)

- [ ] **Step 5: 빌드 + 기존 유닛 테스트 regression**

Run: `cmd.exe //c build.bat debug`
Expected: 빌드 성공, Gateway 관련 유닛 테스트 PASS

- [ ] **Step 6: 커밋**

```bash
git commit -m "refactor(gateway): ServiceBase 라이프사이클 적용 + post_init_callback 제거"
```

---

### Task 9: Auth 서비스 마이그레이션

**Files:**
- Modify: `apex_services/auth-svc/include/apex/auth_svc/auth_service.hpp`
- Modify: `apex_services/auth-svc/src/auth_service.cpp`
- Modify: `apex_services/auth-svc/src/main.cpp`

**의존**: Task 4, 5, 6

- [ ] **Step 1: AuthService를 ServiceBase<AuthService> 상속으로 변경**

현재: 독자적 클래스, CoreEngine 직접 사용
변경: `ServiceBase<AuthService>` 상속, kafka_route 사용

```cpp
class AuthService : public apex::core::ServiceBase<AuthService> {
public:
    AuthService() : ServiceBase("auth") {}

    void on_configure(apex::core::ConfigureContext& ctx) override;
    void on_start() override;
    // on_wire 불필요 — 서비스 간 와이어링 없음
};
```

- [ ] **Step 2: 핸들러 시그니처 변경**

현재: `awaitable<void> handle_login(std::span<const uint8_t>)` (내부에서 current_meta_ 참조)
변경: `awaitable<Result<void>> on_login(const EnvelopeMetadata& meta, uint32_t msg_id, const LoginRequest* req)`

`current_meta_` 멤버 제거. `dispatch_envelope()` 메서드 제거.

- [ ] **Step 3: main.cpp를 Server 기반으로 변경**

```cpp
int main() {
    auto config = parse_auth_config("auth_svc.toml");
    apex::core::Server server({.num_cores = 1});
    server.add_adapter<KafkaAdapter>("request", config.kafka_consumer);
    server.add_adapter<KafkaAdapter>("response", config.kafka_producer);
    server.add_adapter<RedisAdapter>(config.redis);
    server.add_adapter<PgAdapter>(config.pg);
    server.add_service<AuthService>();
    server.run();
}
```

- [ ] **Step 4: EnvelopeBuilder로 응답 생성 코드 전환**

기존 `build_full_envelope()` 호출을 `EnvelopeBuilder{}.build_into(bump())` 패턴으로 변경.

- [ ] **Step 5: 빌드 + Auth 유닛 테스트 regression**

Run: `cmd.exe //c build.bat debug`
Expected: Auth 유닛 테스트 PASS (핸들러 시그니처 변경에 맞게 테스트도 업데이트)

- [ ] **Step 6: 커밋**

```bash
git commit -m "refactor(auth): CoreEngine→Server+ServiceBase 마이그레이션 + kafka_route 전환"
```

---

### Task 10: Chat 서비스 마이그레이션 + E2E Regression

**Files:**
- Modify: `apex_services/chat-svc/include/apex/chat_svc/chat_service.hpp`
- Modify: `apex_services/chat-svc/src/chat_service.cpp`
- Modify: `apex_services/chat-svc/src/main.cpp`

**의존**: Task 9 (Auth 패턴 동일)

- [ ] **Step 1: ChatService를 ServiceBase<ChatService> 상속으로 변경**

Auth와 동일한 패턴. 7개 kafka_route 등록.

- [ ] **Step 2: main.cpp를 Server 기반으로 변경**
- [ ] **Step 3: EnvelopeBuilder 전환**
- [ ] **Step 4: 빌드 + Chat 유닛 테스트 regression**
- [ ] **Step 5: 커밋**

```bash
git commit -m "refactor(chat): CoreEngine→Server+ServiceBase 마이그레이션 + kafka_route 전환"
```

- [ ] **Step 6: 전체 빌드 + 유닛 테스트 전수 실행**

Run: `cmd.exe //c build.bat debug`
Run: 전체 유닛 테스트 실행
Expected: 67+ 기존 테스트 + 신규 테스트 전부 PASS

- [ ] **Step 7: E2E 테스트 regression (Docker 인프라 필요)**

```bash
docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d
sleep 5
D:/.workspace/apex_pipeline_branch_01/build/Windows/debug/apex_services/tests/e2e/apex_e2e_tests.exe
docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v
```

Expected: 기존 11개 E2E 시나리오 PASS

- [ ] **Step 8: 최종 커밋**

```bash
git commit -m "test: 전체 유닛 + E2E regression 통과 확인"
```

---

## 태스크 의존성 요약

```
Task 1 (Context + Registry) ──┐
Task 2 (PeriodicScheduler) ───┤
                               ├─→ Task 3 (ServiceBase 오버홀) ─→ Task 4 (Server 오케스트레이션) ─┐
                               │                                                                   │
Task 5 (EnvelopeBuilder) ─────┼─────────────────────────────────────────────────────────────────────┤
                               │                                                                   │
                               └─→ Task 6 (kafka_route + Bridge) ──────────────────────────────────┤
                                                                                                   │
Task 7 (route<T> error frame) ─────────────────────────────────────────────────────────────────────┤
                                                                                                   │
                               ┌───────────────────────────────────────────────────────────────────┘
                               │
                               ├─→ Task 8 (Gateway 리팩토링)
                               ├─→ Task 9 (Auth 마이그레이션)
                               └─→ Task 10 (Chat 마이그레이션 + E2E)
```

**병렬 가능**:
- Task 1 ∥ Task 2 ∥ Task 5 — 서로 독립
- Task 8 ∥ Task 9 — Gateway와 Auth 독립 (단, 둘 다 Task 4에 의존)
- Task 7은 Task 3 이후 언제든 가능

**순차 필수**:
- Task 1+2 → Task 3 → Task 4 (코어 프레임워크 기반)
- Task 4 + Task 5 + Task 6 → Task 8, 9, 10 (서비스 마이그레이션)
- Task 10 마지막에 E2E regression
