// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/error_code.hpp>
#include <apex/core/frame_codec.hpp>
#include <apex/core/server.hpp>
#include <apex/core/session.hpp>
#include <apex/core/wire_header.hpp>
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>

#include "../test_helpers.hpp"

#include <flatbuffers/flatbuffers.h>
#include <generated/chat_message_generated.h>
#include <generated/echo_generated.h>
#include <generated/error_response_generated.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace apex::core;
using apex::shared::protocols::tcp::TcpBinaryProtocol;
using boost::asio::awaitable;
using boost::asio::ip::tcp;
using namespace std::chrono_literals;

// --- Helpers ---

static std::vector<uint8_t> build_echo_frame(const std::vector<uint8_t>& data)
{
    flatbuffers::FlatBufferBuilder builder(256);
    auto data_vec = builder.CreateVector(data);
    auto req = apex::messages::CreateEchoRequest(builder, data_vec);
    builder.Finish(req);

    WireHeader header{.msg_id = 0x0001, .body_size = static_cast<uint32_t>(builder.GetSize()), .reserved = {}};
    auto hdr_bytes = header.serialize();

    std::vector<uint8_t> frame(hdr_bytes.begin(), hdr_bytes.end());
    frame.insert(frame.end(), builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
    return frame;
}

static std::vector<uint8_t> read_frame(tcp::socket& client)
{
    std::vector<uint8_t> hdr_buf(WireHeader::SIZE);
    boost::asio::read(client, boost::asio::buffer(hdr_buf));
    auto header = WireHeader::parse(hdr_buf);
    if (!header)
        return {};

    std::vector<uint8_t> frame(WireHeader::SIZE + header->body_size);
    std::memcpy(frame.data(), hdr_buf.data(), WireHeader::SIZE);
    if (header->body_size > 0)
    {
        boost::asio::read(client, boost::asio::buffer(frame.data() + WireHeader::SIZE, header->body_size));
    }
    return frame;
}

static tcp::socket make_client(boost::asio::io_context& ctx, uint16_t port)
{
    tcp::socket client(ctx);
    client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    return client;
}

// --- Test Services ---

class TestEchoService : public ServiceBase<TestEchoService>
{
  public:
    TestEchoService()
        : ServiceBase("test_echo")
    {}

    void on_start() override
    {
        route<apex::messages::EchoRequest>(0x0001, &TestEchoService::on_echo);
    }

    awaitable<Result<void>> on_echo(SessionPtr session, uint32_t msg_id, const apex::messages::EchoRequest* req)
    {
        if (!req || !req->data())
            co_return ok();

        flatbuffers::FlatBufferBuilder builder(256);
        auto data_vec = builder.CreateVector(req->data()->data(), req->data()->size());
        auto resp = apex::messages::CreateEchoResponse(builder, data_vec);
        builder.Finish(resp);

        WireHeader header{.msg_id = msg_id, .body_size = static_cast<uint32_t>(builder.GetSize()), .reserved = {}};
        (void)co_await session->async_send(header, {builder.GetBufferPointer(), builder.GetSize()});
        co_return ok();
    }
};

class ThrowingService : public ServiceBase<ThrowingService>
{
  public:
    ThrowingService()
        : ServiceBase("throwing")
    {}
    void on_start() override
    {
        handle(0x0010, &ThrowingService::on_msg);
    }
    awaitable<Result<void>> on_msg(SessionPtr /*session*/, uint32_t /*msg_id*/, std::span<const uint8_t> /*payload*/)
    {
        throw std::runtime_error("test exception");
        co_return ok();
    }
};

class ErrorReturningService : public ServiceBase<ErrorReturningService>
{
  public:
    ErrorReturningService()
        : ServiceBase("error_returning")
    {}
    void on_start() override
    {
        handle(0x0020, &ErrorReturningService::on_msg);
    }
    awaitable<Result<void>> on_msg(SessionPtr /*session*/, uint32_t /*msg_id*/, std::span<const uint8_t> /*payload*/)
    {
        co_return apex::core::error(ErrorCode::Timeout);
    }
};

// --- Test Fixture ---

class ServerE2ETest : public ::testing::Test
{
  protected:
    std::thread server_thread_;

    void run_server(Server& server)
    {
        server_thread_ = std::thread([&server] { server.run(); });
        ASSERT_TRUE(apex::test::wait_for([&] { return server.running(); }, std::chrono::milliseconds(5000)))
            << "Server failed to start within 5 seconds";
    }

    void stop_and_join(Server& server)
    {
        server.stop();
        if (server_thread_.joinable())
            server_thread_.join();
    }
};

// --- Test Cases ---

TEST_F(ServerE2ETest, ServerAcceptAndEcho)
{
    Server server({.heartbeat_timeout_ticks = 0,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {},
                   .affinity = {.enabled = false}});
    server.listen<TcpBinaryProtocol>(0);
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

    auto resp = flatbuffers::GetRoot<apex::messages::EchoResponse>(response.data() + WireHeader::SIZE);
    ASSERT_NE(resp->data(), nullptr);
    EXPECT_EQ(resp->data()->size(), 2u);
    EXPECT_EQ((*resp->data())[0], 0xDE);
    EXPECT_EQ((*resp->data())[1], 0xAD);

    client.close();
    stop_and_join(server);
}

TEST_F(ServerE2ETest, MultipleClients)
{
    Server server({.heartbeat_timeout_ticks = 0,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {},
                   .affinity = {.enabled = false}});
    server.listen<TcpBinaryProtocol>(0);
    server.add_service<TestEchoService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client0 = make_client(client_ctx, server.port());
    auto client1 = make_client(client_ctx, server.port());
    auto client2 = make_client(client_ctx, server.port());

    tcp::socket* clients[] = {&client0, &client1, &client2};
    for (int i = 0; i < 3; ++i)
    {
        auto frame = build_echo_frame({static_cast<uint8_t>(i)});
        boost::asio::write(*clients[i], boost::asio::buffer(frame));

        auto response = read_frame(*clients[i]);
        auto header = WireHeader::parse(response);
        ASSERT_TRUE(header.has_value());

        auto resp = flatbuffers::GetRoot<apex::messages::EchoResponse>(response.data() + WireHeader::SIZE);
        ASSERT_NE(resp->data(), nullptr);
        EXPECT_EQ((*resp->data())[0], static_cast<uint8_t>(i));
    }

    for (auto* c : clients)
        c->close();
    stop_and_join(server);
}

TEST_F(ServerE2ETest, InvalidMessageErrorResponse)
{
    Server server({.heartbeat_timeout_ticks = 0,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {},
                   .affinity = {.enabled = false}});
    server.listen<TcpBinaryProtocol>(0);
    server.add_service<TestEchoService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    WireHeader header{.msg_id = 0x9999, .body_size = 0, .reserved = {}};
    auto hdr_bytes = header.serialize();
    boost::asio::write(client, boost::asio::buffer(std::vector<uint8_t>(hdr_bytes.begin(), hdr_bytes.end())));

    auto response = read_frame(client);
    auto resp_header = WireHeader::parse(response);
    ASSERT_TRUE(resp_header.has_value());
    EXPECT_EQ(resp_header->msg_id, 0x9999);
    EXPECT_TRUE(resp_header->flags & wire_flags::ERROR_RESPONSE);

    auto err = flatbuffers::GetRoot<apex::messages::ErrorResponse>(response.data() + WireHeader::SIZE);
    EXPECT_EQ(err->code(), static_cast<uint16_t>(ErrorCode::HandlerNotFound));

    client.close();
    stop_and_join(server);
}

TEST_F(ServerE2ETest, GracefulShutdown)
{
    Server server({.heartbeat_timeout_ticks = 0,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {},
                   .affinity = {.enabled = false}});
    server.listen<TcpBinaryProtocol>(0);
    server.add_service<TestEchoService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    auto frame = build_echo_frame({0x42});
    boost::asio::write(client, boost::asio::buffer(frame));
    auto response = read_frame(client);
    EXPECT_GT(response.size(), WireHeader::SIZE);

    stop_and_join(server);

    boost::system::error_code ec;
    std::array<uint8_t, 1> buf{};
    client.read_some(boost::asio::buffer(buf), ec);
    EXPECT_TRUE(ec);
}

TEST_F(ServerE2ETest, HeartbeatTimeoutDisconnect)
{
    Server server({
        .heartbeat_timeout_ticks = 3,
        .timer_wheel_slots = 8,
        .handle_signals = false,
        .drain_timeout = std::chrono::seconds{25},
        .cross_core_call_timeout = std::chrono::milliseconds{5000},
        .bump_capacity_bytes = 64 * 1024,
        .arena_block_bytes = 4096,
        .arena_max_bytes = 1024 * 1024,
        .metrics = {},
        .admin = {},
        .affinity = {.enabled = false},
    });
    server.listen<TcpBinaryProtocol>(0);
    server.add_service<TestEchoService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    // Poll for disconnect instead of sleeping a fixed duration (flaky fix)
    auto deadline = std::chrono::steady_clock::now() + 5s;
    boost::system::error_code ec;
    std::array<uint8_t, 1> buf{};
    client.non_blocking(true);
    while (std::chrono::steady_clock::now() < deadline)
    {
        client.read_some(boost::asio::buffer(buf), ec);
        if (ec && ec != boost::asio::error::would_block)
            break;
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, boost::asio::error::would_block);

    stop_and_join(server);
}

TEST_F(ServerE2ETest, HandlerFailedErrorResponse)
{
    Server server({.heartbeat_timeout_ticks = 0,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {},
                   .affinity = {.enabled = false}});
    server.listen<TcpBinaryProtocol>(0);
    server.add_service<ThrowingService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    WireHeader header{.msg_id = 0x0010, .body_size = 0, .reserved = {}};
    auto hdr_bytes = header.serialize();
    boost::asio::write(client, boost::asio::buffer(std::vector<uint8_t>(hdr_bytes.begin(), hdr_bytes.end())));

    auto response = read_frame(client);
    auto resp_header = WireHeader::parse(response);
    ASSERT_TRUE(resp_header.has_value());
    EXPECT_EQ(resp_header->msg_id, 0x0010);
    EXPECT_TRUE(resp_header->flags & wire_flags::ERROR_RESPONSE);

    auto err = flatbuffers::GetRoot<apex::messages::ErrorResponse>(response.data() + WireHeader::SIZE);
    EXPECT_EQ(err->code(), static_cast<uint16_t>(ErrorCode::HandlerException));

    client.close();
    stop_and_join(server);
}

TEST_F(ServerE2ETest, HandlerErrorCodeResponse)
{
    Server server({.heartbeat_timeout_ticks = 0,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {},
                   .affinity = {.enabled = false}});
    server.listen<TcpBinaryProtocol>(0);
    server.add_service<ErrorReturningService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    WireHeader header{.msg_id = 0x0020, .body_size = 0, .reserved = {}};
    auto hdr_bytes = header.serialize();
    boost::asio::write(client, boost::asio::buffer(std::vector<uint8_t>(hdr_bytes.begin(), hdr_bytes.end())));

    auto response = read_frame(client);
    auto resp_header = WireHeader::parse(response);
    ASSERT_TRUE(resp_header.has_value());
    EXPECT_EQ(resp_header->msg_id, 0x0020);
    EXPECT_TRUE(resp_header->flags & wire_flags::ERROR_RESPONSE);

    auto err = flatbuffers::GetRoot<apex::messages::ErrorResponse>(response.data() + WireHeader::SIZE);
    EXPECT_EQ(err->code(), static_cast<uint16_t>(ErrorCode::Timeout));

    client.close();
    stop_and_join(server);
}

TEST_F(ServerE2ETest, ConcurrentMultipleClients)
{
    constexpr int NUM_CLIENTS = 8;

    Server server({
        .num_cores = 2,
        .heartbeat_timeout_ticks = 0,
        .handle_signals = false,
        .drain_timeout = std::chrono::seconds{25},
        .cross_core_call_timeout = std::chrono::milliseconds{5000},
        .bump_capacity_bytes = 64 * 1024,
        .arena_block_bytes = 4096,
        .arena_max_bytes = 1024 * 1024,
        .metrics = {},
        .admin = {},
        .affinity = {.enabled = false},
    });
    server.listen<TcpBinaryProtocol>(0);
    server.add_service<TestEchoService>();
    run_server(server);

    std::atomic<int> success_count{0};
    std::vector<std::string> errors(NUM_CLIENTS);
    std::vector<std::thread> threads;
    threads.reserve(NUM_CLIENTS);

    for (int i = 0; i < NUM_CLIENTS; ++i)
    {
        threads.emplace_back([&, i] {
            try
            {
                boost::asio::io_context ctx;
                auto client = make_client(ctx, server.port());

                // Each client sends its own unique payload
                std::vector<uint8_t> payload = {static_cast<uint8_t>(i), static_cast<uint8_t>(0xA0 + i)};
                auto frame = build_echo_frame(payload);
                boost::asio::write(client, boost::asio::buffer(frame));

                auto response = read_frame(client);
                if (response.size() <= WireHeader::SIZE)
                {
                    errors[i] = "response too small";
                    return;
                }

                auto header = WireHeader::parse(response);
                if (!header.has_value())
                {
                    errors[i] = "failed to parse header";
                    return;
                }

                auto resp = flatbuffers::GetRoot<apex::messages::EchoResponse>(response.data() + WireHeader::SIZE);
                if (!resp->data() || resp->data()->size() != 2)
                {
                    errors[i] = "invalid response data";
                    return;
                }
                if ((*resp->data())[0] != static_cast<uint8_t>(i) ||
                    (*resp->data())[1] != static_cast<uint8_t>(0xA0 + i))
                {
                    errors[i] = "echo payload mismatch";
                    return;
                }

                client.close();
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
            catch (const std::exception& e)
            {
                errors[i] = std::string("exception: ") + e.what();
            }
        });
    }

    for (auto& t : threads)
        t.join();

    for (int i = 0; i < NUM_CLIENTS; ++i)
    {
        EXPECT_TRUE(errors[i].empty()) << "Client " << i << " failed: " << errors[i];
    }
    EXPECT_EQ(success_count.load(), NUM_CLIENTS);

    stop_and_join(server);
}

TEST_F(ServerE2ETest, OversizedBodyDisconnectsSession)
{
    Server server({.heartbeat_timeout_ticks = 0,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {},
                   .affinity = {.enabled = false}});
    server.listen<TcpBinaryProtocol>(0);
    server.add_service<TestEchoService>();
    run_server(server);

    boost::asio::io_context client_ctx;
    auto client = make_client(client_ctx, server.port());

    // body_size를 MAX_BODY_SIZE(16MB)를 초과하는 값으로 설정한 헤더를 전송.
    // WireHeader::parse()에서 BodyTooLarge → FrameCodec에서 FrameError::BodyTooLarge
    // → process_frames()에서 InsufficientData가 아닌 에러이므로 session->close().
    WireHeader header{
        .msg_id = 0x0001,
        .body_size = WireHeader::MAX_BODY_SIZE + 1,
        .reserved = {},
    };
    auto hdr_bytes = header.serialize();
    boost::asio::write(client, boost::asio::buffer(std::vector<uint8_t>(hdr_bytes.begin(), hdr_bytes.end())));

    // 서버가 세션을 끊었으므로 클라이언트에서 EOF/에러가 수신되어야 한다.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    boost::system::error_code ec;
    std::array<uint8_t, 1> buf{};
    client.non_blocking(true);
    while (std::chrono::steady_clock::now() < deadline)
    {
        client.read_some(boost::asio::buffer(buf), ec);
        if (ec && ec != boost::asio::error::would_block)
            break;
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(ec);
    EXPECT_NE(ec, boost::asio::error::would_block);

    stop_and_join(server);
}

TEST_F(ServerE2ETest, GracefulShutdownWithActiveSessions)
{
    constexpr int NUM_CLIENTS = 4;

    Server server({.heartbeat_timeout_ticks = 0,
                   .handle_signals = false,
                   .drain_timeout = std::chrono::seconds{25},
                   .cross_core_call_timeout = std::chrono::milliseconds{5000},
                   .bump_capacity_bytes = 64 * 1024,
                   .arena_block_bytes = 4096,
                   .arena_max_bytes = 1024 * 1024,
                   .metrics = {},
                   .admin = {},
                   .affinity = {.enabled = false}});
    server.listen<TcpBinaryProtocol>(0);
    server.add_service<TestEchoService>();
    run_server(server);

    // Connect clients and send one echo each to ensure sessions are active
    boost::asio::io_context client_ctx;
    std::vector<tcp::socket> clients;
    clients.reserve(NUM_CLIENTS);

    for (int i = 0; i < NUM_CLIENTS; ++i)
    {
        clients.push_back(make_client(client_ctx, server.port()));

        auto frame = build_echo_frame({static_cast<uint8_t>(i)});
        boost::asio::write(clients.back(), boost::asio::buffer(frame));

        auto response = read_frame(clients.back());
        ASSERT_GT(response.size(), WireHeader::SIZE) << "Client " << i << " did not receive echo response";
    }

    // All sessions are active — now call stop() with connections still open
    // The server thread must join without hanging
    auto stop_start = std::chrono::steady_clock::now();
    stop_and_join(server);
    auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;
    EXPECT_LT(stop_elapsed, 5s) << "Server shutdown took too long";

    // Verify server is no longer running
    EXPECT_FALSE(server.running());

    // Verify clients detect the disconnection
    for (int i = 0; i < NUM_CLIENTS; ++i)
    {
        boost::system::error_code ec;
        std::array<uint8_t, 1> buf{};
        clients[i].read_some(boost::asio::buffer(buf), ec);
        EXPECT_TRUE(ec) << "Client " << i << " still connected after shutdown";
    }
}
