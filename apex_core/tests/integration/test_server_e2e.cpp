#include <apex/core/server.hpp>
#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/core/frame_codec.hpp>
#include <apex/core/error_code.hpp>

#include <generated/echo_generated.h>
#include <generated/chat_message_generated.h>
#include <generated/error_response_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace apex::core;
using boost::asio::ip::tcp;
using boost::asio::awaitable;
using namespace std::chrono_literals;

// --- Helpers ---

static std::vector<uint8_t> build_echo_frame(const std::vector<uint8_t>& data) {
    flatbuffers::FlatBufferBuilder builder(256);
    auto data_vec = builder.CreateVector(data);
    auto req = apex::messages::CreateEchoRequest(builder, data_vec);
    builder.Finish(req);

    WireHeader header{
        .msg_id = 0x0001,
        .body_size = static_cast<uint32_t>(builder.GetSize())
    };
    auto hdr_bytes = header.serialize();

    std::vector<uint8_t> frame(hdr_bytes.begin(), hdr_bytes.end());
    frame.insert(frame.end(),
                 builder.GetBufferPointer(),
                 builder.GetBufferPointer() + builder.GetSize());
    return frame;
}

static std::vector<uint8_t> read_frame(tcp::socket& client) {
    std::vector<uint8_t> hdr_buf(WireHeader::SIZE);
    boost::asio::read(client, boost::asio::buffer(hdr_buf));
    auto header = WireHeader::parse(hdr_buf);
    if (!header) return {};

    std::vector<uint8_t> frame(WireHeader::SIZE + header->body_size);
    std::memcpy(frame.data(), hdr_buf.data(), WireHeader::SIZE);
    if (header->body_size > 0) {
        boost::asio::read(client,
            boost::asio::buffer(frame.data() + WireHeader::SIZE, header->body_size));
    }
    return frame;
}

static tcp::socket make_client(boost::asio::io_context& ctx, uint16_t port) {
    tcp::socket client(ctx);
    client.connect(tcp::endpoint(
        boost::asio::ip::address_v4::loopback(), port));
    return client;
}

// --- Test Services ---

class TestEchoService : public ServiceBase<TestEchoService> {
public:
    TestEchoService() : ServiceBase("test_echo") {}

    void on_start() override {
        route<apex::messages::EchoRequest>(0x0001, &TestEchoService::on_echo);
    }

    awaitable<Result<void>> on_echo(SessionPtr session, uint16_t msg_id,
                            const apex::messages::EchoRequest* req) {
        if (!req || !req->data()) co_return ok();

        flatbuffers::FlatBufferBuilder builder(256);
        auto data_vec = builder.CreateVector(
            req->data()->data(), req->data()->size());
        auto resp = apex::messages::CreateEchoResponse(builder, data_vec);
        builder.Finish(resp);

        WireHeader header{
            .msg_id = msg_id,
            .body_size = static_cast<uint32_t>(builder.GetSize())
        };
        co_await session->async_send(header, {builder.GetBufferPointer(), builder.GetSize()});
        co_return ok();
    }
};

class ThrowingService : public ServiceBase<ThrowingService> {
public:
    ThrowingService() : ServiceBase("throwing") {}
    void on_start() override {
        handle(0x0010, &ThrowingService::on_msg);
    }
    awaitable<Result<void>> on_msg(SessionPtr, uint16_t, std::span<const uint8_t>) {
        throw std::runtime_error("test exception");
        co_return ok();
    }
};

class ErrorReturningService : public ServiceBase<ErrorReturningService> {
public:
    ErrorReturningService() : ServiceBase("error_returning") {}
    void on_start() override {
        handle(0x0020, &ErrorReturningService::on_msg);
    }
    awaitable<Result<void>> on_msg(SessionPtr, uint16_t, std::span<const uint8_t>) {
        co_return apex::core::error(ErrorCode::Timeout);
    }
};

// --- Test Fixture ---

class ServerE2ETest : public ::testing::Test {
protected:
    std::thread server_thread_;

    void run_server(Server& server) {
        server_thread_ = std::thread([&server] { server.run(); });
        std::this_thread::sleep_for(200ms);
    }

    void stop_and_join(Server& server) {
        server.stop();
        if (server_thread_.joinable()) server_thread_.join();
    }
};

// --- Test Cases ---

TEST_F(ServerE2ETest, ServerAcceptAndEcho) {
    Server server({.port = 0, .heartbeat_timeout_ticks = 0, .handle_signals = false});
    server.add_service<TestEchoService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    auto frame = build_echo_frame({0xDE, 0xAD});
    boost::asio::write(client, boost::asio::buffer(frame));

    auto response = read_frame(client);
    ASSERT_GT(response.size(), WireHeader::SIZE);

    auto header = WireHeader::parse(response);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->msg_id, 0x0001);

    auto resp = flatbuffers::GetRoot<apex::messages::EchoResponse>(
        response.data() + WireHeader::SIZE);
    ASSERT_NE(resp->data(), nullptr);
    EXPECT_EQ(resp->data()->size(), 2u);
    EXPECT_EQ((*resp->data())[0], 0xDE);
    EXPECT_EQ((*resp->data())[1], 0xAD);

    client.close();
    stop_and_join(server);
}

TEST_F(ServerE2ETest, MultipleClients) {
    Server server({.port = 0, .heartbeat_timeout_ticks = 0, .handle_signals = false});
    server.add_service<TestEchoService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client0 = make_client(client_ctx, server.port());
    auto client1 = make_client(client_ctx, server.port());
    auto client2 = make_client(client_ctx, server.port());

    tcp::socket* clients[] = {&client0, &client1, &client2};
    for (int i = 0; i < 3; ++i) {
        auto frame = build_echo_frame({static_cast<uint8_t>(i)});
        boost::asio::write(*clients[i], boost::asio::buffer(frame));

        auto response = read_frame(*clients[i]);
        auto header = WireHeader::parse(response);
        ASSERT_TRUE(header.has_value());

        auto resp = flatbuffers::GetRoot<apex::messages::EchoResponse>(
            response.data() + WireHeader::SIZE);
        ASSERT_NE(resp->data(), nullptr);
        EXPECT_EQ((*resp->data())[0], static_cast<uint8_t>(i));
    }

    for (auto* c : clients) c->close();
    stop_and_join(server);
}

TEST_F(ServerE2ETest, InvalidMessageErrorResponse) {
    Server server({.port = 0, .heartbeat_timeout_ticks = 0, .handle_signals = false});
    server.add_service<TestEchoService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    WireHeader header{.msg_id = 0x9999, .body_size = 0};
    auto hdr_bytes = header.serialize();
    boost::asio::write(client, boost::asio::buffer(
        std::vector<uint8_t>(hdr_bytes.begin(), hdr_bytes.end())));

    auto response = read_frame(client);
    auto resp_header = WireHeader::parse(response);
    ASSERT_TRUE(resp_header.has_value());
    EXPECT_EQ(resp_header->msg_id, 0x9999);
    EXPECT_TRUE(resp_header->flags & wire_flags::ERROR_RESPONSE);

    auto err = flatbuffers::GetRoot<apex::messages::ErrorResponse>(
        response.data() + WireHeader::SIZE);
    EXPECT_EQ(err->code(), static_cast<uint16_t>(ErrorCode::HandlerNotFound));

    client.close();
    stop_and_join(server);
}

TEST_F(ServerE2ETest, GracefulShutdown) {
    Server server({.port = 0, .heartbeat_timeout_ticks = 0, .handle_signals = false});
    server.add_service<TestEchoService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    auto frame = build_echo_frame({0x42});
    boost::asio::write(client, boost::asio::buffer(frame));
    auto response = read_frame(client);
    EXPECT_GT(response.size(), 0u);

    stop_and_join(server);

    boost::system::error_code ec;
    std::array<uint8_t, 1> buf{};
    client.read_some(boost::asio::buffer(buf), ec);
    EXPECT_TRUE(ec);
}

TEST_F(ServerE2ETest, HeartbeatTimeoutDisconnect) {
    Server server({
        .port = 0,
        .heartbeat_timeout_ticks = 3,
        .timer_wheel_slots = 8,
        .handle_signals = false,
    });
    server.add_service<TestEchoService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    std::this_thread::sleep_for(200ms);

    boost::system::error_code ec;
    std::array<uint8_t, 1> buf{};
    client.read_some(boost::asio::buffer(buf), ec);
    EXPECT_TRUE(ec);

    stop_and_join(server);
}

TEST_F(ServerE2ETest, HandlerFailedErrorResponse) {
    Server server({.port = 0, .heartbeat_timeout_ticks = 0, .handle_signals = false});
    server.add_service<ThrowingService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    WireHeader header{.msg_id = 0x0010, .body_size = 0};
    auto hdr_bytes = header.serialize();
    boost::asio::write(client, boost::asio::buffer(
        std::vector<uint8_t>(hdr_bytes.begin(), hdr_bytes.end())));

    auto response = read_frame(client);
    auto resp_header = WireHeader::parse(response);
    ASSERT_TRUE(resp_header.has_value());
    EXPECT_EQ(resp_header->msg_id, 0x0010);
    EXPECT_TRUE(resp_header->flags & wire_flags::ERROR_RESPONSE);

    auto err = flatbuffers::GetRoot<apex::messages::ErrorResponse>(
        response.data() + WireHeader::SIZE);
    EXPECT_EQ(err->code(), static_cast<uint16_t>(ErrorCode::Unknown));

    client.close();
    stop_and_join(server);
}

TEST_F(ServerE2ETest, HandlerErrorCodeResponse) {
    Server server({.port = 0, .heartbeat_timeout_ticks = 0, .handle_signals = false});
    server.add_service<ErrorReturningService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    WireHeader header{.msg_id = 0x0020, .body_size = 0};
    auto hdr_bytes = header.serialize();
    boost::asio::write(client, boost::asio::buffer(
        std::vector<uint8_t>(hdr_bytes.begin(), hdr_bytes.end())));

    auto response = read_frame(client);
    auto resp_header = WireHeader::parse(response);
    ASSERT_TRUE(resp_header.has_value());
    EXPECT_EQ(resp_header->msg_id, 0x0020);
    EXPECT_TRUE(resp_header->flags & wire_flags::ERROR_RESPONSE);

    auto err = flatbuffers::GetRoot<apex::messages::ErrorResponse>(
        response.data() + WireHeader::SIZE);
    EXPECT_EQ(err->code(), static_cast<uint16_t>(ErrorCode::Timeout));

    client.close();
    stop_and_join(server);
}
