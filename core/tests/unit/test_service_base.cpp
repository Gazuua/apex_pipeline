#include <apex/core/service_base.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using namespace apex::core;

class EchoService : public ServiceBase<EchoService> {
public:
    EchoService() : ServiceBase("echo") {}

    void on_start() override {
        handle(0x0001, &EchoService::on_echo);
        start_called = true;
    }

    void on_stop() override { stop_called = true; }

    void on_echo(uint16_t msg_id, std::span<const uint8_t> payload) {
        last_msg_id = msg_id;
        last_payload.assign(payload.begin(), payload.end());
    }

    bool start_called = false;
    bool stop_called = false;
    uint16_t last_msg_id = 0;
    std::vector<uint8_t> last_payload;
};

class MultiHandlerService : public ServiceBase<MultiHandlerService> {
public:
    MultiHandlerService() : ServiceBase("multi") {}

    void on_start() override {
        handle(0x0001, &MultiHandlerService::on_msg1);
        handle(0x0002, &MultiHandlerService::on_msg2);
        handle(0x0003, &MultiHandlerService::on_msg3);
    }

    void on_msg1(uint16_t, std::span<const uint8_t>) { ++count1; }
    void on_msg2(uint16_t, std::span<const uint8_t>) { ++count2; }
    void on_msg3(uint16_t, std::span<const uint8_t>) { ++count3; }

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
    auto svc = std::make_unique<EchoService>();
    svc->start();

    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto result = svc->dispatcher().dispatch(0x0001, data);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(svc->last_msg_id, 0x0001);
    EXPECT_EQ(svc->last_payload, data);
}

TEST(ServiceBase, UnregisteredMsgReturnsError) {
    auto svc = std::make_unique<EchoService>();
    svc->start();

    auto result = svc->dispatcher().dispatch(0x9999, {});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DispatchError::UnknownMessage);
}

// TQ1: Verify dispatch() return values instead of (void) cast
TEST(ServiceBase, MultipleHandlers) {
    auto svc = std::make_unique<MultiHandlerService>();
    svc->start();

    EXPECT_TRUE(svc->dispatcher().dispatch(0x0001, {}).has_value());
    EXPECT_TRUE(svc->dispatcher().dispatch(0x0001, {}).has_value());
    EXPECT_TRUE(svc->dispatcher().dispatch(0x0002, {}).has_value());
    EXPECT_TRUE(svc->dispatcher().dispatch(0x0003, {}).has_value());
    EXPECT_TRUE(svc->dispatcher().dispatch(0x0003, {}).has_value());
    EXPECT_TRUE(svc->dispatcher().dispatch(0x0003, {}).has_value());

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
