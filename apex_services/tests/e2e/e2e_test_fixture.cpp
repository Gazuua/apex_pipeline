// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "e2e_test_fixture.hpp"

#include <flatbuffers/flatbuffers.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace apex::e2e
{

// ---------------------------------------------------------------------------
// E2EConfig factory
// ---------------------------------------------------------------------------

E2EConfig E2EConfig::from_env()
{
    E2EConfig cfg;
    if (auto* v = std::getenv("E2E_GATEWAY_HOST"))
        cfg.gateway_host = v;
    if (auto* v = std::getenv("E2E_GATEWAY_TCP_PORT"))
        cfg.gateway_tcp_port = static_cast<uint16_t>(std::atoi(v));
    if (auto* v = std::getenv("E2E_GATEWAY_WS_PORT"))
        cfg.gateway_ws_port = static_cast<uint16_t>(std::atoi(v));
    if (auto* v = std::getenv("E2E_STARTUP_TIMEOUT"))
        cfg.startup_timeout = std::chrono::seconds{std::atoi(v)};
    if (auto* v = std::getenv("E2E_REQUEST_TIMEOUT"))
        cfg.request_timeout = std::chrono::seconds{std::atoi(v)};
    return cfg;
}

// ---------------------------------------------------------------------------
// E2EEnvironment static members
// ---------------------------------------------------------------------------

E2EConfig E2EEnvironment::config_{};
bool E2EEnvironment::ready_{false};

// ---------------------------------------------------------------------------
// Global environment lifecycle (once for all tests)
// ---------------------------------------------------------------------------

void E2EEnvironment::SetUp()
{
    config_ = E2EConfig::from_env();

    std::cout << "[E2E] Waiting for Gateway at " << config_.gateway_host << ":" << config_.gateway_tcp_port << "...\n";

    auto deadline = std::chrono::steady_clock::now() + config_.startup_timeout;
    bool gateway_ready = false;

    while (std::chrono::steady_clock::now() < deadline)
    {
        try
        {
            boost::asio::io_context probe_ctx;
            boost::asio::ip::tcp::socket probe_sock(probe_ctx);
            boost::asio::ip::tcp::endpoint ep(boost::asio::ip::make_address(config_.gateway_host),
                                              config_.gateway_tcp_port);
            probe_sock.connect(ep);
            probe_sock.close();
            gateway_ready = true;
            break;
        }
        catch (...)
        {
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
    }

    ASSERT_TRUE(gateway_ready) << "Gateway not reachable within timeout";

    // Kafka 파이프라인 warm-up: 로그인 요청을 보내서 실제 응답이 올 때까지 재시도.
    // Kafka consumer rebalance + 토픽 auto-create + response consumer 준비를 모두 확인.
    std::cout << "[E2E] Warming up Kafka pipeline (login probe)...\n";
    auto warmup_deadline = std::chrono::steady_clock::now() + config_.startup_timeout;
    bool pipeline_ready = false;

    while (std::chrono::steady_clock::now() < warmup_deadline)
    {
        try
        {
            boost::asio::io_context warmup_io;
            E2ETestFixture::TcpClient warmup_client(warmup_io, config_);
            warmup_client.connect();

            // LoginRequest: alice@apex.dev / password123
            flatbuffers::FlatBufferBuilder fbb(256);
            auto email_off = fbb.CreateString("alice@apex.dev");
            auto pw_off = fbb.CreateString("password123");
            auto start = fbb.StartTable();
            fbb.AddOffset(4, email_off);
            fbb.AddOffset(6, pw_off);
            auto loc = fbb.EndTable(start);
            fbb.Finish(flatbuffers::Offset<void>(loc));
            warmup_client.send(1000, fbb.GetBufferPointer(), fbb.GetSize());

            auto resp = warmup_client.recv(std::chrono::seconds{5});
            if (resp.msg_id == 1001)
            {
                pipeline_ready = true;
                std::cout << "[E2E] Kafka pipeline ready (login response received).\n";
                break;
            }
        }
        catch (...)
        {
            // 타임아웃 또는 연결 실패 — 재시도
        }
        std::this_thread::sleep_for(std::chrono::seconds{2});
    }

    if (!pipeline_ready)
    {
        std::cerr << "[E2E] Warning: Kafka pipeline warm-up failed. Tests may fail.\n";
    }

    // PubSub + consumer 안정화 추가 대기
    std::this_thread::sleep_for(std::chrono::seconds{3});

    ready_ = true;
    std::cout << "[E2E] Infrastructure ready.\n";
}

void E2EEnvironment::TearDown()
{
    ready_ = false;
    std::cout << "[E2E] Tests complete.\n";
}

// ---------------------------------------------------------------------------
// E2ETestFixture per-test lifecycle
// ---------------------------------------------------------------------------

void E2ETestFixture::SetUp()
{
    ASSERT_TRUE(E2EEnvironment::is_ready()) << "E2E infrastructure not ready — global SetUp failed";
}

void E2ETestFixture::TearDown()
{
    // Per-test cleanup (connections auto-closed by TcpClient destructor)
}

// ---------------------------------------------------------------------------
// TcpClient
// ---------------------------------------------------------------------------

E2ETestFixture::TcpClient::TcpClient(boost::asio::io_context& io_ctx, const E2EConfig& config)
    : io_ctx_(io_ctx)
    , config_(config)
    , socket_(io_ctx)
{}

E2ETestFixture::TcpClient::~TcpClient()
{
    if (connected_)
    {
        try
        {
            close();
        }
        catch (...)
        {}
    }
}

void E2ETestFixture::TcpClient::connect()
{
    boost::asio::ip::tcp::resolver resolver(io_ctx_);
    auto endpoints = resolver.resolve(config_.gateway_host, std::to_string(config_.gateway_tcp_port));
    boost::asio::connect(socket_, endpoints);
    connected_ = true;
}

void E2ETestFixture::TcpClient::send(uint32_t msg_id, const uint8_t* payload, size_t size)
{
    if (!connected_)
    {
        throw std::runtime_error("TcpClient::send() called on disconnected socket");
    }

    WireHeader hdr{};
    hdr.version = WireHeader::CURRENT_VERSION;
    hdr.flags = 0;
    hdr.msg_id = msg_id;
    hdr.body_size = static_cast<uint32_t>(size);

    std::array<uint8_t, WireHeader::SIZE> hdr_buf{};
    hdr.serialize(std::span<uint8_t, WireHeader::SIZE>(hdr_buf));

    boost::asio::write(socket_, boost::asio::buffer(hdr_buf));
    if (size > 0 && payload != nullptr)
    {
        boost::asio::write(socket_, boost::asio::buffer(payload, size));
    }
}

E2ETestFixture::TcpClient::Response E2ETestFixture::TcpClient::recv(std::chrono::seconds timeout)
{
    if (!connected_)
    {
        throw std::runtime_error("TcpClient::recv() called on disconnected socket");
    }

#ifdef _WIN32
    DWORD timeout_ms = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
    ::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
                 sizeof(timeout_ms));
#else
    struct timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count());
    tv.tv_usec = 0;
    ::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    std::array<uint8_t, WireHeader::SIZE> hdr_buf{};
    boost::asio::read(socket_, boost::asio::buffer(hdr_buf));

    auto hdr_result = WireHeader::parse(std::span<const uint8_t>(hdr_buf.data(), hdr_buf.size()));
    if (!hdr_result.has_value())
    {
        throw std::runtime_error("Failed to parse WireHeader in response");
    }
    auto& hdr = hdr_result.value();

    Response resp;
    resp.msg_id = hdr.msg_id;
    resp.flags = hdr.flags;
    if (hdr.body_size > 0)
    {
        resp.payload.resize(hdr.body_size);
        boost::asio::read(socket_, boost::asio::buffer(resp.payload.data(), resp.payload.size()));
    }

    return resp;
}

void E2ETestFixture::TcpClient::close()
{
    if (connected_)
    {
        boost::system::error_code ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        connected_ = false;
    }
}

// ---------------------------------------------------------------------------
// Auth helpers
// ---------------------------------------------------------------------------

E2ETestFixture::AuthResult E2ETestFixture::login(TcpClient& client, const std::string& email,
                                                 const std::string& password)
{
    flatbuffers::FlatBufferBuilder fbb(256);

    auto email_off = fbb.CreateString(email);
    auto pw_off = fbb.CreateString(password);

    auto start = fbb.StartTable();
    fbb.AddOffset(4, email_off);
    fbb.AddOffset(6, pw_off);
    auto loc = fbb.EndTable(start);
    fbb.Finish(flatbuffers::Offset<void>(loc));

    client.send(1000, fbb.GetBufferPointer(), fbb.GetSize());

    auto resp = client.recv(std::chrono::seconds{static_cast<long>(config_.request_timeout.count())});

    std::cerr << "[E2E-DEBUG] login() recv: msg_id=" << resp.msg_id << " payload_size=" << resp.payload.size();
    if (resp.msg_id != 1001 && !resp.payload.empty())
    {
        std::cerr << " payload_hex=";
        for (size_t i = 0; i < std::min(resp.payload.size(), size_t{16}); ++i)
        {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x", resp.payload[i]);
            std::cerr << buf;
        }
    }
    std::cerr << "\n";

    AuthResult result{};
    if (resp.msg_id == 1001 && !resp.payload.empty())
    {
        auto* root = flatbuffers::GetRoot<flatbuffers::Table>(resp.payload.data());
        if (root)
        {
            auto* at = root->GetPointer<const flatbuffers::String*>(6);
            if (at)
                result.access_token = at->str();

            auto* rt = root->GetPointer<const flatbuffers::String*>(8);
            if (rt)
                result.refresh_token = rt->str();

            result.user_id = root->GetField<uint64_t>(10, 0);
            result.expires_in_sec = root->GetField<uint32_t>(12, 0);
        }
    }

    return result;
}

void E2ETestFixture::authenticate(TcpClient& client, const std::string& token)
{
    flatbuffers::FlatBufferBuilder fbb(static_cast<size_t>(256) + token.size());
    auto token_off = fbb.CreateString(token);
    auto start = fbb.StartTable();
    fbb.AddOffset(4, token_off);
    auto loc = fbb.EndTable(start);
    fbb.Finish(flatbuffers::Offset<void>(loc));

    client.send(3 /* AuthenticateSession */, fbb.GetBufferPointer(), fbb.GetSize());

    // Gateway handles msg_id=3 inline (binds JWT to session) and returns ok()
    // without sending a response.  No recv() needed — the Gateway's read_loop
    // processes frames sequentially, so the next send() is guaranteed to see
    // the bound JWT even without a round-trip wait.
    //
    // Brief sleep to yield and let the Gateway's io_context process the frame.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
}

// ---------------------------------------------------------------------------
// Channel subscription helpers
// ---------------------------------------------------------------------------

void E2ETestFixture::subscribe_channel(TcpClient& client, const std::string& channel)
{
    flatbuffers::FlatBufferBuilder fbb(static_cast<size_t>(64) + channel.size());
    auto ch_off = fbb.CreateString(channel);
    auto start = fbb.StartTable();
    fbb.AddOffset(4, ch_off);
    auto loc = fbb.EndTable(start);
    fbb.Finish(flatbuffers::Offset<void>(loc));

    client.send(4 /* SubscribeChannel */, fbb.GetBufferPointer(), fbb.GetSize());
}

void E2ETestFixture::unsubscribe_channel(TcpClient& client, const std::string& channel)
{
    flatbuffers::FlatBufferBuilder fbb(static_cast<size_t>(64) + channel.size());
    auto ch_off = fbb.CreateString(channel);
    auto start = fbb.StartTable();
    fbb.AddOffset(4, ch_off);
    auto loc = fbb.EndTable(start);
    fbb.Finish(flatbuffers::Offset<void>(loc));

    client.send(5 /* UnsubscribeChannel */, fbb.GetBufferPointer(), fbb.GetSize());
}

} // namespace apex::e2e

// ---------------------------------------------------------------------------
// Custom main — register global E2E environment
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new apex::e2e::E2EEnvironment);
    return RUN_ALL_TESTS();
}
