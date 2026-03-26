// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/configure_context.hpp>
#include <apex/core/periodic_task_scheduler.hpp>
#include <apex/core/server.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/service_registry.hpp>
#include <apex/core/wire_context.hpp>

#include <gtest/gtest.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace apex::core;

// ── 테스트용 서비스 정의 ──────────────────────────────────────────────────────

/// 최소 서비스: on_start만 오버라이드.
class MinimalService : public ServiceBase<MinimalService>
{
  public:
    MinimalService()
        : ServiceBase("minimal")
    {}

    void on_start() override
    {
        start_called = true;
    }

    bool start_called = false;
};

/// 라이프사이클 추적 서비스: 모든 훅의 호출 순서를 기록.
class TrackerService : public ServiceBase<TrackerService>
{
  public:
    TrackerService()
        : ServiceBase("tracker")
    {}

    void on_configure(ConfigureContext&) override
    {
        order.push_back("on_configure");
    }

    void on_wire(WireContext&) override
    {
        order.push_back("on_wire");
    }

    void on_start() override
    {
        order.push_back("on_start");
    }

    void on_stop() override
    {
        order.push_back("on_stop");
    }

    void on_session_closed(SessionId sid) override
    {
        order.push_back("on_session_closed");
        closed_sessions.push_back(sid);
    }

    std::vector<std::string> order;
    std::vector<SessionId> closed_sessions;
};

/// on_configure에서 per-core 접근자가 유효한지 검증하는 서비스.
class ConfigAwareService : public ServiceBase<ConfigAwareService>
{
  public:
    ConfigAwareService()
        : ServiceBase("config_aware")
    {}

    void on_configure(ConfigureContext& /*ctx*/) override
    {
        captured_core_id = core_id();
        bump_available = (&bump() != nullptr);
        arena_available = (&arena() != nullptr);
    }

    uint32_t captured_core_id = UINT32_MAX;
    bool bump_available = false;
    bool arena_available = false;
};

/// [D7] spawn() + outstanding_coroutines() 검증용 서비스.
/// spawn()은 protected이므로 public wrapper 메서드를 통해 테스트한다.
class SpawnService : public ServiceBase<SpawnService>
{
  public:
    SpawnService()
        : ServiceBase("spawn_test")
    {}

    void on_configure(ConfigureContext&) override
    {
        configured = true;
    }

    /// spawn()으로 코루틴 실행. flag를 세팅하여 실행 확인.
    void do_spawn_simple()
    {
        spawn([this]() -> boost::asio::awaitable<void> {
            spawn_executed.store(true, std::memory_order_release);
            co_return;
        });
    }

    /// 테스트용: N개의 카운터 증가 코루틴 spawn.
    void do_spawn_counting(int count, std::atomic<int>& completed)
    {
        for (int i = 0; i < count; ++i)
        {
            spawn([&completed]() -> boost::asio::awaitable<void> {
                completed.fetch_add(1, std::memory_order_relaxed);
                co_return;
            });
        }
    }

    /// 테스트용: 예외를 던지는 코루틴 spawn.
    void do_spawn_throwing()
    {
        spawn([]() -> boost::asio::awaitable<void> {
            throw std::runtime_error("test exception");
            co_return;
        });
    }

    bool configured = false;
    std::atomic<bool> spawn_executed{false};
};

// ── Fixture: 실제 객체로 ConfigureContext/WireContext 생성 ────────────────────
// 기존 aligned_storage + reinterpret_cast UB를 실제 인스턴스로 교체 (BACKLOG-118).

class ServiceLifecycleTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        server_ = std::make_unique<Server>(ServerConfig{
            .num_cores = 1, .heartbeat_timeout_ticks = 0, .handle_signals = false, .metrics = {}, .admin = {}});
        registry_view_ = std::make_unique<ServiceRegistryView>(std::vector<ServiceRegistry*>{&registry_});
        scheduler_ = std::make_unique<PeriodicTaskScheduler>(scheduler_io_);
    }

    ConfigureContext make_cfg_ctx(uint32_t core_id, PerCoreState& pcs)
    {
        return ConfigureContext{*server_, core_id, pcs};
    }

    WireContext make_wire_ctx(uint32_t core_id = 0)
    {
        return WireContext{.server = *server_,
                           .core_id = core_id,
                           .local_registry = registry_,
                           .global_registry = *registry_view_,
                           .scheduler = *scheduler_};
    }

    boost::asio::io_context scheduler_io_;
    std::unique_ptr<Server> server_;
    ServiceRegistry registry_;
    std::unique_ptr<ServiceRegistryView> registry_view_;
    std::unique_ptr<PeriodicTaskScheduler> scheduler_;
};

// ── 테스트 ───────────────────────────────────────────────────────────────────

TEST(ServiceLifecycle, MinimalServiceStartWorks)
{
    // 최소 서비스가 새 CRTP start() 경로에서도 정상 동작하는지 확인
    auto svc = std::make_unique<MinimalService>();
    EXPECT_FALSE(svc->start_called);
    svc->start();
    EXPECT_TRUE(svc->start_called);
    EXPECT_TRUE(svc->started());
}

TEST_F(ServiceLifecycleTest, DefaultHooksAreNoOp)
{
    // 기본 구현(no-op)이 크래시 없이 호출되는지 확인
    auto svc = std::make_unique<MinimalService>();

    // PerCoreState 생성 (heartbeat 0 = disabled)
    PerCoreState pcs(/*id=*/0, /*heartbeat_timeout_ticks=*/0,
                     /*timer_wheel_slots=*/64, /*recv_buf_capacity=*/4096,
                     /*max_queue_depth=*/256, /*bump_capacity=*/4096, /*arena_block=*/1024, /*arena_max=*/4096);
    boost::asio::io_context test_io;
    auto cfg_ctx = make_cfg_ctx(0, pcs);
    svc->bind_io_context(test_io);
    // on_configure 기본 구현은 no-op — 크래시 없어야 함
    svc->internal_configure(cfg_ctx);

    auto wire_ctx = make_wire_ctx();
    // on_wire 기본 구현은 no-op — 크래시 없어야 함
    svc->internal_wire(wire_ctx);

    // on_session_closed 기본 구현은 no-op
    svc->on_session_closed(make_session_id(12345));
}

TEST_F(ServiceLifecycleTest, LifecycleOrderTracking)
{
    // 라이프사이클 훅이 올바른 순서로 호출되는지 확인
    auto svc = std::make_unique<TrackerService>();

    PerCoreState pcs(0, 0, 64, 4096, 256, 4096, 1024, 4096);
    boost::asio::io_context test_io;
    auto cfg_ctx = make_cfg_ctx(0, pcs);
    svc->bind_io_context(test_io);
    auto wire_ctx = make_wire_ctx();

    svc->internal_configure(cfg_ctx);
    svc->internal_wire(wire_ctx);
    svc->start();
    svc->on_session_closed(make_session_id(100));
    svc->on_session_closed(make_session_id(200));
    svc->stop();

    ASSERT_EQ(svc->order.size(), 6u);
    EXPECT_EQ(svc->order[0], "on_configure");
    EXPECT_EQ(svc->order[1], "on_wire");
    EXPECT_EQ(svc->order[2], "on_start");
    EXPECT_EQ(svc->order[3], "on_session_closed");
    EXPECT_EQ(svc->order[4], "on_session_closed");
    EXPECT_EQ(svc->order[5], "on_stop");

    ASSERT_EQ(svc->closed_sessions.size(), 2u);
    EXPECT_EQ(svc->closed_sessions[0], make_session_id(100));
    EXPECT_EQ(svc->closed_sessions[1], make_session_id(200));
}

TEST_F(ServiceLifecycleTest, InternalConfigureBindsPerCoreState)
{
    // internal_configure 후 per-core 접근자가 올바르게 동작하는지 확인
    auto svc = std::make_unique<ConfigAwareService>();

    PerCoreState pcs(/*id=*/7, 0, 64, 4096, 256, 4096, 1024, 4096);
    boost::asio::io_context test_io;
    auto cfg_ctx = make_cfg_ctx(7, pcs);
    svc->bind_io_context(test_io);

    svc->internal_configure(cfg_ctx);

    EXPECT_EQ(svc->captured_core_id, 7u);
    EXPECT_TRUE(svc->bump_available);
    EXPECT_TRUE(svc->arena_available);
}

TEST(ServiceLifecycle, OnSessionClosedReceivesCorrectSessionId)
{
    // on_session_closed가 올바른 SessionId를 전달받는지 확인
    auto svc = std::make_unique<TrackerService>();

    svc->on_session_closed(make_session_id(42));
    svc->on_session_closed(make_session_id(UINT64_MAX));

    ASSERT_EQ(svc->closed_sessions.size(), 2u);
    EXPECT_EQ(svc->closed_sessions[0], make_session_id(42));
    EXPECT_EQ(svc->closed_sessions[1], make_session_id(UINT64_MAX));
}

TEST(ServiceLifecycle, BackwardCompatStartStop)
{
    // 기존 서비스 패턴(on_start/on_stop override)이 그대로 동작하는지 확인
    auto svc = std::make_unique<MinimalService>();
    EXPECT_FALSE(svc->started());
    svc->start();
    EXPECT_TRUE(svc->started());
    EXPECT_TRUE(svc->start_called);
    svc->stop();
    EXPECT_FALSE(svc->started());
}

// ── D7: spawn() + outstanding_coroutines() 테스트 ───────────────────────────

TEST(ServiceLifecycle, OutstandingCoroutinesDefaultZero)
{
    // 서비스 생성 직후 outstanding == 0
    auto svc = std::make_unique<MinimalService>();
    EXPECT_EQ(svc->outstanding_coroutines(), 0u);
}

TEST_F(ServiceLifecycleTest, SpawnExecutesCoroutine)
{
    // spawn()으로 실행한 코루틴이 실제로 실행되는지 확인
    auto svc = std::make_unique<SpawnService>();

    PerCoreState pcs(0, 0, 64, 4096, 256, 4096, 1024, 4096);
    boost::asio::io_context test_io;
    auto cfg_ctx = make_cfg_ctx(0, pcs);
    svc->bind_io_context(test_io);
    svc->internal_configure(cfg_ctx);
    EXPECT_TRUE(svc->configured);

    // spawn 실행 전 outstanding == 0
    EXPECT_EQ(svc->outstanding_coroutines(), 0u);

    svc->do_spawn_simple();

    // spawn 직후 outstanding >= 1 (아직 io_context가 돌지 않았으므로 코루틴 미완료)
    EXPECT_GE(svc->outstanding_coroutines(), 1u);

    // io_context 실행하여 코루틴 완료
    test_io.run();

    EXPECT_TRUE(svc->spawn_executed.load());
    EXPECT_EQ(svc->outstanding_coroutines(), 0u);
}

TEST_F(ServiceLifecycleTest, SpawnOutstandingCounterTracksMultiple)
{
    // 여러 spawn 코루틴이 대기 중일 때 outstanding 카운터가 정확히 추적하는지 확인
    auto svc = std::make_unique<SpawnService>();

    PerCoreState pcs(0, 0, 64, 4096, 256, 4096, 1024, 4096);
    boost::asio::io_context test_io;
    auto cfg_ctx = make_cfg_ctx(0, pcs);
    svc->bind_io_context(test_io);
    svc->internal_configure(cfg_ctx);

    // 간단한 코루틴 3개 spawn (public wrapper 사용)
    std::atomic<int> completed{0};
    svc->do_spawn_counting(3, completed);

    // spawn 직후 outstanding == 3 (io_context 미실행)
    EXPECT_EQ(svc->outstanding_coroutines(), 3u);

    test_io.run();

    EXPECT_EQ(completed.load(), 3);
    EXPECT_EQ(svc->outstanding_coroutines(), 0u);
}

TEST_F(ServiceLifecycleTest, SpawnExceptionDoesNotLeak)
{
    // spawn 내부에서 예외 발생해도 outstanding 카운터가 감소하는지 확인.
    // 예외가 밖으로 전파되지 않고 spdlog::error로 로깅만 됨.
    auto svc = std::make_unique<SpawnService>();

    PerCoreState pcs(0, 0, 64, 4096, 256, 4096, 1024, 4096);
    boost::asio::io_context test_io;
    auto cfg_ctx = make_cfg_ctx(0, pcs);
    svc->bind_io_context(test_io);
    svc->internal_configure(cfg_ctx);

    svc->do_spawn_throwing();

    EXPECT_EQ(svc->outstanding_coroutines(), 1u);

    // io_context 실행 — 예외가 catch되어 카운터 감소
    test_io.run();

    EXPECT_EQ(svc->outstanding_coroutines(), 0u);
}

// ── Kafka 핸들러 기본값 테스트 ──────────────────────────────────────────────

TEST(ServiceLifecycle, KafkaHandlersDefaultEmpty)
{
    // ServiceBase의 기본 구현: Kafka 핸들러 미등록
    auto svc = std::make_unique<MinimalService>();
    EXPECT_FALSE(svc->has_kafka_handlers());
    EXPECT_TRUE(svc->kafka_handler_map().empty());
}

// ── ServiceBaseInterface 기본 구현 테스트 ───────────────────────────────────

TEST(ServiceLifecycle, InterfaceOutstandingDefaultZero)
{
    // ServiceBaseInterface의 기본 구현은 0 반환
    // TrackerService(ServiceBase<Derived>)를 통해 간접 검증
    auto svc = std::make_unique<TrackerService>();
    EXPECT_EQ(svc->outstanding_coroutines(), 0u);
}
