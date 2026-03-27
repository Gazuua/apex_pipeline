// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/adapter_state.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string_view>

using namespace apex::shared::adapters;
using apex::core::AdapterState;

// --- FailingAdapter: init 실패를 제어 가능한 mock ---

class FailingAdapter : public AdapterBase<FailingAdapter>
{
    friend class AdapterBase<FailingAdapter>;

    bool should_fail_ = false;

    std::string_view do_name() const noexcept
    {
        return "FailingAdapter";
    }
    void do_init(apex::core::CoreEngine& /*engine*/)
    {
        if (should_fail_)
            throw std::runtime_error("mock init failure");
    }
    void do_drain() {}
    void do_close() {}
    void do_close_per_core(uint32_t /*core_id*/) {}

  public:
    void set_fail(bool f)
    {
        should_fail_ = f;
    }
};

// --- 공통 CoreEngine 생성 헬퍼 ---

namespace
{
apex::core::CoreEngine make_engine()
{
    apex::core::CoreEngineConfig config{.num_cores = 1,
                                        .spsc_queue_capacity = 64,
                                        .tick_interval = std::chrono::milliseconds{100},
                                        .drain_batch_limit = 1024,
                                        .core_assignments = {},
                                        .numa_aware = true};
    return apex::core::CoreEngine(config);
}
} // namespace

// 1) init 실패 시 CLOSED로 롤백
TEST(AdapterBaseFailure, InitFailureRollsBackToClosed)
{
    FailingAdapter adapter;
    adapter.set_fail(true);
    auto engine = make_engine();

    EXPECT_THROW(adapter.init(engine), std::runtime_error);
    EXPECT_EQ(adapter.state(), AdapterState::CLOSED);
    EXPECT_FALSE(adapter.is_ready());
}

// 2) 이중 init 호출 — 경고 로그 후 RUNNING 유지
TEST(AdapterBaseFailure, DoubleInitWarns)
{
    FailingAdapter adapter;
    auto engine = make_engine();

    adapter.init(engine);
    EXPECT_EQ(adapter.state(), AdapterState::RUNNING);

    // 두 번째 init — 경고 로그 후 무시, 상태 변화 없음
    adapter.init(engine);
    EXPECT_EQ(adapter.state(), AdapterState::RUNNING);
    EXPECT_TRUE(adapter.is_ready());
}

// 3) close() per-core 타임아웃 경고 검증
//    init으로 io_ctxs_가 설정된 후 io_context.run()을 호출하지 않으면
//    post된 do_close_per_core 핸들러가 실행되지 않아 5초 타임아웃 발생.
//    이 테스트는 타임아웃 경로가 크래시 없이 완료되는지 검증한다.
TEST(AdapterBaseFailure, CloseTimeoutLogsWarning)
{
    FailingAdapter adapter;
    auto engine = make_engine();
    adapter.init(engine);
    EXPECT_EQ(adapter.state(), AdapterState::RUNNING);

    // io_context.run() 미호출 → per-core cleanup 미실행 → 5초 타임아웃 경로 진입
    adapter.close();
    EXPECT_EQ(adapter.state(), AdapterState::CLOSED);
    EXPECT_FALSE(adapter.is_ready());
}

// 4) init 실패 후 복구: CLOSED → set_fail(false) → 재init → RUNNING
TEST(AdapterBaseFailure, RecoveryAfterInitFailure)
{
    FailingAdapter adapter;
    adapter.set_fail(true);
    auto engine = make_engine();

    EXPECT_THROW(adapter.init(engine), std::runtime_error);
    EXPECT_EQ(adapter.state(), AdapterState::CLOSED);

    // 복구: 실패 원인 제거 후 재시도
    adapter.set_fail(false);
    adapter.init(engine);
    EXPECT_EQ(adapter.state(), AdapterState::RUNNING);
    EXPECT_TRUE(adapter.is_ready());
}
