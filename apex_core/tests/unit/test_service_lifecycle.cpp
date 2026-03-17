#include <apex/core/service_base.hpp>
#include <apex/core/configure_context.hpp>
#include <apex/core/wire_context.hpp>
#include <apex/core/server.hpp>  // PerCoreState 완전 정의

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace apex::core;

// ── 테스트용 서비스 정의 ──────────────────────────────────────────────────────

/// 최소 서비스: on_start만 오버라이드.
class MinimalService : public ServiceBase<MinimalService> {
public:
    MinimalService() : ServiceBase("minimal") {}

    void on_start() override { start_called = true; }

    bool start_called = false;
};

/// 라이프사이클 추적 서비스: 모든 훅의 호출 순서를 기록.
class TrackerService : public ServiceBase<TrackerService> {
public:
    TrackerService() : ServiceBase("tracker") {}

    void on_configure(ConfigureContext&) override {
        order.push_back("on_configure");
    }

    void on_wire(WireContext&) override {
        order.push_back("on_wire");
    }

    void on_start() override {
        order.push_back("on_start");
    }

    void on_stop() override {
        order.push_back("on_stop");
    }

    void on_session_closed(SessionId sid) override {
        order.push_back("on_session_closed");
        closed_sessions.push_back(sid);
    }

    std::vector<std::string> order;
    std::vector<SessionId> closed_sessions;
};

/// on_configure에서 per-core 접근자가 유효한지 검증하는 서비스.
class ConfigAwareService : public ServiceBase<ConfigAwareService> {
public:
    ConfigAwareService() : ServiceBase("config_aware") {}

    void on_configure(ConfigureContext& ctx) override {
        captured_core_id = core_id();
        bump_available = (&bump() != nullptr);
        arena_available = (&arena() != nullptr);
    }

    uint32_t captured_core_id = UINT32_MAX;
    bool bump_available = false;
    bool arena_available = false;
};

// ── 테스트 ───────────────────────────────────────────────────────────────────

TEST(ServiceLifecycle, MinimalServiceStartWorks) {
    // 최소 서비스가 새 CRTP start() 경로에서도 정상 동작하는지 확인
    auto svc = std::make_unique<MinimalService>();
    EXPECT_FALSE(svc->start_called);
    svc->start();
    EXPECT_TRUE(svc->start_called);
    EXPECT_TRUE(svc->started());
}

TEST(ServiceLifecycle, DefaultHooksAreNoOp) {
    // 기본 구현(no-op)이 크래시 없이 호출되는지 확인
    auto svc = std::make_unique<MinimalService>();

    // PerCoreState 생성 (heartbeat 0 = disabled)
    PerCoreState pcs(/*id=*/0, /*heartbeat_timeout_ticks=*/0,
                     /*timer_wheel_slots=*/64, /*recv_buf_capacity=*/4096,
                     /*bump_capacity=*/4096, /*arena_block=*/1024, /*arena_max=*/4096);
    ConfigureContext cfg_ctx{
        .server = *reinterpret_cast<Server*>(0x1),  // 테스트에서 server는 접근하지 않음
        .core_id = 0,
        .per_core_state = pcs
    };
    // on_configure 기본 구현은 no-op — 크래시 없어야 함
    svc->internal_configure(cfg_ctx);

    WireContext wire_ctx{
        .server = *reinterpret_cast<Server*>(0x1),
        .core_id = 0,
        .local_registry = *reinterpret_cast<ServiceRegistry*>(0x1),
        .global_registry = *reinterpret_cast<ServiceRegistryView*>(0x1),
        .scheduler = *reinterpret_cast<PeriodicTaskScheduler*>(0x1)
    };
    // on_wire 기본 구현은 no-op — 크래시 없어야 함
    svc->internal_wire(wire_ctx);

    // on_session_closed 기본 구현은 no-op
    svc->on_session_closed(12345);
}

TEST(ServiceLifecycle, LifecycleOrderTracking) {
    // 라이프사이클 훅이 올바른 순서로 호출되는지 확인
    auto svc = std::make_unique<TrackerService>();

    PerCoreState pcs(0, 0, 64, 4096, 4096, 1024, 4096);
    ConfigureContext cfg_ctx{
        .server = *reinterpret_cast<Server*>(0x1),
        .core_id = 0,
        .per_core_state = pcs
    };
    WireContext wire_ctx{
        .server = *reinterpret_cast<Server*>(0x1),
        .core_id = 0,
        .local_registry = *reinterpret_cast<ServiceRegistry*>(0x1),
        .global_registry = *reinterpret_cast<ServiceRegistryView*>(0x1),
        .scheduler = *reinterpret_cast<PeriodicTaskScheduler*>(0x1)
    };

    svc->internal_configure(cfg_ctx);
    svc->internal_wire(wire_ctx);
    svc->start();
    svc->on_session_closed(100);
    svc->on_session_closed(200);
    svc->stop();

    ASSERT_EQ(svc->order.size(), 6u);
    EXPECT_EQ(svc->order[0], "on_configure");
    EXPECT_EQ(svc->order[1], "on_wire");
    EXPECT_EQ(svc->order[2], "on_start");
    EXPECT_EQ(svc->order[3], "on_session_closed");
    EXPECT_EQ(svc->order[4], "on_session_closed");
    EXPECT_EQ(svc->order[5], "on_stop");

    ASSERT_EQ(svc->closed_sessions.size(), 2u);
    EXPECT_EQ(svc->closed_sessions[0], 100u);
    EXPECT_EQ(svc->closed_sessions[1], 200u);
}

TEST(ServiceLifecycle, InternalConfigureBindsPerCoreState) {
    // internal_configure 후 per-core 접근자가 올바르게 동작하는지 확인
    auto svc = std::make_unique<ConfigAwareService>();

    PerCoreState pcs(/*id=*/7, 0, 64, 4096, 4096, 1024, 4096);
    ConfigureContext cfg_ctx{
        .server = *reinterpret_cast<Server*>(0x1),
        .core_id = 7,
        .per_core_state = pcs
    };

    svc->internal_configure(cfg_ctx);

    EXPECT_EQ(svc->captured_core_id, 7u);
    EXPECT_TRUE(svc->bump_available);
    EXPECT_TRUE(svc->arena_available);
}

TEST(ServiceLifecycle, OnSessionClosedReceivesCorrectSessionId) {
    // on_session_closed가 올바른 SessionId를 전달받는지 확인
    auto svc = std::make_unique<TrackerService>();

    svc->on_session_closed(42);
    svc->on_session_closed(UINT64_MAX);

    ASSERT_EQ(svc->closed_sessions.size(), 2u);
    EXPECT_EQ(svc->closed_sessions[0], 42u);
    EXPECT_EQ(svc->closed_sessions[1], UINT64_MAX);
}

TEST(ServiceLifecycle, BackwardCompatStartStop) {
    // 기존 서비스 패턴(on_start/on_stop override)이 그대로 동작하는지 확인
    auto svc = std::make_unique<MinimalService>();
    EXPECT_FALSE(svc->started());
    svc->start();
    EXPECT_TRUE(svc->started());
    EXPECT_TRUE(svc->start_called);
    svc->stop();
    EXPECT_FALSE(svc->started());
}
