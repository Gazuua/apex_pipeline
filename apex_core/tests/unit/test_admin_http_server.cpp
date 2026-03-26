// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/admin_http_server.hpp>
#include <apex/core/logging.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <thread>

using namespace apex::core;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace
{

std::string send_http_request(uint16_t port, const std::string& method, const std::string& path)
{
    net::io_context client_io;
    tcp::socket sock(client_io);
    sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port));

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    net::write(sock, net::buffer(request));

    std::string response;
    boost::system::error_code ec;
    for (;;)
    {
        char buf[1024];
        auto n = sock.read_some(net::buffer(buf), ec);
        if (n > 0)
            response.append(buf, n);
        if (ec)
            break;
    }
    return response;
}

int extract_status_code(const std::string& response)
{
    auto pos = response.find(' ');
    if (pos == std::string::npos)
        return 0;
    try
    {
        return std::stoi(response.substr(pos + 1));
    }
    catch (...)
    {
        return 0;
    }
}

std::string extract_body(const std::string& response)
{
    auto pos = response.find("\r\n\r\n");
    if (pos == std::string::npos)
        return {};
    return response.substr(pos + 4);
}

class AdminHttpServerTest : public ::testing::Test
{
  protected:
    net::io_context io_ctx_;
    AdminHttpServer server_;
    uint16_t port_{0};
    std::jthread server_thread_;

    void SetUp() override
    {
        // Ensure spdlog loggers exist for log-level tests
        if (!spdlog::get("apex"))
        {
            auto apex = std::make_shared<spdlog::logger>("apex");
            apex->set_level(spdlog::level::info);
            spdlog::register_logger(apex);
        }
        if (!spdlog::get("app"))
        {
            auto app = std::make_shared<spdlog::logger>("app");
            app->set_level(spdlog::level::info);
            spdlog::register_logger(app);
        }

        server_.start(io_ctx_, 0);
        port_ = server_.local_port();
        ASSERT_NE(port_, 0);
        server_thread_ = std::jthread([this] { io_ctx_.run(); });
    }

    void TearDown() override
    {
        server_.stop();
        io_ctx_.stop();
        // Restore default levels
        if (auto l = spdlog::get("apex"))
            l->set_level(spdlog::level::info);
        if (auto l = spdlog::get("app"))
            l->set_level(spdlog::level::info);
    }
};

} // anonymous namespace

TEST_F(AdminHttpServerTest, GetLogLevel_SingleLogger)
{
    auto response = send_http_request(port_, "GET", "/admin/log-level?logger=apex");
    EXPECT_EQ(extract_status_code(response), 200);

    auto body = extract_body(response);
    EXPECT_NE(body.find("\"apex\""), std::string::npos);
    EXPECT_NE(body.find("\"info\""), std::string::npos);
}

TEST_F(AdminHttpServerTest, GetLogLevel_AllLoggers)
{
    auto response = send_http_request(port_, "GET", "/admin/log-level");
    EXPECT_EQ(extract_status_code(response), 200);

    auto body = extract_body(response);
    EXPECT_NE(body.find("\"apex\""), std::string::npos);
    EXPECT_NE(body.find("\"app\""), std::string::npos);
}

TEST_F(AdminHttpServerTest, PostLogLevel_ChangeLevel)
{
    auto response = send_http_request(port_, "POST", "/admin/log-level?logger=apex&level=debug");
    EXPECT_EQ(extract_status_code(response), 200);

    auto body = extract_body(response);
    EXPECT_NE(body.find("\"debug\""), std::string::npos);
    EXPECT_NE(body.find("\"previous\":\"info\""), std::string::npos);

    // Verify the level actually changed
    EXPECT_EQ(spdlog::get("apex")->level(), spdlog::level::debug);
}

TEST_F(AdminHttpServerTest, PostLogLevel_UnknownLogger)
{
    auto response = send_http_request(port_, "POST", "/admin/log-level?logger=nonexistent&level=debug");
    EXPECT_EQ(extract_status_code(response), 400);

    auto body = extract_body(response);
    EXPECT_NE(body.find("unknown logger"), std::string::npos);
}

TEST_F(AdminHttpServerTest, PostLogLevel_InvalidLevel)
{
    auto response = send_http_request(port_, "POST", "/admin/log-level?logger=apex&level=banana");
    EXPECT_EQ(extract_status_code(response), 400);

    auto body = extract_body(response);
    EXPECT_NE(body.find("invalid level"), std::string::npos);
}

TEST_F(AdminHttpServerTest, PostLogLevel_MissingLogger)
{
    auto response = send_http_request(port_, "POST", "/admin/log-level?level=debug");
    EXPECT_EQ(extract_status_code(response), 400);

    auto body = extract_body(response);
    EXPECT_NE(body.find("missing"), std::string::npos);
}

TEST_F(AdminHttpServerTest, PostLogLevel_MissingLevel)
{
    auto response = send_http_request(port_, "POST", "/admin/log-level?logger=apex");
    EXPECT_EQ(extract_status_code(response), 400);

    auto body = extract_body(response);
    EXPECT_NE(body.find("missing"), std::string::npos);
}

TEST_F(AdminHttpServerTest, UnknownPath_Returns404)
{
    auto response = send_http_request(port_, "GET", "/admin/unknown");
    EXPECT_EQ(extract_status_code(response), 404);
}

TEST_F(AdminHttpServerTest, DeleteMethod_Returns405)
{
    auto response = send_http_request(port_, "DELETE", "/admin/log-level");
    EXPECT_EQ(extract_status_code(response), 405);
}
