#include <apex/core/service_base.hpp>
#include "../test_helpers.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/awaitable.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using namespace apex::core;
using apex::test::run_coro;
using boost::asio::awaitable;

class EchoService : public ServiceBase<EchoService> {
public:
    EchoService() : ServiceBase("echo") {}

    void on_start() override {
        handle(0x0001, &EchoService::on_echo);
        start_called = true;
    }

    void on_stop() override { stop_called = true; }

    awaitable<Result<void>> on_echo(SessionPtr, uint32_t msg_id, std::span<const uint8_t> payload) {
        last_msg_id = msg_id;
        last_payload.assign(payload.begin(), payload.end());
        co_return ok();
    }

    bool start_called = false;
    bool stop_called = false;
    uint32_t last_msg_id = 0;
    std::vector<uint8_t> last_payload;
};

/// 같은 msg_id로 handle()을 2번 호출하는 서비스 (중복 등록 테스트용).
class DuplicateHandlerService : public ServiceBase<DuplicateHandlerService> {
public:
    DuplicateHandlerService() : ServiceBase("dup") {}

    void on_start() override {
        handle(0x0001, &DuplicateHandlerService::on_msg);
        handle(0x0001, &DuplicateHandlerService::on_msg);  // 같은 ID 재등록
        handle(0x0002, &DuplicateHandlerService::on_msg);
    }

    awaitable<Result<void>> on_msg(SessionPtr, uint32_t, std::span<const uint8_t>) { co_return ok(); }
};

class MultiHandlerService : public ServiceBase<MultiHandlerService> {
public:
    MultiHandlerService() : ServiceBase("multi") {}

    void on_start() override {
        handle(0x0001, &MultiHandlerService::on_msg1);
        handle(0x0002, &MultiHandlerService::on_msg2);
        handle(0x0003, &MultiHandlerService::on_msg3);
    }

    awaitable<Result<void>> on_msg1(SessionPtr, uint32_t, std::span<const uint8_t>) { ++count1; co_return ok(); }
    awaitable<Result<void>> on_msg2(SessionPtr, uint32_t, std::span<const uint8_t>) { ++count2; co_return ok(); }
    awaitable<Result<void>> on_msg3(SessionPtr, uint32_t, std::span<const uint8_t>) { ++count3; co_return ok(); }

    int count1 = 0;
    int count2 = 0;
    int count3 = 0;
};

TEST(ServiceBase, NameReturnsCorrectName) {
    auto svc = std::make_unique<EchoService>();
    EXPECT_EQ(svc->name(), "echo");
}

TEST(ServiceBase, StartCallsOnStart) {
    auto svc = std::make_unique<EchoService>();
    EXPECT_FALSE(svc->start_called);
    svc->start();
    EXPECT_TRUE(svc->start_called);
    EXPECT_TRUE(svc->started());
}

TEST(ServiceBase, StopCallsOnStop) {
    auto svc = std::make_unique<EchoService>();
    svc->start();
    EXPECT_FALSE(svc->stop_called);
    svc->stop();
    EXPECT_TRUE(svc->stop_called);
    EXPECT_FALSE(svc->started());
}

TEST(ServiceBase, HandleRegistersAndDispatches) {
    boost::asio::io_context io_ctx;
    auto svc = std::make_unique<EchoService>();
    svc->start();

    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto result = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0001, data));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(svc->last_msg_id, 0x0001);
    EXPECT_EQ(svc->last_payload, data);
}

TEST(ServiceBase, UnregisteredMsgReturnsError) {
    boost::asio::io_context io_ctx;
    auto svc = std::make_unique<EchoService>();
    svc->start();

    auto result = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x9999, {}));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::HandlerNotFound);
}

TEST(ServiceBase, MultipleHandlers) {
    boost::asio::io_context io_ctx;
    auto svc = std::make_unique<MultiHandlerService>();
    svc->start();

    EXPECT_TRUE(run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0001, {})).has_value());
    EXPECT_TRUE(run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0001, {})).has_value());
    EXPECT_TRUE(run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0002, {})).has_value());
    EXPECT_TRUE(run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0003, {})).has_value());
    EXPECT_TRUE(run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0003, {})).has_value());
    EXPECT_TRUE(run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0003, {})).has_value());

    EXPECT_EQ(svc->count1, 2);
    EXPECT_EQ(svc->count2, 1);
    EXPECT_EQ(svc->count3, 3);
}

TEST(ServiceBase, DispatcherHandlerCount) {
    auto svc = std::make_unique<MultiHandlerService>();
    EXPECT_EQ(svc->dispatcher().handler_count(), 0u);

    svc->start();
    EXPECT_EQ(svc->dispatcher().handler_count(), 3u);
}

TEST(ServiceBase, BindExternalDispatcher) {
    boost::asio::io_context io_ctx;
    auto external_dispatcher = std::make_unique<MessageDispatcher>();
    auto svc = std::make_unique<EchoService>();

    svc->bind_dispatcher(*external_dispatcher);
    svc->start();

    EXPECT_TRUE(external_dispatcher->has_handler(0x0001));
    EXPECT_EQ(external_dispatcher->handler_count(), 1u);

    std::vector<uint8_t> data = {0xAA, 0xBB};
    auto result = run_coro(io_ctx, external_dispatcher->dispatch(nullptr, 0x0001, data));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(svc->last_msg_id, 0x0001);
    EXPECT_EQ(svc->last_payload, data);
}

TEST(ServiceBase, DuplicateHandleIdRegistersOnce) {
    auto svc = std::make_unique<DuplicateHandlerService>();
    svc->start();

    // 같은 msg_id(0x0001)로 handle() 2번 + 다른 id(0x0002) 1번 = handler 2개만 등록
    EXPECT_EQ(svc->dispatcher().handler_count(), 2u);
    EXPECT_TRUE(svc->dispatcher().has_handler(0x0001));
    EXPECT_TRUE(svc->dispatcher().has_handler(0x0002));

    svc->stop();

    // stop() 후 unregister가 ID당 1번씩만 호출되어야 함.
    // 중복이었다면 handler_count_가 음수 underflow → 0이 아닌 값이 됨.
    EXPECT_EQ(svc->dispatcher().handler_count(), 0u);
    EXPECT_FALSE(svc->dispatcher().has_handler(0x0001));
    EXPECT_FALSE(svc->dispatcher().has_handler(0x0002));
}
