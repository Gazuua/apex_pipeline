// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/metrics_http_server.hpp>
#include <apex/core/metrics_registry.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace apex::core;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace
{

/// 별도 io_context에서 raw TCP 소켓으로 HTTP 요청을 보내고 전체 응답 문자열을 반환.
std::string send_http_request(uint16_t port, const std::string& method, const std::string& path)
{
    net::io_context client_io;
    tcp::socket sock(client_io);
    sock.connect(tcp::endpoint(net::ip::address_v4::loopback(), port));

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    net::write(sock, net::buffer(request));

    // 서버가 shutdown_send를 하므로 EOF까지 읽음
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

/// HTTP 응답 문자열에서 상태 코드(숫자)를 추출. 파싱 실패 시 0 반환.
int extract_status_code(const std::string& response)
{
    // "HTTP/1.1 200 OK\r\n..." 형태에서 200 추출
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

/// HTTP 응답에서 body 부분을 추출 (\r\n\r\n 이후).
std::string extract_body(const std::string& response)
{
    auto pos = response.find("\r\n\r\n");
    if (pos == std::string::npos)
        return {};
    return response.substr(pos + 4);
}

/// HTTP 응답에서 특정 헤더 값을 추출 (대소문자 무시하지 않음).
std::string extract_header(const std::string& response, const std::string& header_name)
{
    std::string search = header_name + ": ";
    auto pos = response.find(search);
    if (pos == std::string::npos)
        return {};
    auto start = pos + search.size();
    auto end = response.find("\r\n", start);
    if (end == std::string::npos)
        return response.substr(start);
    return response.substr(start, end - start);
}

class MetricsHttpServerTest : public ::testing::Test
{
  protected:
    net::io_context io_ctx_;
    MetricsRegistry registry_;
    std::atomic<bool> running_{true};
    MetricsHttpServer server_;
    uint16_t port_{0};
    std::jthread server_thread_;

    void SetUp() override
    {
        server_.start(io_ctx_, 0, registry_, running_);
        port_ = server_.local_port();
        ASSERT_NE(port_, 0);
        server_thread_ = std::jthread([this] { io_ctx_.run(); });
    }

    void TearDown() override
    {
        server_.stop();
        io_ctx_.stop();
    }
};

} // anonymous namespace

TEST_F(MetricsHttpServerTest, MetricsEndpointReturnsPrometheusFormat)
{
    // 레지스트리에 카운터를 하나 등록해 serialize 결과가 비지 않도록
    auto& c = registry_.counter("test_total", "A test counter");
    c.increment(7);

    auto response = send_http_request(port_, "GET", "/metrics");
    EXPECT_EQ(extract_status_code(response), 200);

    auto content_type = extract_header(response, "Content-Type");
    EXPECT_NE(content_type.find("text/plain"), std::string::npos);

    auto body = extract_body(response);
    EXPECT_NE(body.find("test_total 7"), std::string::npos);
}

TEST_F(MetricsHttpServerTest, HealthEndpointReturnsOk)
{
    auto response = send_http_request(port_, "GET", "/health");
    EXPECT_EQ(extract_status_code(response), 200);

    auto body = extract_body(response);
    EXPECT_NE(body.find("OK"), std::string::npos);
}

TEST_F(MetricsHttpServerTest, ReadyEndpointReflectsRunningFlag)
{
    // running_ = true (SetUp 기본값) → 200
    auto response = send_http_request(port_, "GET", "/ready");
    EXPECT_EQ(extract_status_code(response), 200);

    auto body = extract_body(response);
    EXPECT_NE(body.find("READY"), std::string::npos);

    // running_ = false → 503
    running_.store(false, std::memory_order_release);
    response = send_http_request(port_, "GET", "/ready");
    EXPECT_EQ(extract_status_code(response), 503);

    body = extract_body(response);
    EXPECT_NE(body.find("NOT READY"), std::string::npos);
}

TEST_F(MetricsHttpServerTest, UnknownPathReturns404)
{
    auto response = send_http_request(port_, "GET", "/invalid");
    EXPECT_EQ(extract_status_code(response), 404);

    auto body = extract_body(response);
    EXPECT_NE(body.find("Not Found"), std::string::npos);
}

TEST_F(MetricsHttpServerTest, UnsupportedMethodReturns404)
{
    // 현재 서버는 메서드를 분기하지 않고 GET+path 매칭만 하므로 POST → else → 404
    auto response = send_http_request(port_, "POST", "/metrics");
    EXPECT_EQ(extract_status_code(response), 404);
}

TEST_F(MetricsHttpServerTest, ConcurrentConnections)
{
    constexpr int kThreads = 5;
    std::vector<int> results(kThreads, 0);
    std::vector<std::jthread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([this, &results, i] {
            auto response = send_http_request(port_, "GET", "/health");
            results[static_cast<size_t>(i)] = extract_status_code(response);
        });
    }

    // jthread 소멸자가 join 수행
    threads.clear();

    for (int i = 0; i < kThreads; ++i)
    {
        EXPECT_EQ(results[static_cast<size_t>(i)], 200) << "Thread " << i << " failed";
    }
}
