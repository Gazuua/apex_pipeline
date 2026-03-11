#include <apex/core/core_engine.hpp>
#include <apex/core/frame_codec.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/service_base.hpp>
#include <apex/core/wire_header.hpp>
#include "../test_helpers.hpp"
#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace apex::core;
using apex::test::run_coro;
using boost::asio::awaitable;

// --- Test service that records dispatched messages ---

class RecordingService : public ServiceBase<RecordingService> {
public:
    RecordingService() : ServiceBase("recording") {}

    void on_start() override {
        handle(0x0001, &RecordingService::on_echo);
        handle(0x0002, &RecordingService::on_ping);
    }

    awaitable<Result<void>> on_echo(SessionPtr, uint16_t msg_id, std::span<const uint8_t> payload) {
        last_msg_id = msg_id;
        last_payload.assign(payload.begin(), payload.end());
        ++call_count;
        co_return ok();
    }

    awaitable<Result<void>> on_ping(SessionPtr, uint16_t, std::span<const uint8_t>) {
        ++ping_count;
        co_return ok();
    }

    uint16_t last_msg_id = 0;
    std::vector<uint8_t> last_payload;
    int call_count = 0;
    int ping_count = 0;
};

// --- Pipeline: RingBuffer -> FrameCodec -> ServiceBase dispatch ---

TEST(PipelineIntegration, EncodeDecodeDispatch) {
    // 1. Create and start service
    auto service = std::make_unique<RecordingService>();
    service->start();

    // 2. Encode a frame into RingBuffer
    RingBuffer buf(4096);
    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    WireHeader header{.msg_id = 0x0001, .body_size = static_cast<uint32_t>(payload.size())};
    ASSERT_TRUE(FrameCodec::encode(buf, header, payload));

    // 3. Decode the frame
    auto frame = FrameCodec::try_decode(buf);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->header.msg_id, 0x0001);
    EXPECT_EQ(frame->payload.size(), 6);

    // 4. Dispatch to service
    boost::asio::io_context io_ctx;
    auto result = run_coro(io_ctx, service->dispatcher().dispatch(nullptr,
        frame->header.msg_id, frame->payload));
    ASSERT_TRUE(result.has_value());

    // 5. Verify handler received correct data
    EXPECT_EQ(service->last_msg_id, 0x0001);
    EXPECT_EQ(service->last_payload, payload);
    EXPECT_EQ(service->call_count, 1);

    // 6. Consume and verify buffer is empty
    FrameCodec::consume_frame(buf, *frame);
    EXPECT_EQ(buf.readable_size(), 0);
}

TEST(PipelineIntegration, MultiFramePipeline) {
    auto service = std::make_unique<RecordingService>();
    service->start();

    RingBuffer buf(4096);

    // Encode 3 different frames
    std::vector<uint8_t> p1 = {0x01};
    std::vector<uint8_t> p2 = {0x02, 0x03};
    std::vector<uint8_t> p3 = {0x04, 0x05, 0x06};

    ASSERT_TRUE(FrameCodec::encode(buf, {.msg_id = 0x0001, .body_size = 1}, p1));
    ASSERT_TRUE(FrameCodec::encode(buf, {.msg_id = 0x0002, .body_size = 2}, p2));
    ASSERT_TRUE(FrameCodec::encode(buf, {.msg_id = 0x0001, .body_size = 3}, p3));

    // TQ2: Decode and dispatch all frames — verify dispatch return values
    boost::asio::io_context io_ctx;
    int frames_processed = 0;
    while (auto frame = FrameCodec::try_decode(buf)) {
        auto result = run_coro(io_ctx, service->dispatcher().dispatch(nullptr,frame->header.msg_id, frame->payload));
        EXPECT_TRUE(result.has_value());
        FrameCodec::consume_frame(buf, *frame);
        ++frames_processed;
    }

    EXPECT_EQ(frames_processed, 3);
    EXPECT_EQ(service->call_count, 2);  // msg_id 0x0001 called twice
    EXPECT_EQ(service->ping_count, 1);  // msg_id 0x0002 called once
    EXPECT_EQ(buf.readable_size(), 0);
}

TEST(PipelineIntegration, UnknownMessageIdHandledGracefully) {
    auto service = std::make_unique<RecordingService>();
    service->start();

    RingBuffer buf(4096);
    std::vector<uint8_t> payload = {0xFF};
    ASSERT_TRUE(FrameCodec::encode(buf, {.msg_id = 0x9999, .body_size = 1}, payload));

    auto frame = FrameCodec::try_decode(buf);
    ASSERT_TRUE(frame.has_value());

    boost::asio::io_context io_ctx;
    auto result = run_coro(io_ctx, service->dispatcher().dispatch(nullptr,
        frame->header.msg_id, frame->payload));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::HandlerNotFound);
}

// --- CoreEngine + ServiceBase integration ---

TEST(PipelineIntegration, CoreEngineInterCoreDelivery) {
    CoreEngine engine({.num_cores = 2, .mpsc_queue_capacity = 1024});

    std::atomic<int> core0_received{0};
    std::atomic<int> core1_received{0};

    engine.set_message_handler([&](uint32_t core_id, const CoreMessage& msg) {
        if (msg.op == CrossCoreOp::Custom) {
            if (core_id == 0) ++core0_received;
            if (core_id == 1) ++core1_received;
        }
    });

    std::thread t([&] { engine.run(); });
    ASSERT_TRUE(apex::test::wait_for([&] { return engine.running(); }));

    // Core 0 -> Core 1
    (void)engine.post_to(1, {.op = CrossCoreOp::Custom, .source_core = 0, .data = 1});
    (void)engine.post_to(1, {.op = CrossCoreOp::Custom, .source_core = 0, .data = 2});
    // Core 1 -> Core 0
    (void)engine.post_to(0, {.op = CrossCoreOp::Custom, .source_core = 1, .data = 3});

    ASSERT_TRUE(apex::test::wait_for([&] {
        return core0_received.load() >= 1 && core1_received.load() >= 2;
    }));

    EXPECT_EQ(core0_received.load(), 1);
    EXPECT_EQ(core1_received.load(), 2);

    engine.stop();
    t.join();
}

TEST(PipelineIntegration, WireHeaderFlagsPreserved) {
    WireHeader h{
        .msg_id = 0x0042,
        .body_size = 0,
        .flags = wire_flags::COMPRESSED | wire_flags::REQUIRE_AUTH_CHECK
    };

    auto bytes = h.serialize();
    auto parsed = WireHeader::parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->flags & wire_flags::COMPRESSED);
    EXPECT_TRUE(parsed->flags & wire_flags::REQUIRE_AUTH_CHECK);
    EXPECT_FALSE(parsed->flags & wire_flags::HEARTBEAT);
}
