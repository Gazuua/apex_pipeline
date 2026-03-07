#include <apex/core/service_base.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/core/frame_codec.hpp>
#include <generated/echo_generated.h>
#include "../test_helpers.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/awaitable.hpp>

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

using namespace apex::core;
using apex::test::run_coro;
using boost::asio::awaitable;

static std::vector<uint8_t> build_echo_payload(const std::vector<uint8_t>& data) {
    flatbuffers::FlatBufferBuilder builder(256);
    auto data_vec = builder.CreateVector(data);
    auto req = apex::messages::CreateEchoRequest(builder, data_vec);
    builder.Finish(req);
    return {builder.GetBufferPointer(),
            builder.GetBufferPointer() + builder.GetSize()};
}

class TypedEchoService : public ServiceBase<TypedEchoService> {
public:
    TypedEchoService() : ServiceBase("typed_echo") {}

    void on_start() override {
        route<apex::messages::EchoRequest>(
            0x0010, &TypedEchoService::on_echo);
    }

    awaitable<Result<void>> on_echo(SessionPtr, uint16_t msg_id,
                            const apex::messages::EchoRequest* req) {
        ++call_count;
        if (req && req->data()) {
            last_data.assign(req->data()->begin(), req->data()->end());
        }
        co_return ok();
    }

    int call_count{0};
    std::vector<uint8_t> last_data;
};

TEST(FlatBuffersDispatch, RouteTypedMessage) {
    boost::asio::io_context io_ctx;
    auto svc = std::make_unique<TypedEchoService>();
    svc->start();

    auto payload = build_echo_payload({0xDE, 0xAD});
    auto result = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0010, payload));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().has_value());

    EXPECT_EQ(svc->call_count, 1);
    EXPECT_EQ(svc->last_data, (std::vector<uint8_t>{0xDE, 0xAD}));
}

TEST(FlatBuffersDispatch, RouteInvalidFlatBuffer) {
    boost::asio::io_context io_ctx;
    auto svc = std::make_unique<TypedEchoService>();
    svc->start();

    std::vector<uint8_t> garbage = {0xFF, 0xFF, 0xFF};
    auto result = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0010, garbage));
    // route()가 검증 실패 시 ErrorCode::FlatBuffersVerifyFailed 반환
    EXPECT_TRUE(result.has_value());
    EXPECT_FALSE(result.value().has_value());
    EXPECT_EQ(result.value().error(), ErrorCode::FlatBuffersVerifyFailed);

    EXPECT_EQ(svc->call_count, 0);
}

TEST(FlatBuffersDispatch, RouteAndRawHandlerCoexist) {
    boost::asio::io_context io_ctx;
    auto svc = std::make_unique<TypedEchoService>();
    svc->start();

    int raw_count = 0;
    svc->dispatcher().register_handler(0x0020,
        [&](SessionPtr, uint16_t, std::span<const uint8_t>) -> awaitable<Result<void>> {
            ++raw_count;
            co_return ok();
        });

    auto payload = build_echo_payload({0x42});
    auto r1 = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0010, payload));
    EXPECT_TRUE(r1.has_value());
    EXPECT_TRUE(r1.value().has_value());
    auto r2 = run_coro(io_ctx, svc->dispatcher().dispatch(nullptr, 0x0020, {}));
    EXPECT_TRUE(r2.has_value());
    EXPECT_TRUE(r2.value().has_value());

    EXPECT_EQ(svc->call_count, 1);
    EXPECT_EQ(raw_count, 1);
}
